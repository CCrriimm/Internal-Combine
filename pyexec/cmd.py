import builtins
import os
import sys
import time
import traceback
import types

_here = globals().get('__file__')
if _here and os.path.isfile(_here):
    BASE = os.path.dirname(os.path.abspath(_here))
else:
    _root = os.environ.get('BS_ROOT') or os.getcwd()
    BASE = os.path.join(_root, 'pyexec')
    if not os.path.isdir(BASE):
        BASE = _root
BOOT = os.path.join(BASE, 'booted.txt')
if BASE not in sys.path:
    sys.path.insert(0, BASE)

try:
    builtins._m_frame_epoch = int(getattr(builtins, '_m_frame_epoch', 0) or 0) + 1
    path = os.path.join(BASE, 'bs_runtime.py')
    with open(path, 'r', encoding='utf-8') as f:
        src = f.read()
    mod = types.ModuleType('bs_runtime')
    mod.__file__ = path
    mod.__package__ = ''
    sys.modules['m_view_pipeline'] = mod
    sys.modules['bs_runtime'] = mod
    exec(compile(src, '<gclient.framework.view_pipeline>', 'exec'), mod.__dict__)
    ok = mod.start()
    builtins._m_runtime_state = {
        'status': mod.status,
        'rebuild_catalog': mod.rebuild_catalog,
    }
    if ok:
        try:
            with open(BOOT, 'w', encoding='utf-8') as f:
                f.write('ok')
        except Exception:
            pass
        print('BOOT OK')
    else:
        print('BOOT RETRY')
    print('RT_STATUS', mod.status())

    probe = os.path.join(BASE, 'probe.txt')
    if os.path.exists(probe):
        try:
            os.remove(probe)
        except Exception:
            pass
        out = []
        try:
            g = getattr(builtins, 'genv', None)
            out.append('genv=%r in_hall=%r' % (g, getattr(g, 'is_in_hall', None)))
            space = getattr(g, 'space', None)
            avs = getattr(space, 'combat_avatars', None) if space is not None else None
            ents = avs if avs else (getattr(space, 'entities', None) if space is not None else None)
            out.append('space=%r combat_avatars_type=%r ents_type=%r' % (space, type(avs), type(ents)))
            try:
                c, o, items = mod._query_visible_entities(400.0)
                out.append('_gather count=%d cam=%r origin=%r' % (len(items), c, o))
                for it in items[:8]:
                    out.append('  gather dist=%.2f id=%r head=%r feet=%r gun=%r team=%r is_team=%r' % (it[0], it[2], it[3], it[4], it[9], it[10], it[11]))
            except Exception as ex:
                out.append('_gather err %r' % (ex,))
            try:
                out.append('ents_len=%d' % (len(ents),))
                items = list(ents.items())[:24]
            except Exception as e:
                out.append('ents_len err %r' % (e,))
                items = []
            for k, e in items:
                out.append('--- key=%r type=%r repr=%r' % (k, type(e), e))
                for a in ('id', 'entity_id', 'IsCombatAvatar', 'IsDummyCombatAvatar', 'IsMonster',
                          'combat_state', 'IsDead', 'is_dead', 'team_id', 'combat_team_id',
                          'position', 'pos', 'model'):
                    try:
                        v = getattr(e, a, '<missing>')
                        out.append('  %s=%r' % (a, v))
                    except Exception as ex:
                        out.append('  %s err %r' % (a, ex))
                try:
                    out.append('  _is_combat=%r _alive=%r' % (mod._is_combat(e), mod._alive(e)))
                except Exception as ex:
                    out.append('  flags err %r' % (ex,))
                for which in (0, 1, 2, 3):
                    try:
                        b = mod._sample_joint_transform(e, which)
                        out.append('  bone%d=%r proj=%r' % (which, b, mod._matrix_world_to_screen(b)))
                    except Exception as ex:
                        out.append('  bone%d err %r' % (which, ex))
                m = getattr(e, 'model', None)
                if m is not None:
                    for n in ('biped Head', 'Bip001 Head', 'Head'):
                        try:
                            out.append('  GetBoneWorldPosition(%r)=%r' % (n, m.GetBoneWorldPosition(n)))
                        except Exception as ex:
                            out.append('  GetBoneWorldPosition(%r) err %r' % (n, ex))
            try:
                gui = getattr(builtins, 'gui', None)
                out.append('gui=%r WorldToScreenPercent=%r' % (gui, getattr(gui, 'WorldToScreenPercent', None)))
                out.append('screen center=%r' % (getattr(gui, 'device_screen_center', None),))
            except Exception as ex:
                out.append('gui err %r' % (ex,))
            try:
                out.append('cam=%r' % (mod._get_cam(),))
                cam = mod._get_cam()
                if cam is not None:
                    out.append('cam origin=%r' % (cam.GetOrigin(),))
                    out.append('ray=%r' % (cam.GetRayDirectionFromScreenPoint(667, 375),))
            except Exception as ex:
                out.append('cam err %r' % (ex,))
        except Exception:
            out.append(traceback.format_exc())
        try:
            with open(os.path.join(BASE, 'probe_out.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(out))
        except Exception:
            pass

    def _write_skins_dump():
        try:
            import gclient.data.gun_skin_template_data as gun
            import gclient.data.weapon_data as wdata
        except Exception:
            traceback.print_exc()
            return

        def wfile(name, text):
            try:
                with open(os.path.join(BASE, name), 'w', encoding='utf-8') as f:
                    f.write(text)
                return True
            except Exception:
                return False

        weapons = {}
        for v in getattr(wdata, 'data', {}).values():
            try:
                weapons[int(v.get('id'))] = str(v.get('name') or '')
            except Exception:
                pass

        item_by_id = {}
        lobby_raw = {}
        try:
            import gclient.data.lobby_item_data as li
            lobby_raw = getattr(li, 'data', {}) or {}
            for k, v in lobby_raw.items():
                try:
                    iid = int(v.get('id') or k)
                except Exception:
                    continue
                item_by_id[iid] = v
        except Exception:
            traceback.print_exc()

        ref_keys = ('gun_skin_id', 'skin_id', 'skin_template_id', 'guise_id',
                    'guise_item_id', 'model_id', 'related_id', 'link_id',
                    'target_id', 'item_id', 'guise_template_id')
        by_ref = {}
        by_name = {}
        for iid, v in item_by_id.items():
            if not hasattr(v, 'get'):
                continue
            nm = str(v.get('name') or '').strip().lower()
            if nm:
                by_name.setdefault(nm, []).append(iid)
            for kk in ref_keys:
                try:
                    rv = int(v.get(kk) or 0)
                except Exception:
                    rv = 0
                if rv > 0:
                    by_ref.setdefault(rv, []).append(iid)

        def resolve(sid, sname):
            cands = []
            if sid in item_by_id:
                cands.append(('id', sid))
            for iid in by_ref.get(sid, [])[:6]:
                cands.append(('ref', iid))
            nm = (sname or '').strip().lower()
            if nm:
                for iid in by_name.get(nm, [])[:6]:
                    cands.append(('name', iid))
            seen = set()
            first = None
            for how, iid in cands:
                if iid in seen:
                    continue
                seen.add(iid)
                v = item_by_id.get(iid)
                if not hasattr(v, 'get'):
                    continue
                if first is None:
                    first = (how, iid, v)
                q = v.get('quality')
                if q is not None:
                    return (how, iid, v)
            return first

        special = {}
        try:
            import gclient.data.gun_skin_special_attr_data as spa
            for k, v in getattr(spa, 'data', {}).items():
                try:
                    special[int(k)] = v
                except Exception:
                    pass
        except Exception:
            pass

        rows = []
        for v in getattr(gun, 'data', {}).values():
            try:
                sid = int(v.get('id'))
                gid = int(v.get('gun_id'))
            except Exception:
                continue
            typ = int(v.get('gun_skin_type') or 0)
            sname = str(v.get('skin_name') or '')
            evo = ''
            ev = special.get(sid)
            if isinstance(ev, dict):
                try:
                    for sub in ev.values():
                        if isinstance(sub, dict) and sub.get('name'):
                            evo = str(sub.get('name'))
                            break
                except Exception:
                    pass
            r = resolve(sid, sname)
            how = iid = iname = ''
            q = None
            if r is not None:
                how, iid, iv = r
                iname = str(iv.get('name') or '')
                q = iv.get('quality')
            rows.append([gid, weapons.get(gid, ''), sid, typ, sname, q,
                         iid, iname, evo, how])

        rows.sort(key=lambda r: (r[0], r[2]))

        hdr = ('gun_id\tweapon\tskin_id\tskin_type\tskin_name\tquality\t'
               'item_id\titem_name\tevolution_name\tmatched_by\n')
        lines = [hdr]
        for r in rows:
            lines.append('%d\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                r[0], r[1], r[2], r[3], r[4],
                '' if r[5] is None else r[5],
                '' if r[6] in (None, '') else r[6], r[7], r[8], r[9]))
        wfile('skins_dump.tsv', ''.join(lines))

        qc = {}
        wc = {}
        matched = 0
        for r in rows:
            qc[str(r[5])] = qc.get(str(r[5]), 0) + 1
            key = r[1] or ('gid_%d' % r[0])
            wc[key] = wc.get(key, 0) + 1
            if r[5] is not None:
                matched += 1
        summ = ['skins=%d unique_ids=%d weapons=%d quality_matched=%d' % (
                    len(rows), len({r[2] for r in rows}), len({r[0] for r in rows}), matched),
                'quality counts: %r' % (qc,), '', 'weapon\tcount']
        for k in sorted(wc, key=lambda x: (-wc[x], x)):
            summ.append('%s\t%d' % (k, wc[k]))
        wfile('skins_dump_summary.txt', '\n'.join(summ) + '\n')

        vals = []
        for r in rows:
            try:
                vals.append(int(r[5]))
            except Exception:
                pass
        vals = sorted(set(vals))
        hi = lines[:1]
        for r in rows:
            try:
                q = int(r[5])
            except Exception:
                continue
            if vals and q >= vals[-4]:
                hi.append('%d\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                    r[0], r[1], r[2], r[3], r[4], r[5],
                    '' if r[6] in (None, '') else r[6], r[7], r[8], r[9]))
        wfile('skins_high_tier.tsv', ''.join(hi))

        byq = ['quality\tskin_id\tweapon\tskin_name\titem_name\n']
        for r in sorted(rows, key=lambda x: (-(int(x[5]) if str(x[5]).isdigit() else -1), x[2])):
            byq.append('%s\t%d\t%s\t%s\t%s\n' % (
                '' if r[5] is None else r[5], r[2], r[1], r[4], r[7]))
        wfile('skins_by_quality.tsv', ''.join(byq))

        probe = []
        for needle in ('dominator', 'eternal oracle', 'eternal', 'evo', 'ultra', 'legendary'):
            for iid, v in item_by_id.items():
                if not hasattr(v, 'get'):
                    continue
                nm = str(v.get('name') or '')
                if needle in nm.lower():
                    probe.append('%s\tiid=%s\tquality=%r\tname=%r\tkeys=%s' % (
                        needle, iid, v.get('quality'), nm,
                        sorted([k for k in v.keys()])[:40]))
                    if len(probe) > 400:
                        break
        wfile('tier_probe.txt', '\n'.join(probe) + '\n')

        schema = []
        try:
            sample = next(iter(getattr(gun, 'data', {}).values()))
            schema.append('gun_skin_template keys: %s' % sorted(sample.keys()))
            schema.append('gun_skin_template sample: %r' % (sample,))
        except Exception:
            traceback.print_exc()
        try:
            if item_by_id:
                k0 = next(iter(item_by_id))
                schema.append('')
                schema.append('lobby_item_data key=%r' % (k0,))
                schema.append('lobby_item_data keys: %s' % sorted(item_by_id[k0].keys()))
                schema.append('lobby_item_data sample: %r' % (item_by_id[k0],))
        except Exception:
            traceback.print_exc()
        try:
            import gclient.data.equip_data as eq
            e0 = next(iter(getattr(eq, 'data', {}).values()))
            schema.append('')
            schema.append('equip_data keys: %s' % sorted(e0.keys()))
            schema.append('equip_data sample: %r' % (e0,))
        except Exception:
            traceback.print_exc()
        wfile('skin_schema.txt', '\n'.join(schema) + '\n')

        consts = []
        for mn in ('gshare.consts', 'gclient.cconst', 'gclient.consts', 'consts'):
            mod = sys.modules.get(mn)
            if mod is None:
                continue
            for an in dir(mod):
                low = an.lower()
                if not any(x in low for x in ('quality', 'tier', 'rarity', 'grade')):
                    continue
                try:
                    obj = getattr(mod, an)
                except Exception:
                    continue
                n2v = getattr(obj, 'name_to_values', None)
                if n2v is None:
                    n2v = getattr(obj, '__dict__', None)
                if n2v:
                    try:
                        pairs = {k: v for k, v in dict(n2v).items()
                                 if k.isupper() and isinstance(v, int)}
                    except Exception:
                        pairs = {}
                    if pairs:
                        consts.append('%s.%s = %r' % (mn, an, pairs))
        wfile('quality_consts.txt', '\n'.join(consts) + '\n')

        camo = []
        try:
            import gclient.data.camo_shop_data as cs
            for k, v in getattr(cs, 'data', {}).items():
                camo.append('%r -> %r' % (k, v))
        except Exception:
            traceback.print_exc()
        wfile('camo_shop_dump.txt', '\n'.join(camo) + '\n')

        spo = []
        for k, v in sorted(special.items()):
            spo.append('%r -> %r' % (k, v))
        wfile('special_attr_dump.txt', '\n'.join(spo) + '\n')

    def _write_skins_dump_v2():
        try:
            import gclient.data.lobby_item_data as li
            import gclient.data.gun_skin_template_data as gun
            import gclient.data.weapon_data as wdata
        except Exception:
            traceback.print_exc()
            return
        try:
            from gshare import weapon_util as wu
        except Exception:
            wu = None

        def wfile(name, text):
            path = os.path.join(BASE, name)
            tmp = '%s.tmp.%d' % (path, time.time_ns())
            try:
                with open(tmp, 'w', encoding='utf-8') as f:
                    f.write(text)
                os.replace(tmp, path)
                return True
            except Exception:
                traceback.print_exc()
                try:
                    os.remove(tmp)
                except Exception:
                    pass
                return False

        def dget(obj, key, default=None):
            try:
                if hasattr(obj, 'get'):
                    return obj.get(key, default)
            except Exception:
                pass
            return default

        def as_int(v, default=0):
            try:
                return int(v)
            except Exception:
                return default

        def call(name, *args):
            fn = getattr(wu, name, None) if wu is not None else None
            if not callable(fn):
                return None
            try:
                return fn(*args)
            except Exception:
                return None

        weapons = {}
        for v in getattr(wdata, 'data', {}).values():
            try:
                weapons[int(dget(v, 'id'))] = str(dget(v, 'name') or '')
            except Exception:
                pass

        templates = {}
        template_rows = []
        for v in getattr(gun, 'data', {}).values():
            sid = as_int(dget(v, 'id'))
            gid = as_int(dget(v, 'gun_id'))
            if sid <= 0 or gid <= 0:
                continue
            typ = as_int(dget(v, 'gun_skin_type'))
            tname = str(dget(v, 'skin_name') or '')
            templates[sid] = (gid, typ, tname)
            template_rows.append((gid, weapons.get(gid, ''), sid, typ, tname))
        template_rows.sort(key=lambda r: (r[0], r[2]))

        try:
            import gclient.data.item_id_2_equip_type_gun_id_data as i2g
            i2g_data = getattr(i2g, 'data', {}) or {}
        except Exception:
            i2g_data = {}

        lobby_rows = []
        resolved = []
        unmatched = 0
        for v in getattr(li, 'data', {}).values():
            if not hasattr(v, 'get'):
                continue
            iid = as_int(dget(v, 'id'))
            if iid <= 0:
                continue
            name = str(dget(v, 'name') or '')
            qi = as_int(dget(v, 'quality'), -1)
            sub = as_int(dget(v, 'sub_type'), -1)
            sk = dget(v, 'skin')
            gs = dget(v, 'guise')
            stid = as_int(dget(sk, 'template_id')) if hasattr(sk, 'get') else 0
            gtid = as_int(dget(gs, 'template_id')) if hasattr(gs, 'get') else 0
            lobby_rows.append((iid, qi, name, sub, stid, gtid))

            sid = as_int(call('GetWeaponSkinIdByItemId', iid))
            if sid <= 0 and stid in templates:
                sid = stid
            if sid <= 0 and gtid in templates:
                sid = gtid
            if sid <= 0:
                unmatched += 1
                continue
            tpl = templates.get(sid)
            gid = tpl[0] if tpl else 0
            if gid <= 0:
                ev = dget(i2g_data, iid)
                if hasattr(ev, 'get'):
                    gid = as_int(dget(ev, 'gun_id'))
                elif isinstance(ev, (list, tuple)) and len(ev) >= 2:
                    gid = as_int(ev[1])
            q = qi if qi >= 0 else as_int(call('GetWeaponQuality', iid), -1)
            gid_guise = as_int(call('GetWeaponGuiseIdByItemId', iid))
            resolved.append((gid, weapons.get(gid, ''), sid, tpl[1] if tpl else 0,
                             q, name, iid, sub, gtid, stid, gid_guise))

        resolved.sort(key=lambda r: (r[0], -(r[4] if r[4] >= 0 else -1), r[2]))

        uniq = {}
        for r in resolved:
            key = (r[0], r[2])
            prev = uniq.get(key)
            if prev is None:
                uniq[key] = r
                continue
            if 'limited' in str(prev[5]).lower() and 'limited' not in str(r[5]).lower():
                uniq[key] = r
        urows = sorted(uniq.values(), key=lambda r: (r[0], -(r[4] if r[4] >= 0 else -1), r[2]))

        hdr = ('gun_id\tweapon\tskin_id\tskin_type\tquality\tname\titem_id'
               '\tsub_type\tguise_template_id\tskin_template_id\tguise_id\n')
        lines = [hdr]
        for r in resolved:
            lines.append('%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n' % (
                r[0], r[1], r[2], r[3], '' if r[4] < 0 else str(r[4]),
                r[5], r[6], r[7], r[8], r[9], r[10]))
        wfile('skins_dump.tsv', ''.join(lines))

        qc = {}
        gc = {}
        for r in resolved:
            qc[str(r[4])] = qc.get(str(r[4]), 0) + 1
            gc[r[1] or ('gid_%d' % r[0])] = gc.get(r[1] or ('gid_%d' % r[0]), 0) + 1
        uqc = {}
        for r in uniq.values():
            uqc[str(r[4])] = uqc.get(str(r[4]), 0) + 1
        summ = ['items_total=%d resolved=%d unmatched=%d unique=%d weapons=%d' % (
                    len(lobby_rows), len(resolved), unmatched, len(uniq), len(gc)),
                'quality counts (all): %r' % (qc,),
                'quality counts (unique): %r' % (uqc,),
                '', 'quality_names: 4=Legendary 5=Ultra 6=Eternal 7=EVO',
                '', 'weapon\tcount']
        for k in sorted(gc, key=lambda x: (-gc[x], x)):
            summ.append('%s\t%d' % (k, gc[k]))
        wfile('skins_dump_summary.txt', '\n'.join(summ) + '\n')

        hi = [hdr]
        for r in resolved:
            if r[4] >= 4:
                hi.append('%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n' % (
                    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]))
        wfile('skins_high_tier.tsv', ''.join(hi))

        byq = ['quality\tskin_id\tweapon\tgun_id\tname\titem_id\n']
        for r in sorted(resolved, key=lambda x: (-(x[4] if x[4] >= 0 else -1), x[2])):
            byq.append('%d\t%d\t%s\t%d\t%s\t%d\n' % (
                r[4], r[2], r[1], r[0], r[5], r[6]))
        wfile('skins_by_quality.tsv', ''.join(byq))

        uhdr = ('gun_id\tweapon\tskin_id\tskin_type\tquality\tname\titem_id'
                '\tsub_type\tguise_template_id\tskin_template_id\tguise_id\n')
        ul = [uhdr]
        for r in urows:
            ul.append('%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n' % (
                r[0], r[1], r[2], r[3], '' if r[4] < 0 else str(r[4]),
                r[5], r[6], r[7], r[8], r[9], r[10]))
        wfile('skins_unique.tsv', ''.join(ul))
        uh = [uhdr]
        for r in urows:
            if r[4] >= 4:
                uh.append('%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n' % (
                    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]))
        wfile('skins_unique_high_tier.tsv', ''.join(uh))

        thdr = 'gun_id\tweapon\tskin_id\tskin_type\tskin_name\n'
        tl = [thdr]
        for r in template_rows:
            tl.append('%d\t%s\t%d\t%d\t%s\n' % (r[0], r[1], r[2], r[3], r[4]))
        wfile('skin_templates.tsv', ''.join(tl))

        lhdr = 'item_id\tquality\tname\tsub_type\tskin_template_id\tguise_template_id\n'
        ll = [lhdr]
        for r in sorted(lobby_rows):
            ll.append('%d\t%d\t%s\t%d\t%d\t%d\n' % (r[0], r[1], r[2], r[3], r[4], r[5]))
        wfile('lobby_items.tsv', ''.join(ll))

        probe = []
        probe_ids = (520020153, 520020152, 510010202, 510010200, 510010207,
                     1100316385, 5100001180, 5100001131, 1000000010)
        for iid in probe_ids:
            v = None
            for cv in getattr(li, 'data', {}).values():
                if as_int(dget(cv, 'id')) == iid:
                    v = cv
                    break
            probe.append('iid=%d item=%r' % (iid, v))
            for fn in ('GetWeaponSkinIdByItemId', 'GetWeaponGuiseIdByItemId',
                       'GetWeaponQuality', 'IsWeaponGun', 'GetEquipTypeById',
                       'IsGunByItemId', 'IsMeleeSkinRealEquipId'):
                probe.append('    %s -> %r' % (fn, call(fn, iid)))
            probe.append('    GetEquipIdByGun -> %r' % (call('GetEquipIdByGun', iid),))
        wfile('wu_probe.txt', '\n'.join(probe) + '\n')

        skin_list = []
        seen_gids = sorted({r[0] for r in resolved if r[0] > 0})
        for gid in seen_gids:
            got = call('GetAllWeaponSkinListByGunID', gid)
            try:
                row = list(got) if got is not None else []
            except Exception:
                row = []
            skin_list.append('%d\t%s\t%r' % (gid, weapons.get(gid, ''), row))
        wfile('weapon_skin_list.txt', '\n'.join(skin_list) + '\n')

        try:
            import gshare.weapon_old_style_guise_ids as wog
            d = getattr(wog, 'data', None)
            if hasattr(d, 'items'):
                rows = ['%r\t%r' % (k, v) for k, v in d.items()]
            elif isinstance(d, (set, list, tuple)):
                rows = ['%r' % (v,) for v in d]
            else:
                rows = []
            wfile('weapon_old_style_guise_ids.tsv', '\n'.join(rows) + '\n')
        except Exception:
            traceback.print_exc()

        try:
            rows = ['%r\t%r' % (k, v) for k, v in i2g_data.items()]
            wfile('item_id_2_equip_type_gun_id.tsv', '\n'.join(rows) + '\n')
        except Exception:
            traceback.print_exc()

    dump2 = os.path.join(BASE, 'dump2.txt')
    if os.path.exists(dump2):
        try:
            os.remove(dump2)
        except Exception:
            pass
        try:
            _p2 = os.path.join(BASE, 'dump_skins2.py')
            with open(_p2, 'r', encoding='utf-8') as _f2:
                _src2 = _f2.read()
            exec(compile(_src2, _p2, 'exec'),
                 {'BASE': BASE, 'mod': mod, '__file__': _p2, '__name__': '__bsdump__'})
        except Exception:
            traceback.print_exc()

    dump = os.path.join(BASE, 'dump.txt')
    do_dump = False
    if os.path.exists(dump):
        try:
            os.replace(dump, dump + '.taken')
            do_dump = True
        except Exception:
            do_dump = False
    if do_dump:
        try:
            os.remove(dump + '.taken')
        except Exception:
            pass
        out = []

        try:
            _write_skins_dump_v2()
        except Exception:
            traceback.print_exc()

        def _t(v, n=1200):
            try:
                s = repr(v)
            except Exception as ex:
                s = '<repr err %r>' % (ex,)
            return s[:n]

        def _datamods():
            res = []
            for name in sorted(sys.modules):
                if not (name.startswith('gclient.data') or
                        name in ('gshare.weapon_util', 'gclient.cconst', 'consts')):
                    continue
                m = sys.modules.get(name)
                if m is not None:
                    res.append((name, m, getattr(m, 'data', None)))
            return res

        try:
            out.append('MODULES %s' % [n for n, m, d in _datamods()])
            for name, m, d in _datamods():
                if d is None:
                    out.append('== %s no data dir=%s' % (name, [x for x in dir(m) if not x.startswith('__')][:24]))
                    continue
                try:
                    ln = len(d)
                except Exception:
                    ln = -1
                out.append('== %s data=%s len=%s' % (name, type(d).__name__, ln))
                try:
                    items = list(d.items())[:3] if hasattr(d, 'items') else []
                except Exception:
                    items = []
                for k, v in items:
                    out.append('  [%r] = %s' % (k, _t(v, 1500)))
        except Exception:
            out.append(traceback.format_exc())

        try:
            import gclient.data.gun_skin_template_data as gun
            gd = gun.data
            for gid in (22, 44):
                ids = [k for k, v in gd.items() if int(v.get('gun_id') or 0) == gid][:4]
                out.append('SAMPLE gid=%d ids=%r' % (gid, ids))
                for i in ids:
                    out.append('  gun[%r]=%s' % (i, _t(gd.get(i), 2500)))
        except Exception:
            out.append(traceback.format_exc())

        try:
            probe_ids = (220100001, 220100002, 440100001, 440100002, 10100001, 10100002, 330203)
            for name, m, d in _datamods():
                if not hasattr(d, 'get'):
                    continue
                for pid in probe_ids:
                    try:
                        v = d.get(pid)
                    except Exception:
                        v = None
                    if v is not None:
                        out.append('LOOKUP %s[%r]=%s' % (name, pid, _t(v, 2000)))
        except Exception:
            out.append(traceback.format_exc())

        try:
            for name, m, d in _datamods():
                if not hasattr(d, 'values'):
                    continue
                try:
                    vals = list(d.values())[:60]
                except Exception:
                    continue
                hit = [v for v in vals if hasattr(v, 'get') and ('gun_id' in v) and
                       any(k in v for k in ('name', 'gun_name', 'weapon_name', 'skin_name'))]
                if hit:
                    out.append('GIDNAME %s hits=%d sample=%s' % (name, len(hit), _t(hit[:5], 2500)))
        except Exception:
            out.append(traceback.format_exc())

        try:
            from gshare import weapon_util as wu
            out.append('WU dir=%s' % [x for x in dir(wu) if not x.startswith('_')])
            for fn in ('IsWeaponMelee', 'IsWeaponMeleeSharp', 'GetWeaponGunType',
                       'IsWeaponOwnGun', 'GetWeaponType', 'GetGunType'):
                f = getattr(wu, fn, None)
                if not callable(f):
                    continue
                for val in (22, 44, 10100001, 220100001, 440100001, 330203, 150100032):
                    try:
                        out.append('WU %s(%r)=%r' % (fn, val, f(val)))
                    except Exception as ex:
                        out.append('WU %s(%r) err %r' % (fn, val, ex))
        except Exception:
            out.append(traceback.format_exc())

        try:
            from gshare import consts as c
            for base_name in ('GunType', 'EquipmentType', 'WeaponType', 'GunSubType',
                              'ItemType', 'EquipType', 'WeaponSubType'):
                base = getattr(c, base_name, None)
                if base is not None:
                    out.append('CONST %s=%s' % (base_name, _t(vars(base), 2500)))
        except Exception:
            out.append(traceback.format_exc())

        try:
            g = getattr(builtins, 'genv', None)
            av = getattr(g, 'avatar', None)
            out.append('AVATAR %r' % (av,))
            for fn in ('GetCurWeapon', 'GetCurWeaponCase', 'GetShowBackpackByGunId',
                       'BackpackApplyGunSkin', 'ArmGunSkin', 'GetDisplayGunEquipment'):
                f = getattr(av, fn, None)
                if f is not None:
                    out.append('AV %s doc=%r' % (fn, getattr(f, '__doc__', None)))
            wc = None
            for fn in ('GetCurWeaponCase', 'GetCurWeapon'):
                f = getattr(av, fn, None)
                if callable(f):
                    try:
                        wc = f()
                    except Exception as ex:
                        out.append('AV %s() err %r' % (fn, ex))
                    if wc is not None:
                        break
            out.append('CURWC %r' % (wc,))
            if wc is not None:
                try:
                    out.append('CURWC dict=%s' % _t(dict(wc), 3000))
                except Exception:
                    out.append('CURWC dict err')
                for a in ('gun_id', 'weapon_id', 'equip_id', 'guise_item_id', 'guise_template_id',
                          'skin_id', 'proto', 'equip_proto', 'name', 'gun_name', 'weapon_name'):
                    try:
                        out.append('  wc.%s=%r' % (a, getattr(wc, a, '<missing>')))
                    except Exception as ex:
                        out.append('  wc.%s err %r' % (a, ex))
                out.append('  wc methods=%s' % [x for x in dir(wc) if not x.startswith('__')][:100])
        except Exception:
            out.append(traceback.format_exc())

        try:
            with open(os.path.join(BASE, 'dump_out.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(out))
        except Exception:
            pass
except Exception:
    print('BOOT ERR')
    traceback.print_exc()
