"""Discover loaded recoil functions and optionally observe Python call events."""
import argparse
import json
import time
from pathlib import Path

import frida
from probe_custom_room import BRIDGE


PROBE = r'''
import builtins, dis, io, json, sys, time
g = getattr(builtins, 'genv', None)
camera = getattr(g, 'camera', None)
placer = getattr(camera, 'placer', None)
code_type = type((lambda: None).__code__)
function_type = type(lambda: None)
terms = TERMS
rows, codes, modules = [], {}, []
def consider(label, fn, depth=0):
    if isinstance(fn, (staticmethod, classmethod)):
        fn = fn.__func__
    if isinstance(fn, property):
        fn = fn.fget
    elif type(fn).__name__ == 'cached_property':
        fn = fn.func
    code = getattr(fn, '__code__', None)
    if not isinstance(code, code_type):
        return
    tokens = [label] + list(code.co_names) + [x for x in code.co_consts if isinstance(x, str)]
    if not any(any(term in token.lower() for term in terms) for token in tokens):
        return
    if id(code) not in codes:
        output = io.StringIO()
        try:
            dis.dis(code, file=output, depth=0)
        except Exception as error:
            output.write('Disassembly failed: ' + type(error).__name__)
        row = {'label': label, 'function_module': getattr(fn, '__module__', None),
               'qualname': code.co_qualname, 'filename': code.co_filename,
               'line': code.co_firstlineno,
               'args': list(code.co_varnames[:code.co_argcount + code.co_kwonlyargcount]),
               'names': list(code.co_names), 'disassembly': output.getvalue(), 'aliases': []}
        rows.append(row)
        codes[id(code)] = (code, row)
    else:
        codes[id(code)][1]['aliases'].append(label)
    if depth < 2:
        for index, cell in enumerate(getattr(fn, '__closure__', None) or ()):
            try:
                original = cell.cell_contents
            except ValueError:
                continue
            if isinstance(original, function_type):
                consider(label + '.closure' + str(index), original, depth + 1)
for modname, module in list(sys.modules.items()):
    if module is None or not modname.startswith(('gclient.', 'gshare.')):
        continue
    modules.append(modname)
    for name, value in list(vars(module).items()):
        consider(modname + '.' + name, value)
        if isinstance(value, type) and getattr(value, '__module__', None) == modname:
            for key, fn in list(vars(value).items()):
                consider(modname + '.' + name + '.' + key, fn)
fields = ('state', 'shoot_idx', 'RecoilEnable', 'RecoilPitchRange', 'RecoilYawRange',
          'RecoilUpTime', 'RecoilFallPct', 'RecoilStaticTime', 'RecoilDownTime',
          'RecoilDownPct', 'RecoilDownCycleIdx', 'RecoilDownCyclePct', 'forcePersistRecoilRecover')
values = {}
for name in fields:
    try:
        value = getattr(placer, name)
        if value is None or isinstance(value, (bool, str, int, float)):
            values[name] = value
    except Exception:
        pass
result = {'captured_at_unix': time.time(), 'in_hall': getattr(g, 'is_in_hall', None),
          'player_type': type(getattr(g, 'player', None)).__name__,
          'placer_type': type(placer).__name__, 'placer_fields': values,
          'scanned_modules': len(modules), 'methods': rows}
if TRACE:
    key = '_comet_recoil_observe'
    if hasattr(builtins, key):
        raise RuntimeError('A recoil observation is already active')
    tool_id = next((i for i in (3, 4, 5) if sys.monitoring.get_tool(i) is None), None)
    if tool_id is None:
        raise RuntimeError('No free Python monitoring tool ID')
    sys.monitoring.use_tool_id(tool_id, 'comet-recoil-observe')
    state = {'tool_id': tool_id, 'codes': [], 'events': [], 'counts': {}, 'labels': codes}
    setattr(builtins, key, state)
    def on_start(code, offset):
        label = state['labels'][id(code)][1]['label']
        state['counts'][label] = state['counts'].get(label, 0) + 1
        if len(state['events']) < 256:
            state['events'].append({'time': time.time(), 'function': label})
    sys.monitoring.register_callback(tool_id, sys.monitoring.events.PY_START, on_start)
    for code, row in codes.values():
        state['codes'].append(code)
        sys.monitoring.set_local_events(tool_id, code, sys.monitoring.events.PY_START)
    result['monitoring'] = {'tool_id': tool_id, 'codes': len(codes)}
with open(OUTPUT, 'w', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, ensure_ascii=False)
'''

CLEANUP = r'''
import builtins, json, sys, time
state = getattr(builtins, '_comet_recoil_observe', None)
if state is not None:
    tool_id = state['tool_id']
    for code in state['codes']:
        sys.monitoring.set_local_events(tool_id, code, 0)
    sys.monitoring.register_callback(tool_id, sys.monitoring.events.PY_START, None)
    sys.monitoring.free_tool_id(tool_id)
    result = {'finished_at_unix': time.time(), 'counts': state['counts'],
              'events': state['events'], 'monitoring_removed': sys.monitoring.get_tool(tool_id) is None}
    delattr(builtins, '_comet_recoil_observe')
    with open(OUTPUT, 'w', encoding='utf-8') as handle:
        json.dump(result, handle, indent=2)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trace-seconds', type=float, default=0)
    parser.add_argument('--terms', nargs='+', default=['recoil', 'rollshake', 'kickback'])
    args = parser.parse_args()
    if not 0 <= args.trace_seconds <= 60:
        parser.error('--trace-seconds must be between 0 and 60')
    output = args.output.resolve()
    trace_output = output.with_suffix('.trace.json')
    if output.exists() or trace_output.exists():
        raise FileExistsError('Use a new output path to preserve evidence')
    output.parent.mkdir(parents=True, exist_ok=True)
    source = PROBE.replace('TERMS', repr(tuple(args.terms))).replace('TRACE', repr(bool(args.trace_seconds))).replace('OUTPUT', repr(str(output)))
    compile(source, '<recoil-metadata>', 'exec')
    session = frida.attach(args.pid)
    try:
        script = session.create_script(BRIDGE)
        script.load()

        def execute(code):
            command = 'exec(compile(%r, "<recoil-probe>", "exec"), {"__name__": "__recoil_probe__"})' % code
            result = script.exports_sync.collect(command)
            if result:
                raise RuntimeError('Embedded Python probe failed: %d' % result)

        try:
            execute(source)
            data = json.loads(output.read_text(encoding='utf-8'))
            print('Captured %d unique functions from %d modules; player=%s, placer=%s' %
                  (len(data['methods']), data['scanned_modules'], data['player_type'], data['placer_type']), flush=True)
            if args.trace_seconds:
                print('Observing calls for %g seconds; fire manually to exercise recoil.' % args.trace_seconds, flush=True)
                time.sleep(args.trace_seconds)
        finally:
            if args.trace_seconds:
                execute(CLEANUP.replace('OUTPUT', repr(str(trace_output))))
    finally:
        session.detach()
    if trace_output.exists():
        trace = json.loads(trace_output.read_text(encoding='utf-8'))
        assert trace['monitoring_removed']
        print('Observed %d calls; monitoring removed.' % sum(trace['counts'].values()))


if __name__ == '__main__':
    main()
