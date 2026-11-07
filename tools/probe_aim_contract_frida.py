"""Capture loaded weapon/camera method metadata; never invokes shooting or changes aim."""
import argparse
import json
import time
from pathlib import Path

import frida


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trace-seconds', type=float, default=0,
                        help='Temporarily trace native GetShootDir calls without changing their result (max 10 seconds)')
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    probe = '''
import sys, json, marshal
rows = []
modules = []
code_type = type((lambda: None).__code__)
terms = ('shoot', 'fire', 'bullet', 'recoil', 'direction', 'aimpos', 'aimpoint', 'ray', 'dir')
for modname, module in list(sys.modules.items()):
    if not modname.startswith('gclient.'):
        continue
    if not any(word in modname for word in ('equips.', 'bullet', 'shoot', 'camera.', 'spell', 'weapon')):
        continue
    modules.append(modname)
    for name, value in list(vars(module).items()):
        groups = [(name, value)]
        if isinstance(value, type) and getattr(value, '__module__', '') == modname:
            groups += [(name + '.' + key, item) for key, item in list(vars(value).items())]
        if modname.endswith('.gun_spell'):
            extra = []
            for label, item in groups:
                if getattr(item, '__code__', None) is None and not isinstance(item, type):
                    attributes = getattr(item, '__dict__', None) or {}
                    if hasattr(attributes, 'items'):
                        extra += [(label + '.' + key, fn) for key, fn in list(attributes.items())]
            groups += extra
        for label, fn in groups:
            if isinstance(fn, (staticmethod, classmethod)):
                fn = fn.__func__
            code = getattr(fn, '__code__', None)
            if not isinstance(code, code_type) or (not modname.endswith('.gun_spell') and not any(word in label.lower() for word in terms)):
                continue
            rows.append({'module': modname, 'label': label,
                         'filename': code.co_filename, 'line': code.co_firstlineno,
                         'args': list(code.co_varnames[:code.co_argcount + code.co_kwonlyargcount]),
                         'names': list(code.co_names), 'code': marshal.dumps(code).hex()})
with open(OUTPUT, 'w', encoding='utf-8') as handle:
    json.dump({'modules': sorted(modules), 'methods': rows}, handle)
'''.replace('OUTPUT', repr(str(output)))
    # A separate globals dictionary leaves the game's module globals untouched.
    guarded = ('try:\n' + '\n'.join('    ' + line for line in probe.splitlines()) +
               '\nexcept BaseException:\n    import traceback\n    with open(%r, "w", encoding="utf-8") as error_file:\n        error_file.write(traceback.format_exc())\n    raise\n' % str(output.with_suffix('.error.txt')))
    source = 'exec(compile(%r, "<comet-aim-contract-probe>", "exec"), {"__name__": "__comet_probe__"})' % guarded
    js = '''
const base = Process.getModuleByName('BloodStrike.exe').base;
const specs = [
 [0x2adc1c0, '48895c2410574883ec20488d0d'],
 [0x2adc290, '48895c2410574883ec208bf9488d0d'],
 [0x2a9dee0, '48895c24084889742410574883ec40488bf1488bda']
];
for (const [rva, expected] of specs) {
 const bytes = Array.from(new Uint8Array(base.add(rva).readByteArray(expected.length / 2)))
  .map(v => v.toString(16).padStart(2, '0')).join('');
 if (bytes !== expected) throw new Error('Python ABI signature mismatch at ' + rva.toString(16));
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
    session = frida.attach(args.pid)
    try:
        script = session.create_script(js)
        script.load()
        result = script.exports_sync.collect(source)
        if result != 0:
            raise RuntimeError('Embedded Python metadata capture failed: %d' % result)
        data = json.loads(output.read_text('utf-8'))
        print('Captured %d methods from %d modules to %s' %
              (len(data['methods']), len(data['modules']), output))
        if args.trace_seconds > 0:
            install = '''
import builtins, sys
gun = sys.modules['gclient.gameplay.logic_base.spell.spell_core.gun_spell']
cls = gun.SpellWorker
original = cls.GetShootDir
events = []
def trace(worker, shoot_pos):
    before = None
    try:
        camera = gun.MEngine.GetGameplay().Player.Camera
        ray = camera.GetRayDirectionFromScreenPoint(int(shoot_pos[0]), int(shoot_pos[1]))
        before = (ray.x, ray.y, ray.z)
    except Exception:
        pass
    result = original(worker, shoot_pos)
    try:
        if len(events) < 16:
            after = camera.GetRayDirectionFromScreenPoint(int(shoot_pos[0]), int(shoot_pos[1]))
            events.append({'local': worker.caster is gun.genv.player,
                           'direction': (result.x, result.y, result.z),
                           'camera_before': before, 'camera_after': (after.x, after.y, after.z)})
    except Exception:
        pass
    return result
cls.GetShootDir = trace
builtins._comet_shot_contract_trace = (cls, original, trace, events)
'''
            cleanup = '''
import builtins, json
state = getattr(builtins, '_comet_shot_contract_trace', None)
if state is not None:
    cls, original, trace, events = state
    restored = cls.GetShootDir is trace
    if restored:
        cls.GetShootDir = original
    with open(OUTPUT, 'w', encoding='utf-8') as handle:
        json.dump({'events': events, 'restored': restored}, handle)
    del builtins._comet_shot_contract_trace
'''.replace('OUTPUT', repr(str(output.with_suffix('.trace.json'))))
            def execute(code):
                result = script.exports_sync.collect('exec(%r, {"__name__": "__comet_trace__"})' % code)
                if result:
                    raise RuntimeError('Shot trace Python call failed: %d' % result)
            try:
                execute(install)
                time.sleep(min(10., args.trace_seconds))
            finally:
                execute(cleanup)
            trace_data = json.loads(output.with_suffix('.trace.json').read_text('utf-8'))
            print('Native shot calls observed: %d; original restored: %s' %
                  (len(trace_data['events']), trace_data['restored']))
    finally:
        session.detach()


if __name__ == '__main__':
    main()
