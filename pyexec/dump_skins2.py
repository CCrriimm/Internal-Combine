import os
import sys
import traceback

OUT = BASE


def wfile(name, text):
    try:
        with open(os.path.join(OUT, name), 'w', encoding='utf-8') as f:
            f.write(text)
        return True
    except Exception:
        traceback.print_exc()
        return False


def ints_in(obj, acc, depth=0):
    if depth > 4 or obj is None:
        return
    if isinstance(obj, dict):
        for k, v in obj.items():
            ints_in(k, acc, depth + 1)
            ints_in(v, acc, depth + 1)
    elif isinstance(obj, (list, tuple, set)):
        for v in obj:
            ints_in(v, acc, depth + 1)
    elif isinstance(obj, bool):
        return
    elif isinstance(obj, int):
        if 0 < obj < 10 ** 12:
            acc.add(obj)
    elif isinstance(obj, str):
        return


def load(modname):
    try:
        m = __import__(modname, fromlist=['data'])
        return getattr(m, 'data', None) or {}
    except Exception:
        return {}


gun = load('gclient.data.gun_skin_template_data')
lobby = load('gclient.data.lobby_item_data')
wdata = load('gclient.data.weapon_data')
equip = load('gclient.data.equip_data')

weapons = {}
for v in wdata.values():
    try:
        weapons[int(v.get('id'))] = str(v.get('name') or '')
    except Exception:
        pass

items = {}
for k, v in lobby.items():
    try:
        iid = int(v.get('id') or k)
    except Exception:
        continue
    items[iid] = v

by_ref = {}
ref_hits = {}
for iid, v in items.items():
    acc = set()
    ints_in(v.get('guise'), acc)
    ints_in(v.get('skin'), acc)
    for kk in ('model_id', 'related_id', 'link_id', 'target_id',
               'skin_id', 'skin_template_id', 'guise_id'):
        try:
            iv = int(v.get(kk) or 0)
        except Exception:
            iv = 0
        if iv > 0:
            acc.add(iv)
    for rv in acc:
        by_ref.setdefault(rv, []).append(iid)
        ref_hits[rv] = ref_hits.get(rv, 0) + 1

probe = []
for iid in (520020153, 520020152, 510010202, 510010200, 510010207):
    v = items.get(iid)
    if v is not None:
        probe.append('iid=%s quality=%r name=%r' % (iid, v.get('quality'), v.get('name')))
        probe.append('    guise=%r' % (v.get('guise'),))
        probe.append('    skin=%r' % (v.get('skin'),))
        probe.append('    sub_type=%r model_id=%r' % (v.get('sub_type'), v.get('model_id')))

rows = []
unmatched = 0
for v in gun.values():
    try:
        sid = int(v.get('id'))
        gid = int(v.get('gun_id'))
    except Exception:
        continue
    typ = int(v.get('gun_skin_type') or 0)
    cands = by_ref.get(sid, [])
    item = None
    how = ''
    for iid in cands:
        iv = items.get(iid)
        if not hasattr(iv, 'get'):
            continue
        if iv.get('quality') is not None:
            item = iv
            how = 'guise'
            break
    if item is None and sid in items:
        item = items[sid]
        how = 'id'
    if item is None and cands:
        item = items.get(cands[0])
        how = 'guise'
    if item is None:
        unmatched += 1
        rows.append([gid, weapons.get(gid, ''), sid, typ, '', None, '', '', ''])
        continue
    q = item.get('quality')
    try:
        q = int(q)
    except Exception:
        q = None
    rows.append([gid, weapons.get(gid, ''), sid, typ,
                 str(item.get('name') or ''), q, str(item.get('sub_type') or ''),
                 str(item.get('id') or ''), how])

rows.sort(key=lambda r: (r[0], r[2]))
hdr = 'gun_id\tweapon\tskin_id\tskin_type\tname\tquality\tsub_type\titem_id\tmatched_by\n'
lines = [hdr]
for r in rows:
    lines.append('%d\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n' % (
        r[0], r[1], r[2], r[3], r[4],
        '' if r[5] is None else r[5], r[6], r[7], r[8]))
wfile('skins_tiered.tsv', ''.join(lines))

qc = {}
for r in rows:
    qc[str(r[5])] = qc.get(str(r[5]), 0) + 1
summ = ['skins=%d unmatched=%d' % (len(rows), unmatched),
        'quality counts: %r' % (qc,), '', '--- probes ---'] + probe
wfile('skins_tiered_summary.txt', '\n'.join(summ) + '\n')

hi_lines = [hdr]
for r in rows:
    if r[5] is not None and r[5] >= 4:
        hi_lines.append('%d\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n' % (
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]))
wfile('skins_tiered_hi.tsv', ''.join(hi_lines))

mods = []
for name in sorted(sys.modules):
    if not (name.startswith('gclient.') or name.startswith('gshare.')):
        continue
    m = sys.modules.get(name)
    d = getattr(m, 'data', None)
    n = len(d) if hasattr(d, '__len__') else -1
    mods.append('%s\tdata=%d' % (name, n))
wfile('data_modules.txt', '\n'.join(mods) + '\n')

tier_words = ('eternal', 'ultra', 'legendary', 'evo', 'mythic', 'epic', 'quality', 'tier', 'rarity')
found = []
for name in sorted(sys.modules):
    if not (name.startswith('gclient.') or name.startswith('gshare.')):
        continue
    if not any(w in name.lower() for w in ('quality', 'tier', 'rarity', 'const', 'lang', 'text', 'local', 'name', 'item', 'skin')):
        continue
    m = sys.modules.get(name)
    for an in dir(m):
        if an.startswith('__'):
            continue
        try:
            obj = getattr(m, an)
        except Exception:
            continue
        n2v = getattr(obj, 'name_to_values', None)
        if isinstance(n2v, dict):
            keys = [k for k in n2v if any(w in str(k).lower() for w in tier_words)]
            if keys:
                found.append('%s.%s = %r' % (name, an, {k: n2v.get(k) for k in keys[:40]}))
            continue
        if not isinstance(obj, dict) or not obj or len(obj) > 20000:
            continue
        hit = 0
        samples = []
        for k, v in obj.items():
            if hit >= 12:
                break
            s = None
            if isinstance(v, str):
                s = v
            elif isinstance(v, dict):
                s = v.get('name') or v.get('desc') or v.get('label')
            if not isinstance(s, str):
                continue
            if any(w in s.lower() for w in tier_words[:5]):
                hit += 1
                samples.append('%r -> %r' % (k, s[:60]))
        if samples:
            found.append('%s.%s (%d) %s' % (name, an, len(obj), '; '.join(samples)))
wfile('tier_tables.txt', '\n'.join(found) + '\n')

melee = []
for k, v in equip.items():
    try:
        ec = str(v.get('equip_case') or '')
    except Exception:
        continue
    if 'melee' in ec.lower():
        melee.append('%r\t%s\t%r\t%s' % (k, ec, v.get('name'), v.get('equip_type')))
wfile('equip_melee.txt', '\n'.join(melee) + '\n')
