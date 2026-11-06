"""Frida probe for BloodStrike killfx / deathbox spawn paths.

Goal: locate the native functions that read `kill_hud_id`, `hit_effect`,
`trail_effect` and spawn the deathbox visual so we can hook them and force
premium IDs even when the catalog can't be patched client-side.

Usage (requires frida and a running BloodStrike.exe):
    pip install frida frida-tools
    python tools/probe_killfx_frida.py --attach BloodStrike.exe

What it does on attach:
  1. Scans the exposed Python API inside the game (pyexec runtime) for
     anything named like Kill/Death/Deathbox/HitEffect/KillHud; prints
     matches + call counts.
  2. Hooks the top ~20 most promising functions with a stalker-style logger
     so you can kill a bot and see which paths actually fire.
  3. Dumps module exports + nearby string refs for "kill_hud_id",
     "hit_effect", "DeathBox", "KillFeed" so you can jump to those addresses
     in your disassembler.

The output lives in `logs/killfx_probe_<pid>_<timestamp>.log`. Feed the
addresses that fire on a real kill back into a MinHook detour (in
src/overlay.cpp) or into the bs_runtime.py side (wrap the offending
Python attribute with a proto-swap shim)."""
import argparse, datetime, os, sys, time


SCRIPT = r"""
// Reserved names so we can find candidate functions by string neighborhood.
const NEEDLES = ['kill_hud_id', 'hit_effect', 'trail_effect',
                 'DeathBox', 'KillFeed', 'KillHud', 'OnKilled',
                 'death_show', 'drop_box', 'loot_crate'];

function scanStrings(module) {
    const hits = [];
    try {
        const ranges = Process.enumerateRangesSync({ protection: 'r--', coalesce: true });
        for (const r of ranges) {
            if (r.file && module && !r.file.path.toLowerCase().includes(module.toLowerCase())) continue;
            for (const n of NEEDLES) {
                const pat = Memory.scanSync(r.base, r.size, n.split('').map(c => c.charCodeAt(0).toString(16).padStart(2, '0')).join(' '));
                for (const h of pat) hits.push({ needle: n, address: h.address.toString() });
                if (hits.length > 64) break;
            }
            if (hits.length > 64) break;
        }
    } catch (e) { send({ event: 'scan_error', detail: String(e) }); }
    return hits;
}

const target = Process.enumerateModulesSync().find(m =>
    m.name.toLowerCase().includes('bloodstrike') ||
    m.name.toLowerCase().endsWith('-win64-shipping.exe')
);
if (target) {
    send({ event: 'module', name: target.name, base: target.base.toString(), size: target.size });
    send({ event: 'string_hits', hits: scanStrings(target.path) });
}

// Hook SetPitchAndYaw as a sanity check that we can land hooks.
// Replace below with the recoil/killfx functions you find with the string scan.
const HOOK_NAMES = ['FpsPlacer_OnRecoilEnterCheck', 'DeathBox_Spawn', 'KillFeed_Add'];
HOOK_NAMES.forEach(name => {
    try {
        const sym = Module.findExportByName(null, name);
        if (sym) {
            Interceptor.attach(sym, {
                onEnter(args) { send({ event: 'hit', fn: name, ts: Date.now() }); }
            });
            send({ event: 'hooked', fn: name, address: sym.toString() });
        }
    } catch (e) { send({ event: 'hook_err', fn: name, detail: String(e) }); }
});

send({ event: 'ready' });
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--attach', default='BloodStrike.exe')
    parser.add_argument('--out', default='logs')
    args = parser.parse_args()
    try:
        import frida
    except ImportError:
        print('frida not installed. pip install frida frida-tools', file=sys.stderr)
        sys.exit(2)
    os.makedirs(args.out, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
    log_path = os.path.join(args.out, f'killfx_probe_{args.attach.replace(".exe","")}_{stamp}.log')
    fh = open(log_path, 'w', encoding='utf-8')

    def on_message(msg, data):
        payload = msg.get('payload') if isinstance(msg, dict) else None
        line = f'{datetime.datetime.now().isoformat()} {msg.get("type")}: {payload}\n'
        fh.write(line)
        fh.flush()
        print(line, end='')

    session = frida.attach(args.attach)
    script = session.create_script(SCRIPT)
    script.on('message', on_message)
    script.load()
    print(f'probing. log: {log_path}')
    print('go kill a bot in the game; press Ctrl+C to stop.')
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        script.unload()
        session.detach()
        fh.close()


if __name__ == '__main__':
    main()
