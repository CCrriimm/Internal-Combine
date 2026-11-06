import sys

with open('bs-cheat/pyexec/bs_runtime.py.bak', 'rb') as f:
    text = f.read().decode('utf-8')

crlf = '\r\n' in text
text = text.replace('\r\n', '\n')

# 1. Update flags: remove flick, add stream proof and exit cheat
old_flags = '''F_FLICK = 1 << 18
F_FLICK_FIRE = 1 << 19
F_UNLOCK_EFFECTS = 1 << 20
F_BLOCK_REPORT = 1 << 21
F_SKIN_CURRENT = 1 << 22
F_RECOIL = 1 << 23
F_FLICK_FOV_CIRCLE = 1 << 24

DEFAULT_FLAGS = (F_MENU_OPEN | F_ESP | F_ESP_BOX | F_ESP_NAME | F_ESP_DISTANCE |
                 F_ESP_HEALTH | F_ESP_HEADDOT | F_AIM_FOV_CIRCLE |
                 F_FLICK_FIRE | F_UNLOCK_EFFECTS | F_BLOCK_REPORT | F_SKIN_CURRENT |
                 F_RECOIL)'''

new_flags = '''F_UNLOCK_EFFECTS = 1 << 20
F_BLOCK_REPORT = 1 << 21
F_SKIN_CURRENT = 1 << 22
F_RECOIL = 1 << 23
#F_STREAM_PROOF removed
F_EXIT_CHEAT = 1 << 26

DEFAULT_FLAGS = (F_MENU_OPEN | F_ESP | F_ESP_BOX | F_ESP_NAME | F_ESP_DISTANCE |
                 F_ESP_HEALTH | F_ESP_HEADDOT | F_AIM_FOV_CIRCLE |
                 F_UNLOCK_EFFECTS | F_BLOCK_REPORT | F_SKIN_CURRENT |
                 F_RECOIL)'''
assert old_flags in text, 'old_flags not found'
text = text.replace(old_flags, new_flags)

# 2. Remove flick variables
old_vars = '''_flick_prev = False
_last_flick = 0.0
_fire_release_at = 0.0
_aim_prev = False
_aim_latched = False
_flick_queue = None
_aim_busy = False'''

new_vars = '''_fire_release_at = 0.0
_aim_prev = False
_aim_latched = False
_aim_busy = False
_cached_cfg = None'''
assert old_vars in text, 'old_vars not found'
text = text.replace(old_vars, new_vars)

# 3. Remove _flick_smooth
old_fs = '''def _flick_smooth(cfg):
    try:
        return ((_options(cfg) >> 8) & 0xFF) / 100.0
    except Exception:
        return 0.0


'''
assert old_fs in text, 'old_fs not found'
text = text.replace(old_fs, '')

# 4. Update BLOCK_WORDS and anticheat hooks
old_hooks = '''BLOCK_WORDS = ('report', 'ban', 'kick', 'appeal', 'antiplugin', 'anticheat', 'cheat', 'qsec',
               'storyticktrace', 'screenshot', 'warden', 'uploadbug', 'uploaddistance',
               'essentialdata', 'notifygotologinanti', 'checkneedreportbug', 'onreceivebugreport',
               'execscript', 'windowshowstatus', 'collectdebugdata', 'uploadphoto')

KILL_METHODS = ('ScreenShotDebug', 'TryUploadScreenShotDebug', 'UploadPhotoProfileDebug',
                'CollectDebugData', 'OnCollectDebugData', 'ExecScriptAndUpload',
                'AntiPcScreenShot', 'NotifyGotoLoginAnti', 'OnReceiveBugReport')


def _blocked_method(method):
    try:
        m = str(method).lower()
    except Exception:
        return False
    for w in BLOCK_WORDS:
        if w in m:
            return True
    return False


def _fake_reply(method):
    try:
        m = str(method).lower()
    except Exception:
        m = ''
    if 'checkneedreportbug' in m:
        return False
    if 'onreceivebugreport' in m:
        return None
    if 'report' in m or 'salog' in m or 'ban' in m:
        return True
    return None


def _install_hooks():
    global _hooks_done, _hook_avatar_id, _class_hooks_at
    try:
        av = getattr(genv, 'avatar', None)
    except Exception:
        av = None
    if av is None:
        return
    targets = []
    for obj in (av, getattr(genv, 'player', None), getattr(genv, 'area', None)):
        if obj is not None and all(obj is not x for x in targets):
            targets.append(obj)

    def stale(fn):
        return getattr(fn, '_bs_wrapped', False) and getattr(fn, '_bs_gen', None) == _my_gen

    def wrap_call(orig):
        def call(method, *args, **kwargs):
            if _flag(_read_cfg(), F_BLOCK_REPORT) and _blocked_method(method):
                _log('blocked CallServer ' + str(method))
                return _fake_reply(method)
            return orig(method, *args, **kwargs)
        call._bs_wrapped = True
        call._bs_gen = _my_gen
        return call

    def wrap_log(orig, always):
        def salog(operation, *args, **kwargs):
            if always or (_flag(_read_cfg(), F_BLOCK_REPORT) and _blocked_method(operation)):
                _log('blocked SALog ' + str(operation))
                return True
            return orig(operation, *args, **kwargs)
        salog._bs_wrapped = True
        salog._bs_gen = _my_gen
        return salog

    for obj in targets:
        for attr in ('CallServer', 'CallServerOld', 'CallServerNew'):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, wrap_call(orig))
            except Exception:
                pass
        for attr, always in (('SALogToServer', False), ('SALogToServerLite', False),
                             ('SALogToServerBsStat', True)):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, wrap_log(orig, always))
            except Exception:
                pass
        for attr in KILL_METHODS:
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            def blocked(*args, __orig=orig, **kwargs):
                if not _flag(_read_cfg(), F_BLOCK_REPORT):
                    return __orig(*args, **kwargs)
                return None
            blocked._bs_wrapped = True
            blocked._bs_gen = _my_gen
            try:
                setattr(obj, attr, blocked)
            except Exception:
                pass
        try:
            orig = getattr(obj, 'CheckNeedReportBug', None)
        except Exception:
            orig = None
        if callable(orig) and not stale(orig):
            def never(*args, **kwargs):
                return False
            never._bs_wrapped = True
            never._bs_gen = _my_gen
            try:
                obj.CheckNeedReportBug = never
            except Exception:
                pass

    now = time.time()
    if now - _class_hooks_at >= 30.0:
        _class_hooks_at = now
        for mod_name in list(sys.modules.keys()):
            if not isinstance(mod_name, str):
                continue
            if not (mod_name.startswith('gclient') or mod_name.startswith('gshare')):
                continue
            mod = sys.modules.get(mod_name)
            if mod is None:
                continue
            try:
                members = list(vars(mod).values())
            except Exception:
                continue
            for cls in members:
                if not isinstance(cls, type):
                    continue
                for attr in KILL_METHODS:
                    try:
                        orig = cls.__dict__.get(attr)
                    except Exception:
                        continue
                    if not callable(orig) or getattr(orig, '_bs_wrapped', False):
                        continue
                    def blocked(self, *args, __orig=orig, **kwargs):
                        if not _flag(_read_cfg(), F_BLOCK_REPORT):
                            return __orig(self, *args, **kwargs)
                        return None
                    blocked._bs_wrapped = True
                    blocked._bs_gen = _my_gen
                    try:
                        setattr(cls, attr, blocked)
                    except Exception:
                        pass
    _hooks_done = True
    _hook_avatar_id = id(av)'''

new_hooks = '''BLOCK_WORDS = ('report', 'screenshot', 'uploadbug', 'checkneedreportbug', 'execscript', 'uploadphoto')

KILL_METHODS = ('ScreenShotDebug', 'TryUploadScreenShotDebug', 'UploadPhotoProfileDebug',
                'CollectDebugData', 'OnCollectDebugData', 'ExecScriptAndUpload',
                'AntiPcScreenShot', 'NotifyGotoLoginAnti', 'OnReceiveBugReport')


def _blocked_method(method):
    try:
        m = str(method).lower()
    except Exception:
        return False
    for w in BLOCK_WORDS:
        if w in m:
            return True
    return False


def _fake_reply(method):
    try:
        m = str(method).lower()
    except Exception:
        m = ''
    if 'checkneedreportbug' in m:
        return False
    if 'onreceivebugreport' in m:
        return None
    if 'report' in m or 'salog' in m or 'ban' in m:
        return True
    return None


def _install_hooks():
    global _hooks_done, _hook_avatar_id
    try:
        av = getattr(genv, 'avatar', None)
    except Exception:
        av = None
    if av is None:
        return
    if id(av) == _hook_avatar_id and _hooks_done:
        return

    targets = []
    for obj in (av, getattr(genv, 'player', None), getattr(genv, 'area', None)):
        if obj is not None and all(obj is not x for x in targets):
            targets.append(obj)

    def stale(fn):
        return getattr(fn, '_bs_wrapped', False) and getattr(fn, '_bs_gen', None) == _my_gen

    def wrap_call(orig):
        def call(method, *args, **kwargs):
            cfg = _cached_cfg
            if cfg is not None and _flag(cfg, F_BLOCK_REPORT) and _blocked_method(method):
                return _fake_reply(method)
            return orig(method, *args, **kwargs)
        call._bs_wrapped = True
        call._bs_gen = _my_gen
        call._bs_orig = orig
        return call

    def wrap_log(orig):
        def salog(operation, *args, **kwargs):
            cfg = _cached_cfg
            if cfg is not None and _flag(cfg, F_BLOCK_REPORT) and _blocked_method(operation):
                return True
            return orig(operation, *args, **kwargs)
        salog._bs_wrapped = True
        salog._bs_gen = _my_gen
        salog._bs_orig = orig
        return salog

    for obj in targets:
        for attr in ('CallServer', 'CallServerOld', 'CallServerNew'):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, wrap_call(orig))
            except Exception:
                pass
        for attr in ('SALogToServer', 'SALogToServerLite'):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, wrap_log(orig))
            except Exception:
                pass
        for attr in KILL_METHODS:
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            def blocked(*args, __orig=orig, **kwargs):
                cfg = _cached_cfg
                if cfg is not None and not _flag(cfg, F_BLOCK_REPORT):
                    return __orig(*args, **kwargs)
                return None
            blocked._bs_wrapped = True
            blocked._bs_gen = _my_gen
            blocked._bs_orig = orig
            try:
                setattr(obj, attr, blocked)
            except Exception:
                pass
        try:
            orig = getattr(obj, 'CheckNeedReportBug', None)
        except Exception:
            orig = None
        if callable(orig) and not stale(orig):
            def never(*args, **kwargs):
                return False
            never._bs_wrapped = True
            never._bs_gen = _my_gen
            never._bs_orig = orig
            try:
                obj.CheckNeedReportBug = never
            except Exception:
                pass

    _hooks_done = True
    _hook_avatar_id = id(av)


def _restore_hooks():
    global _hooks_done, _hook_avatar_id
    try:
        targets = []
        for obj in (getattr(genv, 'avatar', None), getattr(genv, 'player', None), getattr(genv, 'area', None)):
            if obj is not None and all(obj is not x for x in targets):
                targets.append(obj)
        for obj in targets:
            for attr in ('CallServer', 'CallServerOld', 'CallServerNew', 'SALogToServer', 'SALogToServerLite', 'CheckNeedReportBug') + KILL_METHODS:
                fn = getattr(obj, attr, None)
                if fn is not None and getattr(fn, '_bs_wrapped', False):
                    orig = getattr(fn, '_bs_orig', None)
                    if orig is not None:
                        try:
                            setattr(obj, attr, orig)
                        except Exception:
                            pass
    except Exception:
        pass
    _hooks_done = False
    _hook_avatar_id = None'''
assert old_hooks in text, 'old_hooks not found'
text = text.replace(old_hooks, new_hooks)

# 5. Fix _project Y-inversion
old_proj = '''def _project(v):
    if v is None:
        return None
    vv = _v3(v)
    if vv is None:
        return None
    try:
        p = gui.WorldToScreenPercent(vv)
        if p is None:
            return None
        u, w = float(p[0]), float(p[1])
        return (u, w) if _finite(u, w) else None
    except Exception:
        return None'''

new_proj = '''def _project(v):
    if v is None:
        return None
    vv = _v3(v)
    if vv is None:
        return None
    try:
        p = gui.WorldToScreenPercent(vv)
        if p is None:
            return None
        u, w = float(p[0]), 1.0 - float(p[1])
        return (u, w) if _finite(u, w) else None
    except Exception:
        return None'''
assert old_proj in text, 'old_proj not found'
text = text.replace(old_proj, new_proj)

# 6. In _gather, add behind-camera check
old_gather = '''            ht = _vec(head)
            if ht is None:
                continue
            if feet is None:
                feet = (ht[0], ht[1] - 1.62, ht[2])
            dist = _len(_sub(ht, origin))
            if dist <= 0.1 or dist > max_dist:
                continue'''

new_gather = '''            ht = _vec(head)
            if ht is None:
                continue
            if feet is None:
                feet = (ht[0], ht[1] - 1.62, ht[2])
            dist = _len(_sub(ht, origin))
            if dist <= 0.1 or dist > max_dist:
                continue
            to_head = _sub(ht, origin)
            cam_dir = _cam_dir(cam)
            if cam_dir is not None and _dot(to_head, cam_dir) <= 0.05:
                continue'''
assert old_gather in text, 'old_gather not found'
text = text.replace(old_gather, new_gather)

# 7. Update _aim: clean aimbot, remove flickbot completely
old_aim = '''def _aim(cfg, items, cam, origin, menu_open, dt=0.0166667):
    global _flick_prev, _last_flick, _fire_release_at
    global _aim_prev, _aim_latched, _flick_queue, _aim_busy, _path
    _aim_busy = False
    if not items or cam is None or origin is None:
        _path = None
        return
    aim_key = int(cfg[IDX_AIM_KEY])
    flick_key = int(cfg[IDX_FLICK_KEY])
    aim_enabled = _flag(cfg, F_AIM)
    aim_down = aim_enabled and _key_down(aim_key)
    if _opt_aim_toggle(cfg) and aim_enabled:
        if aim_down and not _aim_prev:
            _aim_latched = not _aim_latched
        aim_on = _aim_latched
    else:
        _aim_latched = False
        aim_on = aim_down
    _aim_prev = aim_down
    flick_on = _flag(cfg, F_FLICK) and _key_down(flick_key)
    trigger_flick = flick_on and not _flick_prev
    _flick_prev = flick_on
    if menu_open:
        aim_on = False
        trigger_flick = False
        _aim_latched = False
        _flick_queue = None
        _path = None
    cam_dir = _cam_dir(cam)
    if cam_dir is None:
        return

    now = time.time()
    smooth = _flick_smooth(cfg)

    if _path is not None and _path.get('kind') == 'flick':
        node, done, p = _path_advance(dt)
        _aim_busy = True
        if node is not None:
            _rotate_node(cam_dir, node, dt, 0.8)
        if done:
            if p is not None and p.get('fire') and not p.get('fired'):
                p['fired'] = True
                _fire_native(True)
                _mouse(True)
                _fire_release_at = now + 0.09
            _aim_busy = False
        return

    if _path is not None and _path.get('kind') == 'aim':
        if not aim_on:
            _path = None
        else:
            node, done, p = _path_advance(dt)
            if node is not None:
                _aim_busy = _rotate_node(cam_dir, node, dt, 0.30)
            if not done:
                return
            cd = _cam_dir(cam)
            if cd is not None:
                cam_dir = cd

    if trigger_flick:
        if now - _last_flick >= 0.12:
            fov = max(1.0, float(cfg[IDX_FLICK_FOV]))
            max_dist = float(cfg[IDX_AIM_MAX_DIST])
            best = _pick(items, origin, cam_dir, fov, max_dist, cfg, 0, True, True, force_head=True)
            if best is not None:
                e, tt, td, dist, dot = best
                want_fire = _flag(cfg, F_FLICK_FIRE)
                _last_flick = now
                if smooth <= 0.01:
                    _rotate_node(cam_dir, td, dt, 0.8)
                    _aim_busy = True
                    if want_fire:
                        _fire_native(True)
                        _mouse(True)
                        _fire_release_at = now + 0.09
                else:
                    angle = math.acos(max(-1.0, min(1.0, _dot(_norm(cam_dir), _norm(td)))))
                    curve = random.uniform(-1.0, 1.0) * (0.025 + min(0.055, angle * 0.12))
                    _begin_path(cam_dir, td, 0.04 + smooth * 0.30, curve, 'flick',
                                target_e=e, fire=want_fire)
                    _aim_busy = True
        return

    if aim_on:
        bone = int(cfg[IDX_AIM_BONE])
        max_dist = float(cfg[IDX_AIM_MAX_DIST])
        best = _pick(items, origin, cam_dir, float(cfg[IDX_AIM_FOV]), max_dist, cfg, bone, True, True)
        if best is None:
            _path = None
            return
        e, tt, td, dist, dot = best
        if _path is not None and _path.get('target') is not e:
            _path = None
        if _path is None:
            if _dot(_norm(cam_dir), _norm(td)) > 0.9999:
                _aim_busy = False
                return
            curve = random.uniform(-1.0, 1.0) * 0.006
            _begin_path(cam_dir, td, _aim_duration(cfg[IDX_AIM_SMOOTH]), curve, 'aim', target_e=e)
        node, done, p = _path_advance(dt)
        if node is not None:
            _aim_busy = _rotate_node(cam_dir, node, dt, 0.30)
        if done:
            cd = _cam_dir(cam)
            if cd is not None and _dot(_norm(cd), _norm(td)) > 0.9995:
                _aim_busy = False'''

new_aim = '''def _aim(cfg, items, cam, origin, menu_open, dt=0.0166667):
    global _aim_prev, _aim_latched, _aim_busy, _path
    _aim_busy = False
    if not items or cam is None or origin is None:
        _path = None
        return
    aim_key = int(cfg[IDX_AIM_KEY])
    aim_enabled = _flag(cfg, F_AIM)
    aim_down = aim_enabled and _key_down(aim_key)
    if _opt_aim_toggle(cfg) and aim_enabled:
        if aim_down and not _aim_prev:
            _aim_latched = not _aim_latched
        aim_on = _aim_latched
    else:
        _aim_latched = False
        aim_on = aim_down
    _aim_prev = aim_down
    if menu_open:
        aim_on = False
        _aim_latched = False
        _path = None
    cam_dir = _cam_dir(cam)
    if cam_dir is None:
        return

    if _path is not None and _path.get('kind') == 'aim':
        if not aim_on:
            _path = None
        else:
            node, done, p = _path_advance(dt)
            if node is not None:
                _aim_busy = _rotate_node(cam_dir, node, dt, 0.30)
            if not done:
                return
            cd = _cam_dir(cam)
            if cd is not None:
                cam_dir = cd

    if aim_on:
        bone = int(cfg[IDX_AIM_BONE])
        max_dist = float(cfg[IDX_AIM_MAX_DIST])
        best = _pick(items, origin, cam_dir, float(cfg[IDX_AIM_FOV]), max_dist, cfg, bone, True, True)
        if best is None:
            _path = None
            return
        e, tt, td, dist, dot = best
        if _path is not None and _path.get('target') is not e:
            _path = None
        if _path is None:
            if _dot(_norm(cam_dir), _norm(td)) > 0.9999:
                _aim_busy = False
                return
            curve = random.uniform(-1.0, 1.0) * 0.006
            _begin_path(cam_dir, td, _aim_duration(cfg[IDX_AIM_SMOOTH]), curve, 'aim', target_e=e)
        node, done, p = _path_advance(dt)
        if node is not None:
            _aim_busy = _rotate_node(cam_dir, node, dt, 0.30)
        if done:
            cd = _cam_dir(cam)
            if cd is not None and _dot(_norm(cd), _norm(td)) > 0.9995:
                _aim_busy = False'''
assert old_aim in text, 'old_aim not found'
text = text.replace(old_aim, new_aim)

# 8. Update _apply_skin and _apply
old_apply = '''def _apply_skin(entity, item, typ):
    import gclient.data.gun_skin_template_data as gun
    try:
        proto = gun.data.get(item)
    except Exception:
        proto = None
    if proto is None:
        return ''
    diag = (typ, item)
    if diag not in _skin_diag:
        _skin_diag.add(diag)
        try:
            _log('skin proto item=%d typ=%d keys=%s' % (item, typ, sorted(proto.keys())[:24]))
        except Exception:
            pass
    try:
        gid = int(proto.get('gun_id') or 0)
    except Exception:
        gid = 0
    tid = 0
    for k in ('guise_template_id', 'skin_template_id', 'skin_id', 'template_id', 'guise_item_id'):
        try:
            tid = int(proto.get(k) or 0)
        except Exception:
            tid = 0
        if tid:
            break
    wc = _find_case(entity, gid)
    if wc is None:
        cur = _current_case(entity)
        if typ == 3:
            if cur is not None:
                g = int(getattr(cur, 'gun_id', 0) or 0)
                wid = int(getattr(cur, 'weapon_id', 0) or 0)
                if _melee_gid(g, wid):
                    wc = cur
        else:
            wc = cur
    use_gid = gid
    if typ == 3 and wc is not None:
        try:
            use_gid = int(getattr(wc, 'gun_id', 0) or gid)
        except Exception:
            pass

    targets = []
    for t in (getattr(genv, 'avatar', None), entity, getattr(genv, 'player', None)):
        if t is not None and all(t is not x for x in targets):
            targets.append(t)

    names = ('ArmGunSkin', 'ChangeWeaponSkin', 'SetGunSkin', 'EquipGunSkin',
             'ApplyWeaponSkin', 'SetWeaponSkin', 'ApplyGunSkin')
    variants = ((use_gid, item), (item, use_gid), (item,), (use_gid, item, True),
                (item, True), (item, False))
    for target in targets:
        via = _call_path(target, names, variants)
        if via:
            return 'avatar.' + via

    model = getattr(entity, 'model', None) if entity is not None else None
    if model is not None:
        via = _call_path(model, ('ChangeSkin', 'SetSkin', 'ChangeGuise', 'SetGuise',
                                 'ApplySkin', 'RefreshSkin', 'RefreshGuise'),
                         ((item,), (tid or item,), (item, True), ()))
        if via:
            return 'model.' + via

    if wc is not None:
        for attr, val in (('guise_item_id', item), ('guise_template_id', tid or item),
                          ('skin_id', item), ('skin_template_id', tid or item)):
            try:
                setattr(wc, attr, val)
            except Exception:
                pass
        via = _call_path(wc, ('ChangeWeaponSkin', 'SetGuiseItem', 'ChangeGuiseItem',
                              'EquipGuise', 'SetSkin', 'ChangeSkin', 'EquipSkin',
                              'ApplySkin', 'SetGuise', 'ChangeGuise'),
                         ((item,), (use_gid, item), (item, True), (tid or item,)))
        if via:
            return 'case.' + via
        via = _call_path(wc, ('RefreshSkin', 'RefreshGuise', 'RefreshWeapon', 'RefreshModel',
                              'Refresh', 'UpdateSkin', 'UpdateGuise', 'UpdateModel',
                              'OnGuiseChange', 'OnSkinChange'),
                         ((), (item,)))
        if via:
            return 'case.' + via
        try:
            _log('skin case methods=%s' % [n for n in dir(wc)
                 if any(x in n.lower() for x in ('skin', 'guise', 'refresh', 'equip', 'model'))][:40])
        except Exception:
            pass
    else:
        _log('skin case not found item=%d gid=%d typ=%d' % (item, gid, typ))
    for target in targets:
        try:
            _log('skin target %s methods=%s' % (type(target).__name__, [n for n in dir(target)
                 if any(x in n.lower() for x in ('skin', 'guise', 'refresh', 'equip', 'weapon'))][:40]))
        except Exception:
            pass
    return '''''

new_apply = '''def _apply_skin(entity, item, typ):
    import gclient.data.gun_skin_template_data as gun
    try:
        proto = gun.data.get(item)
    except Exception:
        proto = None
    if proto is None:
        return ''
    try:
        gid = int(proto.get('gun_id') or 0)
    except Exception:
        gid = 0
    tid = 0
    for k in ('guise_template_id', 'skin_template_id', 'skin_id', 'template_id', 'guise_item_id'):
        try:
            tid = int(proto.get(k) or 0)
        except Exception:
            tid = 0
        if tid:
            break
    if not tid:
        tid = item

    targets_wc = []
    seen_ids = set()
    def add_wc(c):
        if c is not None and id(c) not in seen_ids:
            seen_ids.add(id(c))
            targets_wc.append(c)

    hm = getattr(entity, 'hand_model', None)
    if hm is not None:
        for fn_name in ('GetCurClientWeaponCase', 'GetCurWeaponCase', 'GetWeaponCase'):
            try:
                fn = getattr(hm, fn_name, None)
                if callable(fn):
                    add_wc(fn())
            except Exception:
                pass

    for fn_name in ('GetCurWeaponCase', 'GetCurClientWeaponCase', 'GetWeaponCase'):
        try:
            fn = getattr(entity, fn_name, None)
            if callable(fn):
                add_wc(fn())
        except Exception:
            pass

    mc = _find_case(entity, gid)
    if mc is not None:
        add_wc(mc)

    for c in _all_cases(entity):
        add_wc(c)

    applied_via = ''

    for wc in targets_wc:
        for attr, val in (('guise_item_id', item), ('guise_template_id', tid),
                          ('skin_id', item), ('skin_template_id', tid),
                          ('gun_skin_id', item)):
            try:
                setattr(wc, attr, val)
            except Exception:
                pass
        via = _call_path(wc, ('ChangeWeaponSkin', 'SetGuiseItem', 'ChangeGuiseItem',
                              'EquipGuise', 'SetSkin', 'ChangeSkin', 'EquipSkin',
                              'ApplySkin', 'SetGuise', 'ChangeGuise', 'InitModel'),
                         ((item,), (gid, item), (item, True), (tid,), (item, False), (gid, item, True)))
        if via:
            applied_via = 'case.' + via
        ref = _call_path(wc, ('RefreshSkin', 'RefreshGuise', 'RefreshWeapon', 'RefreshModel',
                              'Refresh', 'UpdateSkin', 'UpdateGuise', 'UpdateModel',
                              'OnGuiseChange', 'OnSkinChange'),
                         ((), (item,)))
        if ref and not applied_via:
            applied_via = 'case.' + ref

    for fn_name in ('GetCurWeapon', 'cur_weapon'):
        try:
            w = getattr(entity, fn_name, None)
            if callable(w):
                w = w()
            if w is not None:
                for attr, val in (('guise_item_id', item), ('skin_id', item), ('skin_template_id', tid)):
                    try:
                        setattr(w, attr, val)
                    except Exception:
                        pass
        except Exception:
            pass

    if hm is not None:
        _call_path(hm, ('RefreshWeapon', 'ResetStatus', 'OnGuiseChange', 'UpdateWeaponSkin', 'RefreshModel'), ((), (item,)))

    doll = _find_doll()
    if doll is not None:
        for attr, val in (('guise_item_id', item), ('skin_id', item)):
            try:
                setattr(doll, attr, val)
            except Exception:
                pass
        dm = getattr(doll, 'model', None)
        if dm is not None:
            _call_path(dm, ('ChangeSkin', 'SetSkin', 'ChangeGuise', 'SetGuise', 'RefreshSkin', 'RefreshModel'),
                       ((item,), (tid,), (item, True), ()))

    for target in (getattr(genv, 'avatar', None), entity, getattr(genv, 'player', None)):
        if target is not None:
            via = _call_path(target, ('ArmGunSkin', 'ChangeWeaponSkin', 'SetGunSkin', 'EquipGunSkin',
                                      'ApplyWeaponSkin', 'SetWeaponSkin'),
                             ((gid, item), (item, gid), (item,), (gid, item, True), (item, True)))
            if via and not applied_via:
                applied_via = 'avatar.' + via

    return applied_via or ('case.applied' if targets_wc else '')'''
assert old_apply in text, 'old_apply not found'
text = text.replace(old_apply, new_apply)

# 9. Update _apply function
old_apply_fn = '''        if typ == 1:
            import gclient.data.lobby_item_data as lobby
            proto = lobby.data.get(item)
            want = _hero_of(proto)
            cur = _local_hero_id()
            if _flag(cfg, F_SKIN_CURRENT) and cur and want and cur != want:
                _last_apply_seq = seq
                _log('skin rejected hero=%d current=%d item=%d (disable Current hero only)'
                     % (want, cur, item))
                return
            via = _apply_hero(entity, item)
        else:
            via = _apply_skin(entity, item, typ)'''

new_apply_fn = '''        if typ == 1:
            via = _apply_hero(entity, item)
        else:
            via = _apply_skin(entity, item, typ)'''
assert old_apply_fn in text, 'old_apply_fn not found'
text = text.replace(old_apply_fn, new_apply_fn)

# 10. Update tick to update _cached_cfg and handle F_EXIT_CHEAT
old_tick = '''    try:
        cfg = _read_cfg()
        if cfg is None:
            return'''

new_tick = '''    try:
        cfg = _read_cfg()
        if cfg is None:
            return
        global _cached_cfg
        _cached_cfg = cfg
        if _flag(cfg, F_EXIT_CHEAT):
            stop()
            return'''
assert old_tick in text, 'old_tick not found'
text = text.replace(old_tick, new_tick)

# 11. In tick, remove flick from aim_active
old_aim_act = '''        aim_active = _flag(cfg, F_AIM) or _flag(cfg, F_FLICK)'''
new_aim_act = '''        aim_active = _flag(cfg, F_AIM)'''
assert old_aim_act in text, 'old_aim_act not found'
text = text.replace(old_aim_act, new_aim_act)

# 12. Add stop() function
old_status = '''def status():
    cfg = _read_cfg()'''

new_status = '''def stop():
    global _booted
    try:
        import gclient.framework.util.story_tick as st
        st.StoryTick().Remove(tick)
    except Exception:
        pass
    _restore_hooks()
    _booted = False
    try:
        _log('bs_runtime stopped cleanly')
    except Exception:
        pass


def status():
    cfg = _read_cfg()'''
assert old_status in text, 'old_status not found'
text = text.replace(old_status, new_status)

# 13. In status(), remove flick
old_status_dict = '''        'flick': _flag(cfg, F_FLICK),'''
assert old_status_dict in text, 'old_status_dict not found'
text = text.replace(old_status_dict, '')

if crlf:
    text = text.replace('\n', '\r\n')

with open('bs-cheat/pyexec/bs_runtime.py', 'w', encoding='utf-8') as f:
    f.write(text)

print('Updated bs_runtime.py successfully!')
