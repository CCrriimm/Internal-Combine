"""Snapshot player fields through the existing, signature-checked Python bridge."""
import argparse
import ast
import json
from pathlib import Path

import frida


PROBE = r'''
import builtins, json, time
g = getattr(builtins, 'genv', None)
space = getattr(g, 'space', None)
player = getattr(g, 'player', None)
def read(obj, name):
    try:
        value = getattr(obj, name)
        if value is None or isinstance(value, (str, bool, int, float)):
            return value
        if isinstance(value, (tuple, list)):
            return list(value)
        if all(hasattr(value, k) for k in ('x', 'y', 'z')):
            return [value.x, value.y, value.z]
        return {'type': type(value).__name__}
    except Exception as error:
        return {'error': type(error).__name__}
fields = ('id', 'combat_team_id', 'team_id', 'hp', 'server_hp', 'cur_maxhp',
          'max_hp', 'maxhp', 'armor', 'server_armor', 'combat_state',
          'position', 'pos', 'IsPlayerCombatAvatar', 'IsRobotCombatAvatar',
          'IsDummyCombatAvatar', 'IsCombatAvatar')
rows, seen, sources = [], set(), {}
for name in ('combat_avatars', 'entities'):
    collection = getattr(space, name, None)
    sources[name] = {'type': type(collection).__name__}
    try:
        values = list(collection.values())
        sources[name]['count'] = len(values)
    except Exception as error:
        sources[name]['error'] = type(error).__name__
        continue
    for entity in values:
        if id(entity) in seen:
            continue
        seen.add(id(entity))
        row = {'type': type(entity).__name__, 'source': name,
               'local': entity is player, 'fields': {k: read(entity, k) for k in fields}}
        rows.append(row)
result = {'captured_at_unix': time.time(), 'in_hall': read(g, 'is_in_hall'),
          'space_type': type(space).__name__, 'sources': sources,
          'game_logic_type': type(getattr(space, 'game_logic', None)).__name__,
          'local_player_type': type(player).__name__,
          'local_player': {k: read(player, k) for k in fields}, 'entities': rows}
VERIFY
with open(OUTPUT, 'w', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, ensure_ascii=False)
'''

BRIDGE = r'''
const base = Process.getModuleByName('BloodStrike.exe').base;
const specs = [
 [0x2adc1c0, '48895c2410574883ec20488d0d'],
 [0x2adc290, '48895c2410574883ec208bf9488d0d'],
 [0x2a9dee0, '48895c24084889742410574883ec40488bf1488bda']
];
for (const [rva, expected] of specs) {
 const got = Array.from(new Uint8Array(base.add(rva).readByteArray(expected.length / 2)))
  .map(v => v.toString(16).padStart(2, '0')).join('');
 if (got !== expected) throw new Error('Python ABI signature mismatch: ' + rva.toString(16));
}
rpc.exports.collect = function(source) {
 const ensure = new NativeFunction(base.add(specs[0][0]), 'int', []);
 const release = new NativeFunction(base.add(specs[1][0]), 'void', ['int']);
 const run = new NativeFunction(base.add(specs[2][0]), 'int', ['pointer', 'pointer']);
 const text = Memory.allocUtf8String(source);
 const state = ensure();
 try { return run(text, ptr(0)); } finally { release(state); }
};
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-source', type=Path,
                        help='Evaluate player-field helpers against live entities without starting the runtime')
    parser.add_argument('--apply-fix', action='store_true',
                        help='Replace only _local, _is_combat and _max_hp in an already loaded runtime')
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise FileExistsError('Use a new output path to preserve previous evidence')
    if args.apply_fix and args.runtime_source is None:
        parser.error('--apply-fix requires --runtime-source')
    verify = ''
    if args.runtime_source:
        text = args.runtime_source.read_text(encoding='utf-8-sig')
        tree = ast.parse(text)
        names = {'_call_attr', '_entity_flag', '_normalize_team_id', '_team_id',
                 '_is_vehicle', '_is_skill_object', '_is_combat', '_max_hp', '_local',
                 '_check_faction_relation'}
        definitions = {n.name: ast.get_source_segment(text, n) for n in tree.body
                       if isinstance(n, ast.FunctionDef) and n.name in names}
        if definitions.keys() != names:
            raise ValueError('Runtime helper definitions are incomplete')
        verify = '''
import sys
ns = {'genv': g, 'time': time, '_local_cache': None,
      '_vec': lambda value: tuple(value) if value is not None else None}
definitions = DEFINITIONS
for code in definitions.values():
    exec(compile(code, '<player-field-check>', 'exec'), ns)
av, local_id, local_team, local_pos = ns['_local']()
checks = []
for entity in list((getattr(space, 'combat_avatars', None) or {}).values()):
    checks.append({'id': read(entity, 'id'), 'type': type(entity).__name__,
                   'eligible': ns['_is_combat'](entity), 'max_hp': ns['_max_hp'](entity),
                   'team': ns['_team_id'](entity), 'self': entity is av,
                   'enemy': ns['_check_faction_relation'](entity, av, local_team,
                              ns['_team_id'](entity), False, False)})
loaded = []
seen_modules = set()
for name in ('m_view_pipeline', 'bs_runtime'):
    module = sys.modules.get(name)
    if module is None or id(module) in seen_modules:
        continue
    seen_modules.add(id(module))
    entry = {'name': name, 'file': 'pyexec/bs_runtime.py', 'applied': False}
    if APPLY_FIX:
        if not all(callable(getattr(module, key, None)) for key in ('_local', '_is_combat', '_max_hp')):
            raise RuntimeError('Loaded runtime lacks expected helper functions')
        for key in ('_local', '_is_combat', '_max_hp'):
            exec(compile(definitions[key], '<player-field-fix>', 'exec'), vars(module))
        module._local_cache = None
        module._ent_list_cache = None
        entry['applied'] = True
    if all(callable(getattr(module, key, None)) for key in ('_local', '_is_combat', '_max_hp')):
        entry['verification'] = [
            {'id': read(entity, 'id'), 'eligible': module._is_combat(entity),
             'max_hp': module._max_hp(entity)}
            for entity in list((getattr(space, 'combat_avatars', None) or {}).values())]
        current_local = module._local()
        entry['local_id'] = current_local[1]
        entry['local_team'] = current_local[2]
    loaded.append(entry)
result['verification'] = {'local_id': local_id, 'local_team': local_team,
                          'entities': checks, 'loaded_runtimes': loaded}
'''.replace('DEFINITIONS', repr(definitions)).replace('APPLY_FIX', repr(args.apply_fix))
    source = PROBE.replace('VERIFY', verify).replace('OUTPUT', repr(str(output)))
    compile(source, '<custom-room-probe>', 'exec')
    command = 'exec(compile(%r, "<custom-room-probe>", "exec"), {"__name__": "__room_probe__"})' % source
    session = frida.attach(args.pid)
    try:
        script = session.create_script(BRIDGE)
        script.load()
        result = script.exports_sync.collect(command)
        if result:
            raise RuntimeError('Python snapshot failed: %d' % result)
        data = json.loads(output.read_text(encoding='utf-8'))
        summary = {k: v for k, v in data.items() if k not in ('entities', 'local_player', 'verification')}
        if 'verification' in data:
            check = data['verification']
            summary['verification'] = {'local_id': check['local_id'],
                'eligible': sum(e['eligible'] for e in check['entities']),
                'enemies': sum(e['enemy'] and not e['self'] for e in check['entities']),
                'loaded_runtimes': [{k: v for k, v in module.items() if k != 'verification'}
                                    for module in check['loaded_runtimes']]}
        print(json.dumps(summary, indent=2))
        print('Captured %d entities to %s' % (len(data['entities']), output))
    finally:
        session.detach()


if __name__ == '__main__':
    main()
