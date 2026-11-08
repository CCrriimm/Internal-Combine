import builtins
import os
import sys
import struct
import time
import math
import random
import traceback
import threading

try:
    import MUI
except Exception:
    MUI = None

_import_order_moved = False


def _meta_path_names():
    names = []
    for finder in list(sys.meta_path):
        try:
            names.append('%s.%s' % (type(finder).__module__, type(finder).__name__))
        except Exception:
            names.append('?')
    return names


def _finder_raises_for_anything(finder):
    """True when this finder raises TypeError instead of saying "not here".

    The engine's finder is what does it, but its class name is not a reliable
    identifier across builds (`MImporter.finder` in one, something else in
    another), so the test is behavioural: ask it about a module that cannot
    exist. A working finder returns None; the broken one raises.
    """
    probe = getattr(finder, 'find_spec', None)
    if probe is None:
        return False
    try:
        probe('__comet_import_probe__')
        return False
    except TypeError as exc:
        # Deliberately narrow. A finder with a different signature raises TypeError
        # too ("missing 1 required positional argument"), and demoting a working
        # finder for that would be a change for no reason. The engine's failure is
        # recognisable by its message.
        text = str(exc)
        return 'NoneType' in text or 'unsupported operand' in text
    except BaseException:
        return False


def _repair_import_order():
    """Move finders that raise for unknown modules to the end of sys.meta_path.

    On a damaged install `MImporter.finder.find_spec` raises

        TypeError: unsupported operand type(s) for +: 'NoneType' and 'str'
          (find_spec_in_patch_encrypt -> get_relpath_with_path)

    for any module that is not in the patch index. It sits at the front of
    sys.meta_path, so it is asked about every import first - standard library
    modules included - and a finder that raises aborts the import instead of
    letting the next finder try. `import ctypes` failing there is why the shared
    memory never came up, which is why the overlay had to read state.bin from
    disk on every frame, which is what the runtime then blocked ~6 s on while
    writing the same file.

    Nothing is replaced or wrapped: the finder stays in the chain and is still
    asked for the game modules that only it can resolve - it is simply asked
    after the finders that work by path.
    """
    global _import_order_moved
    try:
        order = list(sys.meta_path)
    except Exception:
        return False

    offenders = []
    rest = []
    for finder in order:
        cls = type(finder)
        name = ((getattr(cls, '__module__', '') or '') + '.' +
                (getattr(cls, '__name__', '') or '')).lower()
        named = 'importer' in name or 'finder_encrypt' in name
        if named or _finder_raises_for_anything(finder):
            offenders.append(finder)
        else:
            rest.append(finder)

    if not offenders:
        return False
    if order[len(rest):] == offenders:
        return False
    try:
        sys.meta_path[:] = rest + offenders
    except Exception:
        return False
    _import_order_moved = True
    return True


def _load_ctypes():
    """ctypes, from wherever it can be had. Never raises."""
    module = sys.modules.get('ctypes')
    if module is not None:
        return module
    try:
        import ctypes as module
        return module
    except Exception:
        return None


try:
    _repair_import_order()
    if _import_order_moved:
        # Loaded now, on purpose: once it is in sys.modules nothing can take it
        # away, whatever the engine's finder does next.
        _early_ctypes = _load_ctypes()
        if _early_ctypes is not None:
            sys.modules.setdefault('ctypes', _early_ctypes)
except Exception:
    pass


def _log_import_repair():
    """One line that says whether the import chain is usable - and if not, why."""
    try:
        ctypes_ok = 'ctypes' in sys.modules or _load_ctypes() is not None
        if globals().get('_import_order_moved') and ctypes_ok:
            _sys_log('[sys:boot] engine finder moved last, ctypes ok: %s'
                     % ','.join(_meta_path_names()), force=True)
        elif ctypes_ok:
            _sys_log('[sys:boot] ctypes ok, no finder needed moving: %s'
                     % ','.join(_meta_path_names()), force=True)
        else:
            _sys_log('[sys:boot] ctypes UNAVAILABLE, meta_path: %s'
                     % ','.join(_meta_path_names()), force=True)
    except Exception:
        pass


def _root_dir():
    for key in ('BS_ROOT', 'BS_CHEAT_ROOT'):
        v = os.environ.get(key)
        if v and os.path.isdir(v):
            return os.path.abspath(v)
    here = os.path.dirname(os.path.abspath(globals().get('__file__') or '.'))
    cand = os.path.abspath(os.path.join(here, '..'))
    if os.path.isdir(os.path.join(cand, 'ipc')) or os.path.basename(here).lower() == 'pyexec':
        return cand
    return here


BS_ROOT = _root_dir()
IPC_DIR = os.path.join(BS_ROOT, 'ipc')
try:
    os.makedirs(IPC_DIR, exist_ok=True)
except Exception:
    pass
STATE_PATH = os.path.join(IPC_DIR, 'state.bin')
CFG_PATH = os.path.join(IPC_DIR, 'cfg.bin')
CATALOG_PATH = os.path.join(IPC_DIR, 'catalog.bin')
ERR_PATH = os.path.join(IPC_DIR, 'runtime_err.txt')

CFG_MAGIC = 0x42534346
STATE_MAGIC = 0x42535331
CATALOG_MAGIC = 0x42534354
VERSION = 11
MAX_ENTITIES = 128

# Cfg grew at the tail with the Comet port (team colours, armor tiers,
# options word, noise). Everything before field 37 keeps its original offset, so
# only the parser had to learn the new tail - the on-disk file is written by the
# overlay, this is only ever read.
CFG_FMT = '<IIIIIiIiIifffffiiI' + 'I' * 13 + 'f' + 'I' * 19 + 'ff' + 'I' * 3 + 'i'
STATE_FMT = '<IIIIQ14fBBH'
# Ent grew by hero_id + is_dying (56 -> 58 packed fields, 280 -> 288 bytes).
ENT_FMT = '<Q64s32s3f3f2f2f5fi4BI48fII'   # 64-byte UTF-8 name: CJK + emoji; 16 bones x world xyz
CAT_HEAD_FMT = '<IIII'
CAT_ENTRY_FMT = '<IIII48s'
# Ground-loot publishing was removed at the user's request: the marker read as
# flicker and the scan was measurable work inside the game's tick. The packet
# still carries an empty trailer so the overlay's reader stays in step.
LOOT_MAX = 64
LOOT_FMT = '<Q2ffI'
LOOT_SIZE = struct.calcsize(LOOT_FMT)
LOOT_HEAD = 4

CFG_SIZE = struct.calcsize(CFG_FMT)
STATE_HEAD_SIZE = struct.calcsize(STATE_FMT)
ENT_SIZE = struct.calcsize(ENT_FMT)
CAT_HEAD_SIZE = struct.calcsize(CAT_HEAD_FMT)
CAT_ENTRY_SIZE = struct.calcsize(CAT_ENTRY_FMT)
STATE_SIZE = STATE_HEAD_SIZE + ENT_SIZE * MAX_ENTITIES + LOOT_HEAD + LOOT_SIZE * LOOT_MAX
MAX_HERO = 4096
MAX_GUN = 65536

assert CFG_SIZE == 228
assert STATE_HEAD_SIZE == 84
assert ENT_SIZE == 288
assert CAT_ENTRY_SIZE == 64
assert LOOT_SIZE == 24

IDX_MAGIC = 0
IDX_VERSION = 1
IDX_SEQ = 2
IDX_FLAGS = 3
IDX_APPLY_SEQ = 4
IDX_APPLY_TYPE = 5
IDX_APPLY_ITEM = 6
IDX_TRACK_KEY = 7
IDX_COL_FOV = 8
IDX_MENU_KEY = 9
IDX_TRACK_FOV = 10
IDX_TRACK_SMOOTH = 11
IDX_TRACK_MAX_DIST = 12
IDX_HUMAN_SMOOTH = 13
IDX_HUMANIZE = 14
IDX_TRACK_JOINT = 15
IDX_RENDER_MAX_DIST = 16
IDX_OPTIONS = 17
IDX_COL_BOX = 18
IDX_COL_BOX_HIDDEN = 19
IDX_COL_TEAM = 20
IDX_COL_MONSTER = 21
IDX_COL_SKEL = 22
IDX_COL_LINE = 23
IDX_COL_NAME = 24
IDX_COL_GUN = 25
IDX_COL_DIST = 26
IDX_COL_HEALTH = 27
IDX_COL_HEADDOT = 28
IDX_COL_COUNT = 29
IDX_RENDER_STYLE = 30
IDX_BOX_THICK = 31
IDX_THEME_HERO = 32
IDX_THEME_GUN = 33
IDX_THEME_KNIFE = 34
IDX_AUTO_THEME = 35
IDX_COL_ARMOR = 36
# Comet port tail (see Cfg in src/bs_cfg.hpp). Only the fields the runtime
# reads are named; 37-49 are the twelve colour slots (col_team_box[4],
# col_team_line[4], col_armor_tier[4], col_loot) and 53-55 are Cfg::pad.
IDX_OPTS = 50
IDX_LOOT_MAX_DIST = 51
IDX_NOISE_STRENGTH = 52
IDX_AIM_KEY2 = 56

# Secondary option word (`Cfg::opts`). Only the bits the runtime acts on are
# defined here; OPT2_ESP_HERO / TEAM_LABEL / TEAM_COLORS / ESP_ARMOR_MAX are
# The overlay owns the capture guard; the runtime only has to publish the
# default so a fresh config starts protected.
# OPT2_ESP_LOOT (1 << 1) is retired: no ground-loot ESP any more.
OPT2_OBS_PROTECT = 1 << 5
OPT2_AIM_NOISE = 1 << 6

OPT_TRACK_TOGGLE = 1 << 0
OPT_TRACK_IGNORE_DOWNED = 1 << 1
OPT_TRACK_JOINTS_SET = 1 << 2
OPT_RAY_REDIRECT = 1 << 5
OPT_TRACK_HEAD = 1 << 8
OPT_TRACK_NECK = 1 << 9
OPT_TRACK_BODY = 1 << 10
F_TRACK_MULTIPOINT = 1 << 24

F_MENU_OPEN = 1 << 0
F_RENDER_ENTITIES = 1 << 1
F_RENDER_BOUNDS = 1 << 2
F_RENDER_SKELETON = 1 << 3
F_RENDER_TRACER = 1 << 4
F_RENDER_LABEL = 1 << 5
F_RENDER_EQUIPMENT = 1 << 6
F_RENDER_RANGE = 1 << 7
F_RENDER_VITALITY = 1 << 8
F_RENDER_HEAD_MARKER = 1 << 9
F_RENDER_FACTION = 1 << 10
F_RENDER_OCCLUSION = 1 << 11
F_RENDER_SUMMARY = 1 << 12
F_TRACK_VIEW = 1 << 13
F_TRACK_OCCLUSION = 1 << 14
F_TRACK_FACTION = 1 << 15
F_TRACK_FOV_INDICATOR = 1 << 16
F_TRACK_HUMANIZE = 1 << 17
F_TRACK_LEAD = 1 << 18
F_RENDER_SHIELD = 1 << 19
F_UNLOCK_EFFECTS = 1 << 20
F_BLOCK_REPORT = 1 << 21
F_THEME_PRESERVE = 1 << 22
F_DAMP_IMPULSE = 1 << 23
F_FORCE_KILLFX = 1 << 25
F_EXIT_CHEAT = 1 << 26

DEFAULT_FLAGS = (F_MENU_OPEN | F_RENDER_ENTITIES | F_RENDER_BOUNDS | F_RENDER_LABEL | F_RENDER_RANGE |
                 F_RENDER_VITALITY | F_RENDER_SHIELD | F_RENDER_HEAD_MARKER | F_TRACK_FOV_INDICATOR |
                 F_UNLOCK_EFFECTS | F_BLOCK_REPORT | F_THEME_PRESERVE |
                 F_DAMP_IMPULSE | F_RENDER_SUMMARY | (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30))

DEFAULT_CFG = (
    CFG_MAGIC, VERSION, 0, DEFAULT_FLAGS,
    0, 0, 0,
    0x02, 0xB4FFA259, 0x2D,
    8.0, 0.35, 400.0, 0.5, 0.90,
    0, 400, OPT_TRACK_JOINTS_SET | OPT_TRACK_HEAD | (1 << 4),
    4036750176, 3948567280, 3707742810, 3947940095,
    3952864096, 3198316889, 4294045164, 3957250262,
    4040674760, 4036750176, 3959422975, 4288408416,
    0, 1.5,
    0, 0, 0, 1,
    4043289680,
    # col_team_box[4] - identical values to DefaultCfg() in src/overlay.cpp
    3959401050, 3946815231, 3950244467, 3959390169,
    # col_team_line[4]
    3372198490, 3359612671, 3363041907, 3372187609,
    # col_armor_tier[4] (grey / blue / purple / gold)
    4120810636, 4127165491, 4127150765, 4112499455,
    # col_loot, opts, loot_max_dist, noise_strength, pad[3] (= Cfg.pad)
    4127188531, OPT2_OBS_PROTECT, 120.0, 0.0016,
    0, 0, 0,
    0,   # aim_key2: second aim key, 0 = unbound
)

_runtime = None
_state_seq = 0
_last_state = 0.0
_last_apply_seq = -1
_last_apply_try = 0.0
_last_catalog_check = 0.0
_last_err = 0.0
_flick_prev = False
_last_flick = 0.0
_fire_release_at = 0.0
_track_prev = False
_track_latched = False
_track_hold_until = 0.0
_track_diag_t = 0.0
_track_fallback_n = 0
_track_sign_y = -1.0
_track_sign_p = -1.0
_track_sign_ready = False
_track_eid = None
_track_eid_t = 0.0
_track_hold_log_t = 0.0
_perf_t = 0.0
_perf_last = 0.0
# Per-tick cost, averaged. The [sys:perf] line above only fires for a tick over
# 60 ms, which says nothing about the tick that runs sixty times a second on the
# game thread - and that one is the only place the runtime could be costing the
# user frames. Summed every tick (two float adds) and logged every 10 s.
_perf_phase_sum = [0.0, 0.0, 0.0, 0.0, 0.0]
_perf_bone_sum = 0.0
_appearance_sync_t = 0.0
_damping_t = 0.0
_perf_items = 0
_unlock_t = 0.0
_g_bone_t = 0.0
_perf_tick_window = 0.0
_perf_tick_count = 0
_perf_tick_sum = 0.0
_perf_tick_max = 0.0
_perf_tick_dt = 0.0
_state_file_t = 0.0
_g_log_t = 0.0
_g_slow = 0.0
_flick_queue = None
_track_busy = False
_jit_t = 0.0
_impulse_t = 0.0
_impulse_grace = 0.0
_impulse_hooks = None
_ray_target = None
_ray_hook = None
_vis_cache = {}
_melee_cache = {}
_gather_cache = (0.0, None, None, [])
_last_gather = 0.0
_last_hooks = 0.0
_last_scrub = 0.0
_bone_cache = {}
_gun_cache = {}
_diag_t = 0.0
_ent_list_cache = None
_apply_tries = 0
_apply_seq_seen = -1
_asset_diag = set()
_hooks_done = False
_hook_avatar_id = None
_booted = False
_model_to_hero = {}
_unlock_cache = {}
_unlock_done = False
_unlock_iter = None
_unlock_pred_iter = None
_unlock_step_t = 0.0
_unlock_scan_t = 0.0
_unlock_changed = 0
_wrapped_preds = set()
_wrapped_logged = False
_last_unlock = 0.0
_my_gen = 0
_asset_auto_seen = None
_appearance_task = []
_appearance_task_t = 0.0
_appearance_fail = {}
_asset_last_sync = 0.0
_asset_preserve_active = False

_cfg_cache_vals = DEFAULT_CFG
_cfg_cache_mtime = -1
_cfg_cache_t = 0.0
_local_cache = None
_hero_id_cache = None
_class_hooks_at = 0.0
_log_last_msg = None
_log_last_t = 0.0
_path = None

HERO_NAMES = {
    101: 'Ethan', 102: 'Jet', 103: 'Nova', 104: 'Val', 105: 'Hank',
    106: 'Spike', 107: 'E.M.', 108: 'Krak', 110: 'Ran', 111: 'Nach',
    112: 'Blas', 113: 'Zero', 114: 'Volt', 115: 'Emma', 116: 'Kain',
    117: 'Katy', 119: 'Jack', 122: 'Luci', 124: 'Alon', 125: 'Vox',
}


def _sys_log(msg, force=False):
    global _last_err, _log_last_msg, _log_last_t
    now = time.time()
    text = str(msg)[:1200]
    if text == _log_last_msg and now - _log_last_t < 10.0:
        return
    # The 0.5 s throttle keeps a busy hook from flooding the file, but it also hid
    # one-off lines that matter (boot, IPC, guard install) behind asset spam.
    if not force and now - _last_err < 0.5:
        return
    _last_err = now
    _log_last_msg = text
    _log_last_t = now
    try:
        try:
            if os.path.getsize(ERR_PATH) > 2 * 1024 * 1024:
                os.remove(ERR_PATH)
        except Exception:
            pass
        with open(ERR_PATH, 'a', encoding='utf-8', errors='replace') as f:
            f.write(time.strftime('%H:%M:%S ') + text + '\n')
    except Exception:
        pass


BONE_NAMES = [
    ('biped Head', 'Bip001 Head', 'Head'),                                    # 0: Head
    ('biped Neck', 'Bip001 Neck', 'Neck'),                                    # 1: Neck
    ('biped Spine1', 'biped Spine', 'Bip001 Spine2', 'Spine'),                # 2: Spine
    ('biped Pelvis', 'Bip001 Pelvis', 'biped Hip', 'Bip001 Hip', 'Pelvis'),   # 3: Pelvis
    ('biped L UpperArm', 'Bip001 L UpperArm', 'L UpperArm', 'LUpperArm'),    # 4: L Shoulder
    ('biped L Forearm', 'Bip001 L Forearm', 'L Forearm', 'LForeArm'),         # 5: L Elbow
    ('biped L Hand', 'Bip001 L Hand', 'L Hand', 'LHand'),                     # 6: L Hand
    ('biped R UpperArm', 'Bip001 R UpperArm', 'R UpperArm', 'RUpperArm'),    # 7: R Shoulder
    ('biped R Forearm', 'Bip001 R Forearm', 'R Forearm', 'RForeArm'),         # 8: R Elbow
    ('biped R Hand', 'Bip001 R Hand', 'R Hand', 'RHand'),                     # 9: R Hand
    ('biped L Thigh', 'Bip001 L Thigh', 'L Thigh', 'LThigh'),                 # 10: L Hip
    ('biped L Calf', 'Bip001 L Calf', 'L Calf', 'LCalf'),                     # 11: L Knee
    ('biped L Foot', 'Bip001 L Foot', 'L Foot', 'LFoot'),                     # 12: L Foot
    ('biped R Thigh', 'Bip001 R Thigh', 'R Thigh', 'RThigh'),                 # 13: R Hip
    ('biped R Calf', 'Bip001 R Calf', 'R Calf', 'RCalf'),                     # 14: R Knee
    ('biped R Foot', 'Bip001 R Foot', 'R Foot', 'RFoot')                      # 15: R Foot
]

try:
    _log_import_repair()
except Exception:
    pass


def _close(a, b, tol=1e-9):
    return a is not None and b is not None and abs(a - b) < tol


def _flag(cfg, bit):
    try:
        return bool(int(cfg[IDX_FLAGS]) & bit)
    except Exception:
        return False


def _options(cfg):
    try:
        return int(cfg[IDX_OPTIONS])
    except Exception:
        return 0


def _opt2(cfg, bit):
    """Secondary option word. Older/partial config tuples simply read as off."""
    try:
        return bool(int(cfg[IDX_OPTS]) & bit)
    except Exception:
        return False


def _noise_strength(cfg):
    try:
        value = float(cfg[IDX_NOISE_STRENGTH])
    except Exception:
        return 0.0
    if not math.isfinite(value) or value <= 0.0:
        return 0.0
    return min(0.008, max(0.0, value))


def _get_track_mode_toggle(cfg):
    return bool(_options(cfg) & OPT_TRACK_TOGGLE)


def _view_snap_smooth(cfg):
    try:
        return ((_options(cfg) >> 8) & 0xFF) / 100.0
    except Exception:
        return 0.0


def _vec(v):
    if v is None:
        return None
    try:
        t = v.tuple()
        return (float(t[0]), float(t[1]), float(t[2]))
    except Exception:
        pass
    try:
        return (float(v.x), float(v.y), float(v.z))
    except Exception:
        pass
    try:
        return (float(v[0]), float(v[1]), float(v[2]))
    except Exception:
        return None


_V3_CLASS = None


def _v3(v):
    t = _vec(v)
    if t is None:
        return None
    global _V3_CLASS
    try:
        if _V3_CLASS is None:
            from MType import Vector3 as _V
            _V3_CLASS = _V
        return _V3_CLASS(t[0], t[1], t[2])
    except Exception:
        return t


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _len(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def _norm(a):
    n = _len(a)
    if n < 1e-9:
        return (0.0, 0.0, 0.0)
    return (a[0] / n, a[1] / n, a[2] / n)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _id_u64(v):
    try:
        return int(v) & 0xFFFFFFFFFFFFFFFF
    except Exception:
        pass
    h = 1469598103934665603
    try:
        data = str(v).encode('utf-8', 'replace')
    except Exception:
        data = b''
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def _finite(*xs):
    for x in xs:
        try:
            if not math.isfinite(float(x)):
                return False
        except Exception:
            return False
    return True


_shm_state_p = None
_shm_cfg_p = None

_shm_logged = None


def _init_ctypes_shm():
    """Map the shared-memory blocks the overlay reads each frame.

    No `ctypes.wintypes` here: on a broken install the engine's importer raises
    TypeError for any module that is not already loaded, `ctypes.wintypes` is one
    of them, and the old code swallowed the failure - so the overlay fell back to
    reading state.bin from disk on every presented frame. That is the ~5 ms/frame
    measured as `pre_avg_ms` in overlay_log.txt.
    """
    global _shm_state_p, _shm_cfg_p, _shm_logged
    if _shm_state_p is not None and _shm_cfg_p is not None:
        return
    # Never a top-level import: the engine's importer rejects modules that are not
    # already loaded on a damaged install, and a raise here at module scope takes
    # the whole runtime down (no ESP, no hotkeys, no menu). Prefer whatever is
    # already in sys.modules, and treat "no ctypes" as "no shared memory".
    ctypes = sys.modules.get('ctypes')
    if ctypes is None:
        try:
            ctypes = _load_ctypes()
        except Exception:
            ctypes = None
    if ctypes is None:
        if _shm_logged != 'missing':
            _shm_logged = 'missing'
            _sys_log('[sys:ipc] ctypes unavailable - overlay reads state.bin (slower); '
                     'meta_path: %s' % ','.join(_meta_path_names()), force=True)
        return
    try:
        k32 = ctypes.windll.kernel32
        k32.OpenFileMappingA.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_char_p]
        k32.OpenFileMappingA.restype = ctypes.c_void_p
        k32.CreateFileMappingA.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p]
        k32.CreateFileMappingA.restype = ctypes.c_void_p
        k32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_uint32, ctypes.c_size_t]
        k32.MapViewOfFile.restype = ctypes.c_void_p

        PAGE_READWRITE = 0x04
        FILE_MAP_ALL_ACCESS = 0x000F001F
        INVALID_HANDLE = ctypes.c_void_p(-1)

        if _shm_state_p is None:
            h = k32.OpenFileMappingA(FILE_MAP_ALL_ACCESS, False, b"Local\\BS_IPC_STATE")
            if not h:
                h = k32.CreateFileMappingA(INVALID_HANDLE, None, PAGE_READWRITE, 0, 65536,
                                           b"Local\\BS_IPC_STATE")
            if h:
                _shm_state_p = k32.MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 65536)

        if _shm_cfg_p is None:
            h = k32.OpenFileMappingA(FILE_MAP_ALL_ACCESS, False, b"Local\\BS_IPC_CFG")
            if not h:
                h = k32.CreateFileMappingA(INVALID_HANDLE, None, PAGE_READWRITE, 0, CFG_SIZE,
                                           b"Local\\BS_IPC_CFG")
            if h:
                _shm_cfg_p = k32.MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, CFG_SIZE)
    except Exception:
        pass

    ok = _shm_state_p is not None
    if _shm_logged != ok:
        _shm_logged = ok
        if ok:
            _sys_log('[sys:ipc] state shared memory ok', force=True)
        else:
            _sys_log('[sys:ipc] state shared memory failed - overlay reads state.bin on every '
                     'frame; retrying', force=True)


# Size the state file is padded to. Fixed on purpose: the overlay maps this file
# and reads the frames straight out of the mapping, so it must never change size
# and must never be replaced by a rename - a rename would leave the overlay with a
# mapping of the old file and a frozen ESP.
STATE_FILE_SIZE = 65536

_state_fh = None


def _write_state_inplace(buf):
    """Write the state snapshot into the same file, at offset 0, in one go.

    The alternative (temp file + os.replace) is atomic but has two costs measured
    in the user's logs: the overlay has to open/read/close state.bin on every
    presented frame (3.4-5.8 ms as `state_avg_ms`), and the rename fights that
    reader - one tick spent 5.9 s inside this write (`write=5.9421` in
    [sys:perf]) with the game thread waiting on the lock.

    Writing in place keeps the file identity stable, so the overlay's memory
    mapping stays valid and a frame costs it a memcpy instead of three syscalls.
    Readers detect a half-written snapshot with the sequence number, so a torn
    read costs one frame of data, never a bad frame.
    """
    global _state_fh
    try:
        if _state_fh is None:
            exists = os.path.exists(STATE_PATH)
            _state_fh = open(STATE_PATH, 'r+b' if exists else 'w+b')
        data = buf if len(buf) >= STATE_FILE_SIZE else buf + b'\x00' * (STATE_FILE_SIZE - len(buf))
        _state_fh.seek(0)
        _state_fh.write(data)
        _state_fh.flush()
        return True
    except Exception:
        try:
            if _state_fh is not None:
                _state_fh.close()
        except Exception:
            pass
        _state_fh = None
        return False


# Disk fallback must not block the engine tick: replace pending snapshots with
# the newest one instead of queueing unbounded old frames. The worker only sees
# immutable bytes and filesystem paths, never game/engine objects.
_state_writer_event = threading.Event()
_state_writer_lock = threading.Lock()
_state_writer_pending = None
_state_writer_thread = None


def _write_file(path, data):
    tmp = path + '.tmp'
    try:
        with open(tmp, 'wb') as f:
            f.write(data)
        for _ in range(4):
            try:
                os.replace(tmp, path)
                return True
            except OSError:
                time.sleep(0.001)
    except Exception:
        pass
    try:
        with open(path, 'wb') as f:
            f.write(data)
        return True
    except Exception:
        return False


def _state_writer_loop():
    global _state_writer_pending
    while True:
        _state_writer_event.wait()
        with _state_writer_lock:
            pending = _state_writer_pending
            _state_writer_pending = None
            _state_writer_event.clear()
        if pending is not None:
            _write_file(STATE_PATH, pending)


def _write(path, data):
    global _state_file_t, _state_writer_thread, _state_writer_pending
    if path != STATE_PATH:
        return _write_file(path, data)
    if _shm_state_p is None:
        _init_ctypes_shm()
    shared = False
    if _shm_state_p:
        try:
            import ctypes
            ctypes.memmove(_shm_state_p, data, len(data))
            shared = True
        except Exception:
            pass
    now = time.time()
    if shared and now - _state_file_t < 0.25:
        return True
    _state_file_t = now
    if not shared and _write_state_inplace(data):
        # No shared memory (ctypes unavailable) but still no rename and no
        # reader/writer fight: the overlay maps this file and reads the sequence
        # number out of the view.
        return True
    try:
        if _state_writer_thread is None or not _state_writer_thread.is_alive():
            _state_writer_thread = threading.Thread(target=_state_writer_loop, name='Comet state writer', daemon=True)
            _state_writer_thread.start()
        # Never wait for the writer. It holds this lock while it is inside the
        # filesystem, and a contended write on this machine took 5.9 s once
        # (`write=5.9421` in [sys:perf]) - the game thread waited on the lock for
        # all of it. A dropped snapshot costs one frame of data; blocking costs
        # six seconds of frames.
        if not _state_writer_lock.acquire(False):
            return False
        try:
            _state_writer_pending = bytes(data)
            _state_writer_event.set()
        finally:
            _state_writer_lock.release()
        return True
    except Exception:
        # Do not fall back to synchronous I/O on the game thread.
        return shared


def _read(path, size):
    try:
        with open(path, 'rb') as f:
            return f.read(size)
    except Exception:
        return None


def _bytes(x, n):
    if x is None:
        return b''.ljust(n, b'\x00')
    try:
        b = str(x).encode('utf-8', 'replace')[:max(0, n - 1)]
    except Exception:
        b = b''
    while b:
        try:
            b.decode('utf-8')
            break
        except UnicodeDecodeError:
            b = b[:-1]
    return b.ljust(n, b'\x00')


def _read_cfg():
    global _cfg_cache_vals, _cfg_cache_mtime, _cfg_cache_t
    now = time.time()
    if now - _cfg_cache_t < 0.033:
        return _cfg_cache_vals
    _cfg_cache_t = now
    raw = None
    if _shm_cfg_p is None:
        _init_ctypes_shm()
    if _shm_cfg_p:
        try:
            import ctypes
            candidate = ctypes.string_at(_shm_cfg_p, CFG_SIZE)
            if len(candidate) == CFG_SIZE:
                v = struct.unpack(CFG_FMT, candidate)
                if v[0] == CFG_MAGIC and v[1] == VERSION:
                    _cfg_cache_vals = v
                    return v
        except Exception:
            pass
    try:
        mtime = os.stat(CFG_PATH).st_mtime_ns
    except Exception:
        mtime = -1
    if mtime == _cfg_cache_mtime:
        return _cfg_cache_vals
    _cfg_cache_mtime = mtime
    raw = _read(CFG_PATH, CFG_SIZE)
    if raw is None or len(raw) != CFG_SIZE:
        _cfg_cache_vals = DEFAULT_CFG
        return DEFAULT_CFG
    try:
        vals = struct.unpack(CFG_FMT, raw)
        if vals[0] != CFG_MAGIC or vals[1] != VERSION:
            _cfg_cache_vals = DEFAULT_CFG
        else:
            _cfg_cache_vals = vals
    except Exception:
        _cfg_cache_vals = DEFAULT_CFG
    return _cfg_cache_vals


def _make_cfg_default():
    try:
        os.makedirs(IPC_DIR, exist_ok=True)
        raw = _read(CFG_PATH, CFG_SIZE)
        if raw is None or len(raw) != CFG_SIZE:
            _write(CFG_PATH, struct.pack(CFG_FMT, *DEFAULT_CFG))
            return
        vals = struct.unpack(CFG_FMT, raw)
        if vals[0] != CFG_MAGIC or vals[1] != VERSION:
            _write(CFG_PATH, struct.pack(CFG_FMT, *DEFAULT_CFG))
    except Exception:
        _sys_log(traceback.format_exc())


def _weapon_util():
    try:
        from gshare import weapon_util
        return weapon_util
    except Exception:
        return None


def _melee_value(value, space):
    try:
        value = int(value)
    except Exception:
        return False
    if value <= 0:
        return False
    key = (space, value)
    if key in _melee_cache:
        return _melee_cache[key]
    res = False
    wu = _weapon_util()
    if wu is not None:
        for fn_name in ('IsWeaponMelee', 'IsWeaponMeleeSharp'):
            fn = getattr(wu, fn_name, None)
            if callable(fn):
                try:
                    if bool(fn(value)):
                        res = True
                        break
                except Exception:
                    pass
        if not res:
            try:
                from gshare import consts
                gt = None
                gfn = getattr(wu, 'GetWeaponGunType', None)
                if callable(gfn):
                    gt = gfn(value)
                keys = set()
                for base_name in ('GunType', 'EquipmentType', 'WeaponType'):
                    base = getattr(consts, base_name, None)
                    if base is None:
                        continue
                    for k, v in vars(base).items():
                        if not k.isupper():
                            continue
                        if any(x in k for x in ('KNIFE', 'MELEE', 'BLADE', 'SWORD', 'HAMMER', 'AXE')):
                            keys.add(v)
                if gt is not None and keys and gt in keys:
                    res = True
            except Exception:
                pass
    _melee_cache[key] = res
    return res


def _melee_gid(gid, wid=None):
    if wid:
        return _melee_value(wid, 'w')
    return _melee_value(gid, 'g')


def _melee_name(name):
    try:
        s = str(name).lower()
    except Exception:
        return False
    for w in ('knife', 'karambit', 'dagger', 'machete', 'bayonet', 'blade', 'sword', 'axe', 'hammer',
              'kunai', 'tanto', 'cleaver', 'sickle', 'scythe', 'pickaxe', 'shovel', 'crowbar', 'wrench',
              'katana', 'rapier', 'spear', 'halberd', 'trident', 'nunchaku', 'tonfa', 'mace', 'flail',
              'chainsaw', 'bowie', 'stiletto', 'kukri', 'gauntlet', 'claw', 'baseball',
              '\u5200', '\u5251', '\u65a7', '\u9524', '\u5315', '\u68cd', '\u9570',
              '\u77db', '\u621f', '\u722a', '\u9576'):
        if w in s:
            return True
    return False


_CN_WORDS = (
    ('\u8774\u8776\u5200', 'Butterfly Knife'), ('\u519b\u5200', 'Combat Knife'),
    ('\u722a\u5200', 'Karambit'), ('\u6b66\u58eb\u5200', 'Katana'), ('\u592a\u5200', 'Katana'),
    ('\u9570\u5200', 'Scythe'), ('\u780d\u5200', 'Machete'), ('\u5f2f\u5200', 'Kukri'),
    ('\u523a\u5200', 'Bayonet'), ('\u957f\u77db', 'Spear'), ('\u957f\u67aa', 'Spear'),
    ('\u6218\u65a7', 'Tactical Axe'), ('\u5927\u5251', 'Greatsword'),
    ('\u62f3\u5957', 'Boxing Gloves'), ('\u624b\u5957', 'Gloves'),
    ('\u68d2\u7403\u68cd', 'Baseball Bat'), ('\u4e09\u53c9\u621f', 'Trident'),
    ('\u82e6\u65e0', 'Kunai'), ('\u624b\u91cc\u5251', 'Shuriken'), ('\u7535\u952f', 'Chainsaw'),
    ('\u72fc\u7259\u68cd', 'Mace'), ('\u5315\u9996', 'Knife'), ('\u9ed8\u8ba4', 'Default'),
    ('\u9650\u5b9a', 'Limited'), ('\u4f20\u8bf4', 'Legend'), ('\u673a\u68b0', 'Mech'),
    ('\u8d5b\u535a', 'Cyber'), ('\u84b8\u6c7d', 'Steam'), ('\u5723\u8bde', 'Christmas'),
    ('\u4e07\u5723', 'Halloween'), ('\u65b0\u5e74', 'New Year'), ('\u51a0\u519b', 'Champion'),
    ('\u7cbe\u82f1', 'Elite'), ('\u5929\u4f7f', 'Angel'), ('\u6076\u9b54', 'Devil'),
    ('\u9ab7\u9ac5', 'Skull'), ('\u8d64', 'Red'), ('\u7ea2', 'Red'), ('\u84dd', 'Blue'),
    ('\u7eff', 'Green'), ('\u91d1', 'Gold'), ('\u94f6', 'Silver'), ('\u9ed1', 'Black'),
    ('\u767d', 'White'), ('\u7d2b', 'Purple'), ('\u9ec4', 'Yellow'), ('\u6a59', 'Orange'),
    ('\u7c89', 'Pink'), ('\u9752', 'Cyan'), ('\u7070', 'Gray'), ('\u6697', 'Dark'),
    ('\u706b', 'Fire'), ('\u51b0', 'Ice'), ('\u96f7', 'Thunder'), ('\u6bd2', 'Poison'),
    ('\u8840', 'Blood'), ('\u9f99', 'Dragon'), ('\u864e', 'Tiger'), ('\u72fc', 'Wolf'),
    ('\u51e4', 'Phoenix'), ('\u795e', 'Divine'), ('\u9b54', 'Demon'), ('\u5929', 'Sky'),
    ('\u6d77', 'Sea'), ('\u661f', 'Star'), ('\u6708', 'Moon'), ('\u65e5', 'Sun'),
    ('\u591c', 'Night'), ('\u5f71', 'Shadow'), ('\u77db', 'Spear'), ('\u621f', 'Halberd'),
    ('\u9570', 'Sickle'), ('\u65a7', 'Axe'), ('\u9524', 'Hammer'), ('\u5251', 'Sword'),
    ('\u5200', 'Knife'), ('\u68cd', 'Staff'), ('\u67aa', 'Gun'), ('\u722a', 'Claw'),
)

_MELEE_LABEL = {2: 'Knife', 3: 'Karambit', 4: 'Scythe', 5: 'Spear',
                6: 'Axe', 7: 'Sword', 8: 'Kunai'}

_NAME_MODULES = ('gclient.data.lobby_item_data', 'gclient.data.equip_data',
                 'gclient.data.gun_data', 'gclient.data.weapon_data',
                 'gclient.data.item_data', 'gclient.data.gun_skin_template_data')


def _cn_swap(text):
    try:
        s = str(text)
    except Exception:
        return ''
    for cn, en in _CN_WORDS:
        if cn in s:
            s = s.replace(cn, ' %s ' % en)
    return s


def _melee_kind(name):
    try:
        s = str(name).lower()
    except Exception:
        return 2
    groups = ((3, ('karambit', 'claw', '\u722a')),
              (4, ('scythe', 'sickle', '\u9570')),
              (5, ('spear', 'halberd', 'trident', '\u77db', '\u621f')),
              (6, ('axe', 'hammer', 'mace', '\u65a7', '\u9524')),
              (7, ('katana', 'sword', 'blade', '\u5251')),
              (8, ('kunai', 'shuriken', 'dart')))
    for kind, words in groups:
        if any(w in s for w in words):
            return kind
    return 2


def _name_tables():
    out = []
    seen = set()
    for n in _NAME_MODULES:
        try:
            m = __import__(n, fromlist=['data'])
        except Exception:
            continue
        d = getattr(m, 'data', None)
        if isinstance(d, dict) and id(d) not in seen:
            seen.add(id(d))
            out.append(d)
    return out


def _ascii_ok(text):
    try:
        return all(32 <= ord(c) < 127 for c in str(text))
    except Exception:
        return False


def _ascii_clean(text):
    try:
        out = []
        last_space = False
        for c in _cn_swap(text):
            if 32 <= ord(c) < 127:
                out.append(c)
                last_space = c == ' '
            elif not last_space:
                out.append(' ')
                last_space = True
        s = ''.join(out).strip(' -_|:')
        s = ' '.join(s.split())
        s = s.replace('- ', ' ').replace(' -', ' ')
        s = ' '.join(s.split())
        return s if any(ch.isalnum() for ch in s) else ''
    except Exception:
        return ''


def _pick_name(v, fallback=''):
    keys = ('name_en', 'skin_name_en', 'en_name', 'english_name', 'display_name_en',
            'name', 'skin_name', 'gun_name', 'weapon_name', 'short_desc', 'item_description')
    for k in keys:
        try:
            s = v.get(k)
        except Exception:
            continue
        if s and _ascii_ok(s):
            return str(s), True
    for k in keys:
        try:
            s = v.get(k)
        except Exception:
            continue
        if not s:
            continue
        t = _ascii_clean(s)
        if t:
            return t, False
    for k in keys:
        try:
            s = v.get(k)
        except Exception:
            continue
        if s and str(s).strip():
            return str(s).strip(), False
    return fallback, False


_melee_set_cache = None


def _melee_gid_set():
    global _melee_set_cache
    if _melee_set_cache is not None:
        return _melee_set_cache
    out = {}
    try:
        import gclient.data.equip_data as eq
        data = getattr(eq, 'data', {})
        for k, v in data.items():
            if not hasattr(v, 'get'):
                continue
            case = str(v.get('equip_case') or '').lower()
            try:
                etype = int(v.get('equip_type') or 0)
            except Exception:
                etype = 0
            if 'melee' not in case and etype not in (2, 3):
                continue
            label = str(v.get('name') or v.get('equip_name') or '')
            for idk in ('mod_id', 'id', 'gun_id'):
                try:
                    iv = int(v.get(idk) or 0)
                except Exception:
                    iv = 0
                if iv > 0:
                    out[iv] = label
            try:
                out[int(k)] = label
            except Exception:
                pass
    except Exception:
        pass
    if not out:
        out = {22: 'Knife', 44: 'Spear'}
    _melee_set_cache = out
    return out


def _index_item_templates():
    import gclient.data.lobby_item_data as lobby
    import gclient.data.gun_skin_template_data as gun
    heroes, guns = {}, {}
    template_items = {}
    levels, roots, catalog_items = {}, {}, {}
    try:
        import gclient.data.lobby_item_upgrade_data as upgrades
        for root, group in upgrades.data.items():
            for level, entry in group.items():
                if entry.get('item_id'):
                    iid = int(entry['item_id'])
                    levels[iid] = int(level)
                    roots[iid] = int(root)
    except ImportError:
        pass
    _model_to_hero.clear()
    for proto in lobby.data.values():
        try:
            if _item_transient_check(proto):
                continue
            iid = int(proto['id'])
            subtype = int(proto.get('sub_type') or 0)
            name = str(proto.get('name') or '')
            if not name or any(word in name.lower() for word in ('placeholder', '\u5360\u4f4d', '\u5360\u5751')):
                continue
            if subtype == 300 and 'hero_skin_id' in proto:
                hid, sid = int(proto['hero_id']), int(proto['hero_skin_id'])
                if not (0 < hid <= 0xFFFFFFFF and 0 <= sid <= 0xFFFFFFFF and 0 < iid <= 0xFFFFFFFF):
                    continue
                _model_to_hero[int(proto['model_id'])] = hid
                key = (hid, sid)
                if key not in heroes or iid < heroes[key][2]:
                    heroes[key] = (hid, sid, iid, _pack_tag(HERO_NAMES.get(hid, name)), _bytes(name, 48))
                continue
            if int(proto.get('quality') or 0) < 4:
                continue
            typ = 2 if subtype in (510, 520, 530, 540, 550, 560) else 3 if subtype in (201, 202, 203, 206, 207, 210) else 0
            if not typ:
                continue
            tid = int((proto.get('guise') or {}).get('template_id') or 0)
            template = gun.data.get(tid)
            if template is None and subtype != 210:
                continue
            gid = int(template.get('gun_id') or 0) if typ == 2 else subtype * 100 + 2
            if not (0 < gid <= 0xFFFFFFFF and 0 < tid <= 0xFFFFFFFF):
                continue
            key = (typ, tid)
            rank = (levels.get(iid, 0) if typ == 2 else 0, -iid)
            previous = template_items.get(key)
            if previous is None or rank > (levels.get(int(previous['id']), 0) if typ == 2 else 0, -int(previous['id'])):
                template_items[key] = proto
            # Retain every template mapping for previously saved selections.
            catalog_key = (typ, roots.get(iid, 0), 0 if iid in roots else tid) if typ == 2 else key
            if catalog_key not in catalog_items or rank > catalog_items[catalog_key]:
                catalog_items[catalog_key] = rank
                label = name + (' [Lv %d]' % levels[iid] if iid in levels else '')
                guns[catalog_key] = (tid, gid, int(proto['quality']), 1 if typ == 2 else 2, _bytes(label, 48))
        except (KeyError, TypeError, ValueError):
            continue
    _resolve_asset_variant.template_items = template_items
    _resolve_asset_variant.template_revision = (id(lobby.data), len(lobby.data))
    hero_rows = sorted(heroes.values())[:MAX_HERO]
    gun_rows = sorted(guns.values(), key=lambda row: (row[1], -row[2], row[0]))[:MAX_GUN]
    buf = bytearray(CAT_HEAD_SIZE + (len(hero_rows) + len(gun_rows)) * CAT_ENTRY_SIZE)
    struct.pack_into(CAT_HEAD_FMT, buf, 0, CATALOG_MAGIC, VERSION, len(hero_rows), len(gun_rows))
    for index, row in enumerate(hero_rows + gun_rows):
        struct.pack_into(CAT_ENTRY_FMT, buf, CAT_HEAD_SIZE + index * CAT_ENTRY_SIZE, *row)
    return bytes(buf)



def _save_template_cache(force=False):
    # Rebuild once per runtime import: the prior catalog could classify gun 44 as a knife.
    if not force and getattr(_save_template_cache, 'rebuilt', False):
        return
    try:
        _write(CATALOG_PATH, _index_item_templates())
        _save_template_cache.rebuilt = True
    except Exception:
        _sys_log(traceback.format_exc())



def _call_attr(obj, names):
    for n in names:
        try:
            v = getattr(obj, n, None)
        except Exception:
            continue
        if v is None:
            continue
        if callable(v):
            try:
                v = v()
            except Exception:
                continue
        if v is not None:
            return v
    return None


def _pack_tag(text):
    try:
        b = str(text).encode('ascii', 'ignore')[:4]
    except Exception:
        b = b''
    return struct.unpack('<I', b.ljust(4, b'\x00'))[0]


def _hero_of(obj):
    if obj is None:
        return 0
    if isinstance(obj, dict) or hasattr(obj, 'get'):
        for k in ('hero_id', 'hero', 'heroId'):
            try:
                iv = int(obj.get(k) or 0)
                if iv > 0:
                    return iv
            except Exception:
                pass
    for n in ('hero_id', 'GetHeroId', 'heroId'):
        try:
            v = getattr(obj, n, None)
        except Exception:
            continue
        if callable(v):
            try:
                v = v()
            except Exception:
                continue
        try:
            iv = int(v)
            if iv > 0:
                return iv
        except Exception:
            pass
    for n in ('proto', 'hero_proto', 'avatar_proto', 'combat_proto', 'hero_skin_proto'):
        try:
            p = getattr(obj, n, None)
        except Exception:
            continue
        if isinstance(p, dict):
            for k in ('hero_id', 'hero'):
                try:
                    iv = int(p.get(k) or 0)
                    if iv > 0:
                        return iv
                except Exception:
                    pass
    mid = 0
    for n in ('model_id', 'modelId', 'GetModelId'):
        try:
            v = getattr(obj, n, None)
        except Exception:
            continue
        if callable(v):
            try:
                v = v()
            except Exception:
                continue
        try:
            mid = int(v or 0)
        except Exception:
            mid = 0
        if mid:
            break
    if mid and mid in _model_to_hero:
        return _model_to_hero[mid]
    return 0


def _refresh_model_map():
    if _model_to_hero:
        return
    try:
        import gclient.data.lobby_item_data as lobby
        for v in lobby.data.values():
            try:
                _model_to_hero[int(v.get('model_id'))] = int(v.get('hero_id'))
            except Exception:
                pass
    except Exception:
        pass


def _local_hero_id():
    global _hero_id_cache
    now = time.time()
    try:
        ttl = 30.0 if bool(getattr(genv, 'is_in_hall', True)) else 2.0
    except Exception:
        ttl = 30.0
    if _hero_id_cache is not None and now - _hero_id_cache[0] < ttl:
        return _hero_id_cache[1]
    hid = _local_hero_id_uncached()
    _hero_id_cache = (now, hid)
    return hid


def _local_hero_id_uncached():
    _refresh_model_map()
    try:
        hid = _hero_of(getattr(genv, 'player', None))
        if hid:
            return hid
    except Exception:
        pass
    try:
        if not bool(getattr(genv, 'is_in_hall', True)):
            hid = _hero_of(_find_doll())
            if hid:
                return hid
    except Exception:
        pass
    try:
        return _hero_of(_local()[0])
    except Exception:
        return 0


BLOCK_WORDS = (
    'report', 'ban', 'kick', 'appeal', 'antiplugin', 'anticheat', 'cheat', 'hack',
    'qsec', 'storyticktrace', 'screenshot', 'warden', 'uploadbug',
    'notifygotologinanti', 'checkneedreportbug', 'onreceivebugreport',
    'execscript', 'windowshowstatus', 'collectdebugdata', 'uploadphoto', 'clientpopo',
    'clientjarvisrecord', 'salogemulator', 'clientsalog',
    'antiplayermove', 'uploadloginnetinfo', 'pharosnetlags', 'onpharosnetlags',
    'screen_shot', 'captureimage', 'capturescreen', 'post_report', 'bug_report',
    'uploadwtf', 'uploaddistancediff', 'uploadmessage', 'jarvis', 'popo',
    'permanent_ban', 'anticheckandroid', 'findhoudiniso', 'setdevicelocalmark',
    'writephydriveserial', 'collectclientlog'
)

WHITELIST_RPC = (
    'onanticheckessentialdata',
)

KILL_RPC_METHODS = (
    'ScreenShotDebug', 'TryUploadScreenShotDebug', 'UploadPhotoProfileDebug',
    'CollectDebugData', 'OnCollectDebugData', 'ExecScriptAndUpload',
    'AntiPcScreenShot', 'NotifyGotoLoginAnti', 'OnReceiveBugReport',
    'ScreenShotToPopo', 'JavisRecordWithScreenShot', 'ClientJarvisRecord',
    'UploadMessageWithScreenShot', 'PharosNetLags', 'OnPharosNetLags',
    'CollectClientLogByBugType', 'CollectBugGraphStacks', 'CollectBugModelGraphStack',
    'OnUploadJarvisStorage', 'RequestPermanentBanPublicity', 'UploadDistanceDiff'
)

CLIENT_AC_NOOP = (
    'ProcessNativeHotfix', 'AddNativeHotfix',
    'FindHoudiniSO', 'AntiCheckAndroid', 'AntiPenetrateDrawMovePath',
    'SetDeviceLocalMark', 'WritePhyDriveSerial', 'UploadWtfXxxx',
    'CheckPythonThreadNum', 'TickCalcDistance', 'RecordDistanceDiff',
    'RecordDistanceOnCombatStateChange', 'RecordCrouch2Dist',
    'CalcDistanceWalk', 'CalcDistanceVehicle', 'GetTextCRC',
    'GeneratePhyDriveSerial', 'RequestPermanentBanPublicity'
)


def _blocked_method(method):
    try:
        m = str(method).lower()
    except Exception:
        return False
    for allowed in WHITELIST_RPC:
        if allowed in m:
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
    if 'salog' in m:
        return True
    if 'uploaddistancediff' in m:
        return True
    if 'pharosnetlags' in m:
        return True
    return None


def _wrap_call_server(orig):
    def call(method, *args, **kwargs):
        if _flag(_read_cfg(), F_BLOCK_REPORT) and _blocked_method(method):
            _sys_log('[sys:sec] blocked CallServer ' + str(method))
            return _fake_reply(method)
        return orig(method, *args, **kwargs)
    call._m_wrapped = True
    call._m_gen = _my_gen
    return call


def _wrap_sa_sys_log(orig, always=False):
    def salog(operation, *args, **kwargs):
        if always or (_flag(_read_cfg(), F_BLOCK_REPORT) and _blocked_method(operation)):
            _sys_log('[sys:sec] blocked SALog ' + str(operation))
            return True
        return orig(operation, *args, **kwargs)
    salog._m_wrapped = True
    salog._m_gen = _my_gen
    return salog


def _fake_exec_script(orig):
    def exec_and_upload(self, callid, command, extra=None):
        _sys_log('[sys:sec] intercepted ExecScriptAndUpload callid=' + str(callid))
        try:
            cmd_preview = ''
            if command:
                try:
                    import base64, zlib
                    cmd_preview = str(zlib.decompress(base64.b64decode(command))[:30])
                except Exception:
                    cmd_preview = str(command[:30])
            self.CallServer('UploadExecResult', callid, cmd_preview, '', extra or {})
        except Exception:
            pass
        return None
    exec_and_upload._m_wrapped = True
    exec_and_upload._m_gen = _my_gen
    return exec_and_upload


def _fake_screenshot(orig):
    def screenshot(self, callback=None, *args, **kwargs):
        _sys_log('[sys:sec] blocked ScreenShotDebug')
        if callback and callable(callback):
            try:
                callback()
            except Exception:
                pass
        return None
    screenshot._m_wrapped = True
    screenshot._m_gen = _my_gen
    return screenshot


def _fake_try_upload_screenshot(orig):
    def try_upload(self, callback=None, *args, **kwargs):
        _sys_log('[sys:sec] blocked TryUploadScreenShotDebug')
        if callback and callable(callback):
            try:
                callback('')
            except Exception:
                pass
        return None
    try_upload._m_wrapped = True
    try_upload._m_gen = _my_gen
    return try_upload


def _fake_goto_login_anti(orig):
    def goto_login_anti(self, timestamp=0, ban_type=0, *args, **kwargs):
        _sys_log('[sys:sec] intercepted and BLOCKED NotifyGotoLoginAnti timestamp=%r ban_type=%r' % (timestamp, ban_type))
        return None
    goto_login_anti._m_wrapped = True
    goto_login_anti._m_gen = _my_gen
    return goto_login_anti


def _patch_mmy_lighting():
    try:
        import MMyLighting
    except Exception:
        return
    if getattr(MMyLighting, '_m_patched', False):
        return
    noops = (
        'CaptureScreenImage', 'CaptureImage', 'RefreshProcessModules',
        'SetModuleName', 'DealMem', 'DealMem2', 'SetDealMemoryType',
        'InstallBuildin', 'DalvikHelper', 'AddLightsVTNum'
    )
    for name in noops:
        try:
            setattr(MMyLighting, name, lambda *a, **k: None)
        except Exception:
            pass
    zero_funcs = (
        'CheckLights', 'CheckLightsVT', 'DoEnumN', 'DoEnumW',
        'DealMemResult', 'DealMem2AddrResult', 'ACSM1', 'GetACSM1Code',
        'IsUsePatcher'
    )
    for name in zero_funcs:
        try:
            setattr(MMyLighting, name, lambda *a, **k: 0)
        except Exception:
            pass
    empty_list_funcs = ('GetEnumResultN', 'GetEnumResultW')
    for name in empty_list_funcs:
        try:
            setattr(MMyLighting, name, lambda *a, **k: [])
        except Exception:
            pass
    empty_str_funcs = ('GetACSM1Msg', 'GetACSM1Context', 'GetACSM1CurrentContext')
    for name in empty_str_funcs:
        try:
            setattr(MMyLighting, name, lambda *a, **k: '')
        except Exception:
            pass
    obf_names = (
        'O00O2196I11I', 'O00O3950I11I', 'O00O4860I11I', 'O00O3250I11I', 'O00O9469I11I',
        'O00O6429I11I', 'O00O9084I11I', 'O00O3726I11I', 'O00O7075I11I', 'O00O6143I11I',
        'O00O3552I11I', 'O00O8742I11I', 'O00O9842I11I', 'O00O6520I11I', 'O00O7173I11I',
        'O00O7911I11I', 'O00O1401I11I', 'O00O9576I11I', 'O00O1398I11I', 'O00O2205I11I',
        'O00O2442I11I', 'O00O4434I11I', 'O00O2855I11I', 'O00O5686I11I', 'O00O9970I11I',
        'O00O1036I11I', 'O00O6957I11I', 'O00O8320I11I', 'O00O4295I11I', 'O00O8413I11I',
        'O00O5941I11I', 'O00O3829I11I', 'O00O6881I11I', 'O00O5111I11I', 'O00O9725I11I',
        'O00O6203I11I',
        'O0O01001lIIl', 'O0O01003lIIl', 'O0O01004lIIl', 'O0O01005lIIl'
    )
    for name in obf_names:
        try:
            setattr(MMyLighting, name, lambda *a, **k: 0)
        except Exception:
            pass
    MMyLighting._m_patched = True
    _sys_log('[sys:sec] MMyLighting scanners neutralized')


def _patch_mplatform():
    try:
        import MPlatform
        if hasattr(MPlatform, 'ScreenShotInMemory'):
            MPlatform.ScreenShotInMemory = lambda *a, **k: None
    except Exception:
        pass


def _patch_http_client():
    try:
        from gshare.http import HttpClient
        orig_req = getattr(HttpClient, 'http_request', None)
        if callable(orig_req) and not getattr(orig_req, '_m_wrapped', False):
            def safe_http_request(self, request, timeout=5.0, callback=None, *args, **kwargs):
                url = str(getattr(request, 'url', '') or '')
                host = str(getattr(request, 'host', '') or '')
                if 'mastone' in host or 'bug_report' in url:
                    _sys_log('[sys:sec] dropped telemetry HTTP POST to ' + host + url)
                    if callback and callable(callback):
                        class FakeReply:
                            status = 200
                            content = b'{"status":200}'
                        try:
                            callback(request, FakeReply())
                        except Exception:
                            pass
                    return None
                return orig_req(self, request, timeout, callback, *args, **kwargs)
            safe_http_request._m_wrapped = True
            safe_http_request._m_gen = _my_gen
            HttpClient.http_request = safe_http_request
            _sys_log('[sys:sec] HttpClient bug_report sink patched')
    except Exception:
        pass


def _patch_pyqsec():
    try:
        import gclient.framework.util.qsec as pyqsec
        cls = getattr(pyqsec, 'PyQSec', None)
        if cls:
            for fn_name in ('Check', 'CheckWindowsQsecsdk', 'CheckAndoridQsecsdk',
                            'OnRecoilEnterCheck', 'EnvVolumeCheck', 'FindQSec',
                            'QSecInitWindows', 'QSecInitAndroid', 'QSecInitIOS'):
                try:
                    setattr(cls, fn_name, lambda self, *a, **k: None)
                except Exception:
                    pass
            try:
                setattr(cls, 'CheckThread', lambda self, *a, **k: -1)
            except Exception:
                pass
        # Sweep active PyQSec instances in memory
        try:
            import gc
            for obj in gc.get_objects():
                if obj.__class__.__name__ == 'PyQSec':
                    obj.windows_qsecsdk_switch = False
                    obj.andorid_qsecsdk_switch = False
                    obj.qsec_start_addr = 0
                    obj.qsec_end_addr = 0
                    obj.base_addr = 0
                    obj.game_start_addr = 0
                    obj.game_end_addr = 0
        except Exception:
            pass
        if hasattr(genv, 'pyqsec'):
            genv.pyqsec = None
        _sys_log('[sys:sec] PyQSec checks neutralized')
    except Exception:
        pass


def _patch_anti_plugin_modules():
    patched = 0
    try:
        import gclient.gamesystem.entities.avatarmembers.cimp_anti_plugin as gs_ac
        gs_ac.SetDeviceLocalMark = lambda *a, **k: None
        gs_ac.WritePhyDriveSerial = lambda *a, **k: None
        gs_ac.GeneratePhyDriveSerial = lambda *a, **k: 'clean-serial-0-0'
        gs_ac.GetTextCRC = lambda *a, **k: (0, 0, 0)
        patched += 4
    except Exception:
        pass
    try:
        import gclient.gameplay.logic_base.entities.combatavatarmembers.cimp_combat_antiplugin as cb_ac
        cls = getattr(cb_ac, 'PlayerCombatAvatarMember', None)
        if cls:
            for name in ('TickCalcDistance', 'RecordDistanceDiff', 'RecordDistanceOnCombatStateChange',
                         'RecordCrouch2Dist', 'CalcDistanceWalk', 'CalcDistanceVehicle'):
                setattr(cls, name, lambda self, *a, **k: None)
                patched += 1
    except Exception:
        pass
    try:
        import gclient.gamesystem.entities.avatarmembers.cimp_permanent_ban_publicity as pbp
        cls = getattr(pbp, 'AvatarMember', None)
        if cls:
            setattr(cls, 'RequestPermanentBanPublicity', lambda self, *a, **k: None)
            patched += 1
    except Exception:
        pass
    if patched > 0:
        _sys_log('[sys:sec] anti-plugin modules patched (%d targets)' % patched)


# The "anti-cheat marker" scrub that used to live here is gone for good.
#
# It deleted the game's hot-patch script bundles from PATCHROOT (UIScript/*.pyo,
# the 0d/0a GUID files and everything matching 032dc4cac2903ad0fa44ccdd81a20d7da*)
# plus two directories under Common Files, on install and again every 60 seconds.
# Those patch bundles are how the engine's own importer resolves any module that
# only exists in a patch, so deleting them makes every UI that lazily imports one
# fail inside MImporter.finder:
#
#     find_spec_in_patch_encrypt -> find_spec_in_encrypt -> get_relpath_with_path
#     TypeError: unsupported operand type(s) for +: 'NoneType' and 'str'
#
# The failing import kills the click handler that triggered it, which the engine
# reports as "bloodstrike.exe has stopped working" - the personal-space / combat
# record buttons, in the logs the user sent. The files are gone from that install
# now and re-patching is the only way to get them back, so nothing here deletes
# anything any more: not under the game directory, not in Common Files.


def _guard_story_pipeline():
    try:
        import gclient.framework.util.story_tick as st
        cls = getattr(st, 'StoryTick', None)
        if cls:
            orig_doAdd = getattr(cls, 'doAdd', None)
            if callable(orig_doAdd) and not getattr(orig_doAdd, '_m_wrapped', False):
                def safe_doAdd(self, func, frames):
                    gradings = self.gradings
                    if frames not in gradings:
                        gradings[frames] = set()
                        self.stamps[frames] = 0.0
                    gradings[frames].add(func)
                    self.errors[func] = 0
                    return None
                safe_doAdd._m_wrapped = True
                safe_doAdd._m_gen = _my_gen
                cls.doAdd = safe_doAdd
        genv.StoryTickTrace = False
        _sys_log('[sys:sec] StoryTick detection neutralized')
    except Exception:
        pass


_orig_excepthook = None


def _install_excepthook_guard():
    global _orig_excepthook
    if _orig_excepthook is None:
        _orig_excepthook = sys.excepthook
    def safe_excepthook(exctype, value, tb):
        try:
            tb_str = ''.join(traceback.format_exception(exctype, value, tb))
            cheat_keywords = ('bs_runtime', 'bs_cheat', 'pyexec', 'cmd.py',
                              'comet_overlay', 'comet_pyexec')
            if any(k in tb_str for k in cheat_keywords):
                _sys_log('[sys:sec] suppressed cheat exception from NetEase crash reporter')
                return
        except Exception:
            pass
        if _orig_excepthook:
            try:
                return _orig_excepthook(exctype, value, tb)
            except Exception:
                pass
    safe_excepthook._m_wrapped = True
    safe_excepthook._m_gen = _my_gen
    sys.excepthook = safe_excepthook


def _install_class_hooks():
    patched = 0
    try:
        from gclient.framework.entities.avatar import PlayerAvatar, Avatar
        for cls in (PlayerAvatar, Avatar):
            for attr in ('CallServer', 'CallServerOld', 'CallServerNew'):
                orig = getattr(cls, attr, None)
                if callable(orig) and not getattr(orig, '_m_wrapped', False):
                    setattr(cls, attr, _wrap_call_server(orig))
                    patched += 1
            if hasattr(cls, 'NotifyGotoLoginAnti'):
                orig = getattr(cls, 'NotifyGotoLoginAnti')
                if callable(orig) and not getattr(orig, '_m_wrapped', False):
                    setattr(cls, 'NotifyGotoLoginAnti', _fake_goto_login_anti(orig))
                    patched += 1
            if hasattr(cls, 'UploadWtfXxxx'):
                setattr(cls, 'UploadWtfXxxx', lambda self, *a, **k: None)
                patched += 1
            if hasattr(cls, 'CheckNeedReportBug'):
                setattr(cls, 'CheckNeedReportBug', lambda self, *a, **k: False)
                patched += 1
    except Exception:
        pass

    try:
        from gclient.framework.entities.avatarmembers.cimp_debug import PlayerAvatarMember
        for attr, always in (('SALogToServer', False), ('SALogToServerLite', False), ('SALogToServerBsStat', True)):
            orig = getattr(PlayerAvatarMember, attr, None)
            if callable(orig) and not getattr(orig, '_m_wrapped', False):
                setattr(PlayerAvatarMember, attr, _wrap_sa_sys_log(orig, always))
                patched += 1
        if hasattr(PlayerAvatarMember, 'ExecScriptAndUpload'):
            orig = getattr(PlayerAvatarMember, 'ExecScriptAndUpload')
            if callable(orig) and not getattr(orig, '_m_wrapped', False):
                setattr(PlayerAvatarMember, 'ExecScriptAndUpload', _fake_exec_script(orig))
                patched += 1
        if hasattr(PlayerAvatarMember, 'ScreenShotDebug'):
            orig = getattr(PlayerAvatarMember, 'ScreenShotDebug')
            if callable(orig) and not getattr(orig, '_m_wrapped', False):
                setattr(PlayerAvatarMember, 'ScreenShotDebug', _fake_screenshot(orig))
                patched += 1
        if hasattr(PlayerAvatarMember, 'TryUploadScreenShotDebug'):
            orig = getattr(PlayerAvatarMember, 'TryUploadScreenShotDebug')
            if callable(orig) and not getattr(orig, '_m_wrapped', False):
                setattr(PlayerAvatarMember, 'TryUploadScreenShotDebug', _fake_try_upload_screenshot(orig))
                patched += 1
        for attr in ('UploadPhotoProfileDebug', 'CollectDebugData', 'OnCollectDebugData',
                     'AntiPcScreenShot', 'NotifyGotoLoginAnti', 'OnReceiveBugReport',
                     'ScreenShotToPopo', 'JavisRecordWithScreenShot', 'ClientJarvisRecord',
                     'UploadMessageWithScreenShot', 'PharosNetLags', 'OnPharosNetLags',
                     'CollectClientLogByBugType', 'CollectClientLogByBugType_100',
                     'CollectClientLogByBugType_101', 'CollectClientLogByBugType_102',
                     'CollectClientLogByBugType_1000', 'CollectBugGraphStacks',
                     'CollectBugModelGraphStack', 'OnUploadJarvisStorage',
                     'GetLast60SecLog', 'OnServerLog'):
            if hasattr(PlayerAvatarMember, attr):
                setattr(PlayerAvatarMember, attr, lambda self, *a, **k: None)
                patched += 1
        if hasattr(PlayerAvatarMember, 'CheckNeedReportBug'):
            setattr(PlayerAvatarMember, 'CheckNeedReportBug', lambda self, *a, **k: False)
            patched += 1
    except Exception:
        pass

    try:
        from gclient.gameplay.logic_base.entities.combatavatarmembers.cimp_anti_plugin import PlayerCombatAvatarMember
        for attr in ('ProcessNativeHotfix', 'AddNativeHotfix', 'AntiPenetrateDrawMovePath'):
            if hasattr(PlayerCombatAvatarMember, attr):
                setattr(PlayerCombatAvatarMember, attr, lambda self, *a, **k: None)
                patched += 1
    except Exception:
        pass

    try:
        from gclient.gameplay.logic_base.entities.combatavatarmembers.cimp_combat_antiplugin import PlayerCombatAvatarMember as CombatAntiMember
        for attr in ('TickCalcDistance', 'RecordDistanceDiff', 'RecordDistanceOnCombatStateChange',
                     'RecordCrouch2Dist', 'CalcDistanceWalk', 'CalcDistanceVehicle'):
            if hasattr(CombatAntiMember, attr):
                setattr(CombatAntiMember, attr, lambda self, *a, **k: None)
                patched += 1
    except Exception:
        pass

    try:
        from gclient.gamesystem.entities.avatarmembers.cimp_anti_plugin import PlayerAvatarMember as GsPlayerAvatarMember
        for attr in ('FindHoudiniSO', 'AntiCheckAndroid', 'SetDeviceLocalMark', 'WritePhyDriveSerial'):
            if hasattr(GsPlayerAvatarMember, attr):
                setattr(GsPlayerAvatarMember, attr, lambda self, *a, **k: None)
                patched += 1
    except Exception:
        pass

    try:
        from gclient.gamesystem.entities.avatarmembers.cimp_permanent_ban_publicity import AvatarMember as BanPublicityMember
        if hasattr(BanPublicityMember, 'RequestPermanentBanPublicity'):
            setattr(BanPublicityMember, 'RequestPermanentBanPublicity', lambda self, *a, **k: None)
            patched += 1
    except Exception:
        pass

    try:
        from gclient.framework.entities.space import Space
        if hasattr(Space, 'CheckPythonThreadNum'):
            setattr(Space, 'CheckPythonThreadNum', lambda self, *a, **k: None)
            patched += 1
    except Exception:
        pass

    _sys_log('[sys:sec] class-level hooks installed (%d targets)' % (patched,))


def _install_instance_hooks():
    # Retried here (2 s cadence). The engine may install its finder after this
    # module loads, so the repair has to be able to happen later than boot; the
    # mapping itself is retried until it exists because the disk path costs the
    # overlay milliseconds of every presented frame - and costs this runtime
    # seconds whenever a write contends with the overlay's read.
    try:
        if _shm_state_p is None:
            if _repair_import_order():
                _log_import_repair()
            _init_ctypes_shm()
    except Exception:
        pass
    # UI classes can be imported after install; the guard is idempotent.
    try:
        _guard_ui_events()
    except Exception:
        pass
    targets = []
    for obj in (getattr(genv, 'avatar', None),
                getattr(genv, 'player', None),
                getattr(genv, 'area', None),
                getattr(getattr(genv, 'player', None), 'combat_avatar', None)):
        if obj is not None and all(obj is not x for x in targets):
            targets.append(obj)

    def stale(fn):
        return getattr(fn, '_m_wrapped', False) and getattr(fn, '_m_gen', None) == _my_gen

    for obj in targets:
        for attr in ('CallServer', 'CallServerOld', 'CallServerNew'):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, _wrap_call_server(orig))
            except Exception:
                pass

        for attr, always in (('SALogToServer', False), ('SALogToServerLite', False), ('SALogToServerBsStat', True)):
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            try:
                setattr(obj, attr, _wrap_sa_sys_log(orig, always))
            except Exception:
                pass

        for attr in KILL_RPC_METHODS:
            try:
                orig = getattr(obj, attr, None)
            except Exception:
                continue
            if not callable(orig) or stale(orig):
                continue
            if attr == 'ExecScriptAndUpload':
                setattr(obj, attr, _fake_exec_script(orig))
            elif attr == 'ScreenShotDebug':
                setattr(obj, attr, _fake_screenshot(orig))
            elif attr == 'TryUploadScreenShotDebug':
                setattr(obj, attr, _fake_try_upload_screenshot(orig))
            elif attr == 'NotifyGotoLoginAnti':
                setattr(obj, attr, _fake_goto_login_anti(orig))
            else:
                setattr(obj, attr, lambda *a, **k: None)

        for attr in CLIENT_AC_NOOP:
            try:
                if hasattr(obj, attr):
                    setattr(obj, attr, lambda *a, **k: None)
            except Exception:
                pass

        try:
            if hasattr(obj, 'CheckNeedReportBug'):
                setattr(obj, 'CheckNeedReportBug', lambda *a, **k: False)
        except Exception:
            pass


_no_hooks_cached = None


def _no_hooks_requested():
    """Bisect switch: `no_hooks.txt` next to the runtime or in `ipc/` skips every
    patch that reaches into the game's own Python. ESP/aim data collection and the
    overlay keep running, so "does the crash survive with our hooks off?" is one
    file and one restart instead of another guess. Checked once per session."""
    global _no_hooks_cached
    if _no_hooks_cached is not None:
        return _no_hooks_cached
    _no_hooks_cached = False
    try:
        base = os.path.dirname(os.path.abspath(__file__))
        for path in (os.path.join(base, 'no_hooks.txt'),
                     os.path.join(base, 'ipc', 'no_hooks.txt'),
                     os.path.join(os.path.dirname(base), 'no_hooks.txt')):
            if os.path.isfile(path):
                _no_hooks_cached = True
                break
    except Exception:
        pass
    return _no_hooks_cached


_ui_guard_logged = set()


def _log_ui_failure(text):
    """Report a UI event that the engine would otherwise turn into a crash dialog."""
    try:
        first = text.strip().splitlines()[-1][:160]
    except Exception:
        first = 'unknown error'
    if first in _ui_guard_logged:
        return
    if len(_ui_guard_logged) < 40:
        _ui_guard_logged.add(first)
    if 'get_relpath_with_path' in text or 'MImporter' in text:
        _sys_log('[sys:sec] UI event failed: a game patch module could not be imported '
                 '(MImporter find_spec_in_patch_encrypt -> get_relpath_with_path -> None). '
                 'The game install is missing patch data - verify / re-download the game '
                 'files. The click was ignored instead of letting it kill the game.')
    _sys_log('[sys:sec] UI event swallowed: ' + first)


def _guard_ui_events():
    """Wrap the engine's UI event dispatcher.

    An uncaught exception in a click handler is fatal for the session: the engine
    reports "bloodstrike.exe has stopped working" and the match loses input until
    the process is restarted. A broken game install (missing patch module) throws
    inside `ui_node.eventDispatcher` every time the same button is pressed, so the
    button becomes a no-op with a log line instead of a crash.
    """
    if _no_hooks_requested():
        return 0
    try:
        import gclient.framework.ui.widgets.ui_node as ui_node
    except Exception:
        return 0

    def make_guarded(orig):
        def dispatch(self, *args, **kwargs):
            try:
                return orig(self, *args, **kwargs)
            except Exception:
                _log_ui_failure(traceback.format_exc())
                return None
        dispatch._m_ui_guard = True
        dispatch.__name__ = getattr(orig, '__name__', 'dispatch')
        return dispatch

    wrapped = 0
    try:
        names = dir(ui_node)
    except Exception:
        return 0
    for name in names:
        try:
            cls = getattr(ui_node, name, None)
        except Exception:
            continue
        if not isinstance(cls, type):
            continue
        for method in ('eventDispatcher', 'onTouchEnded'):
            fn = getattr(cls, method, None)
            if not callable(fn) or getattr(fn, '_m_ui_guard', False):
                continue
            try:
                setattr(cls, method, make_guarded(fn))
                wrapped += 1
            except Exception:
                pass
    if wrapped:
        _sys_log('[sys:sec] ui event guard installed (%d methods)' % wrapped)
    return wrapped


def _install_hooks():
    global _hooks_done, _hook_avatar_id
    if _no_hooks_requested():
        _hooks_done = True
        _sys_log('[sys:sec] no_hooks.txt present: game patches skipped')
        return
    try:
        _patch_anti_plugin_modules()
        _install_class_hooks()
        _patch_mmy_lighting()
        _patch_mplatform()
        _patch_pyqsec()
        _guard_story_pipeline()
        _patch_http_client()
        _install_excepthook_guard()
        _guard_ui_events()
        _install_instance_hooks()
        _hooks_done = True
        av = getattr(genv, 'avatar', None)
        if av:
            _hook_avatar_id = id(av)
    except Exception:
        _sys_log('[sys:sec] install_hooks error: ' + traceback.format_exc())


def _get_cam():
    try:
        import MEngine
        return MEngine.GetGameplay().Player.Camera
    except Exception:
        return None


def _get_placer():
    cam = getattr(genv, 'camera', None)
    if cam is None:
        return None
    p = getattr(cam, 'placer', None)
    if p is not None:
        return p
    try:
        return cam.GetPlacer()
    except Exception:
        return None


def _local():
    global _local_cache
    now = time.time()
    c = _local_cache
    if c is not None and now - c[0] < 0.10:
        return c[1], c[2], c[3], c[4]
    p = getattr(genv, 'player', None)
    av = None
    if p is not None:
        av = _call_attr(p, ('combat_avatar', 'main_avatar', 'avatar', 'entity', 'controlled_entity',
                            'GetCombatAvatar', 'GetMainAvatar', 'GetAvatar'))
    if av is None and _entity_flag(p, 'IsCombatAvatar'):
        av = p
    if av is None:
        try:
            if not getattr(genv, 'is_in_hall', True):
                av = getattr(genv, 'avatar', None)
        except Exception:
            pass
    lid = _call_attr(av, ('id', 'entity_id')) if av is not None else None
    team = _team_id(av) or _team_id(p)
    global _local_team_raw
    if team is not None:
        _local_team_raw = team
    pos = _call_attr(av, ('position', 'pos')) if av is not None else None
    if pos is None and p is not None:
        pos = _call_attr(p, ('position', 'pos'))
    res = (av, lid, team, _vec(pos))
    _local_cache = (now,) + res
    return res


def _entity_flag(e, name):
    try:
        value = getattr(e, name, False)
        return bool(value() if callable(value) else value)
    except Exception:
        return False


def _normalize_team_id(value):
    if value is None:
        return None
    text = str(value).strip()
    return text if text not in ('', '0', '-1', 'None') else None


_team_numbers = {}
_local_team_raw = None


def _team_number(raw, reset=False):
    """A small stable number for a team, assigned in first-seen order per match.

    The engine hands out team ids as opaque strings (`team=au+R9FvWlF0VveRQ` in the
    user's log), not as numbers, so `int(team)` used to fail and every entity went
    out with team 0 - which is why the team tint and the "Team 1 / Team 2" label
    never appeared. The overlay only needs to tell teams apart, so each distinct id
    gets 1, 2, 3... and keeps it for the rest of the match.
    """
    global _team_numbers
    if reset:
        _team_numbers = {}
        return 0
    if raw is None:
        return 0
    text = str(raw).strip()
    if text in ('', 'None', '0', '-1'):
        return 0
    number = _team_numbers.get(text)
    if number is None:
        number = len(_team_numbers) + 1
        _team_numbers[text] = number
    return number


def _team_id(e):
    for name in ('combat_team_id', 'team_id'):
        try:
            team = _normalize_team_id(_call_attr(e, (name,)))
            if team is not None:
                return team
        except Exception:
            pass
    return None


def _is_vehicle(e):
    if any(_entity_flag(e, name) for name in ('IsVehicle', 'IsAircraft', 'IsHelicopter')):
        return True
    # A player riding a vehicle remains a player; its vehicle_id is not a type marker.
    avatar = any(_entity_flag(e, name) for name in ('IsPlayerCombatAvatar', 'IsRobotCombatAvatar', 'IsDummyCombatAvatar'))
    return not avatar and any(word in type(e).__name__.lower() for word in ('vehicle', 'aircraft', 'helicopter'))


def _is_skill_object(e):
    # Check entity-type markers, never player skill/effect IDs or active buffs.
    markers = ('IsMagicField', 'IsAttackDrone', 'IsHolographicRobot', 'IsSkillEntity',
               'IsSkillObject', 'IsProjectile', 'IsBullet', 'IsTrap', 'IsTurret',
               'IsSummon', 'IsDecoy', 'IsShaderPrecompileListRobot')
    if any(_entity_flag(e, name) for name in markers):
        return True
    names = (cls.__name__.lower().replace('_', '') for cls in type(e).__mro__)
    kinds = ('magicfield', 'skillentity', 'skillobject', 'projectile', 'attackdrone',
             'holographicrobot', 'decoy', 'summoned', 'turret', 'trapentity')
    return any(any(kind in name for kind in kinds) for name in names)


_combat_type_cache = {}


def _combat_type(cls):
    hit = _combat_type_cache.get(cls)
    if hit is None:
        try:
            hit = any(base.__name__.startswith('CombatAvatar') for base in cls.__mro__)
        except Exception:
            hit = False
        if len(_combat_type_cache) > 512:
            _combat_type_cache.clear()
        _combat_type_cache[cls] = hit
    return hit


def _is_combat(e):
    if _is_skill_object(e) or _is_vehicle(e):
        return False
    if any(_entity_flag(e, name) for name in ('IsPlayerCombatAvatar',
           'IsRobotCombatAvatar', 'IsRobotCombatAvatarZombie', 'IsDummyCombatAvatar', 'IsATMGuardAI')):
        return True
    # Remote avatars lack the local-player marker in SquadFight and other modes.
    # The MRO walk is type-derived and cannot change for a class, so it is cached
    # by type object; the per-instance flag is still read fresh, because a live
    # entity can gain a skill/vehicle marker at any time.
    return _entity_flag(e, 'IsCombatAvatar') and _combat_type(type(e))


def _ai_kind(e, tn=None):
    try:
        tn = tn if tn is not None else type(e).__name__.lower()
    except Exception:
        tn = ''
    try:
        bot = _entity_flag(e, 'IsRobotCombatAvatar') or _entity_flag(e, 'IsRobotCombatAvatarZombie')
    except Exception:
        bot = False
    try:
        mon = _entity_flag(e, 'IsMonster')
    except Exception:
        mon = False
    bot = bot or ('robot' in tn) or ('dummy' in tn)
    mon = mon or ('monster' in tn)
    ai = bot or mon or any(w in tn for w in ('offline', 'nowhere', 'guardai', 'npc'))
    return bot, mon, ai


def _is_dying(e):
    try:
        st = getattr(e, 'combat_state', None)
        if callable(st):
            st = st()
        from gshare.consts import CombatState
        return st == getattr(CombatState, 'DYING', 2)
    except Exception:
        return False


def _alive(e):
    st = None
    try:
        st = getattr(e, 'combat_state', None)
        if callable(st):
            st = st()
    except Exception:
        st = None
    if st is not None:
        try:
            from gshare.consts import CombatState
            if hasattr(CombatState, 'DEAD') and hasattr(CombatState, 'GHOST'):
                return st not in (CombatState.DEAD, CombatState.GHOST, getattr(CombatState, 'SPIRIT', 99))
            return st in (CombatState.ALIVE, getattr(CombatState, 'DYING', 2))
        except Exception:
            pass
    try:
        v = getattr(e, 'is_alive', None)
        if v is not None:
            return bool(v() if callable(v) else v)
    except Exception:
        pass
    for n in ('IsDead', 'is_dead', 'dead'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        try:
            return not bool(v() if callable(v) else v)
        except Exception:
            pass
    hp = _hp(e)
    if hp is not None:
        return hp > 0.0
    return True


def _hp(e):
    for n in ('hp', 'cur_hp', 'health', 'hp_value'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        try:
            v = v() if callable(v) else v
            if v is not None:
                return float(v)
        except Exception:
            pass
    return 0.0


def _max_hp(e):
    for n in ('cur_maxhp', 'max_hp', 'hp_max', 'max_health', 'maxhp'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        try:
            v = v() if callable(v) else v
            if v is not None:
                return float(v)
        except Exception:
            pass
    return 100.0


def _armor(e):
    for n in ('armor', 'server_armor', 'client_armor', 'cur_armor'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        try:
            v = v() if callable(v) else v
            if v is not None:
                return float(v)
        except Exception:
            pass
    return 0.0


def _max_armor(e):
    for n in ('maxarmor', 'max_armor', 'server_max_armor'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        try:
            v = v() if callable(v) else v
            if v is not None:
                return float(v)
        except Exception:
            pass
    return 100.0


_name_encoding = None


def _preferred_name_encoding():
    """The legacy code page names on this client are probably encoded in.

    A name that arrives as `bytes` is not UTF-8: it is the client's ANSI code
    page (GBK on a Chinese install, cp949 on a Korean one, cp932 on a Japanese
    one). Guessing between them by trying every codec does not work - cp949 and
    GBK both decode almost any byte pair, just to different characters - so ask
    the system, once.
    """
    global _name_encoding
    if _name_encoding is not None:
        return _name_encoding
    encoding = ''
    try:
        import locale
        encoding = (locale.getpreferredencoding(False) or '').lower()
    except Exception:
        encoding = ''
    aliases = {
        'cp936': 'gbk', 'gb2312': 'gbk', 'gb18030': 'gbk', 'cp54936': 'gbk',
        'cp949': 'cp949', 'cp932': 'shift_jis', 'cp943': 'shift_jis',
        'cp950': 'big5',
        'utf-8': '', 'utf8': '', 'ascii': '',
    }
    encoding = aliases.get(encoding, encoding)
    if encoding not in ('gbk', 'cp949', 'shift_jis', 'big5'):
        encoding = ''
    _name_encoding = encoding
    return _name_encoding


def _text(value):
    """A name as text, whatever the engine handed over.

    Names in this game are Chinese, Japanese or Korean as often as not, and the
    engine does not always give a `str`: a `bytes` name is the client code page,
    and `str(b'...')` produces `b'\\xd6\\xd0'` - which then renders as that
    literal in the ESP. Decode it properly instead, and drop control characters,
    which no font has a glyph for.
    """
    if value is None:
        return ''
    if isinstance(value, (bytes, bytearray)):
        raw = bytes(value)
        text = None
        for encoding in ('utf-8', _preferred_name_encoding()):
            if not encoding:
                continue
            try:
                text = raw.decode(encoding)
                break
            except Exception:
                text = None
        if text is None:
            for encoding in ('gbk', 'cp949', 'shift_jis', 'big5'):
                try:
                    text = raw.decode(encoding)
                    break
                except Exception:
                    text = None
        if text is None:
            text = raw.decode('utf-8', 'replace')
    else:
        text = str(value)
    return ''.join(ch for ch in text if ch >= ' ' and ch != '\x7f')


def _ename(e):
    for n in ('show_name', 'player_name', 'nickname', 'name'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v:
            text = _text(v)
            if text:
                return text
    try:
        text = _text(e.GetName())
        if text:
            return text
    except Exception:
        pass
    return type(e).__name__


def _weapon_case(e):
    for n in ('GetCurWeaponCase', 'GetCurWeapon', 'weapon_case', 'cur_weapon_case'):
        try:
            v = getattr(e, n, None)
        except Exception:
            continue
        if v is None:
            continue
        if callable(v):
            try:
                v = v()
            except Exception:
                continue
        if v is not None:
            return v
    return None


def _gun_name(e):
    try:
        key = getattr(e, 'id', None)
    except Exception:
        key = None
    now = time.time()
    if key is not None:
        c = _gun_cache.get(key)
        if c is not None and now - c[0] < 0.50:
            return c[1]
    result = ''
    wc = _weapon_case(e)
    if wc is not None:
        for n in ('equip_proto', 'body_equip_proto', 'weapon_proto', 'proto'):
            try:
                src = getattr(wc, n, None)
            except Exception:
                continue
            if src is None:
                continue
            try:
                for k in ('name', 'gun_name', 'weapon_name', 'skin_name'):
                    v = src.get(k)
                    if v:
                        result = str(v)
                        break
            except Exception:
                pass
            if result:
                break
        if not result:
            for n in ('gun_name', 'weapon_name', 'name'):
                try:
                    v = getattr(wc, n, None)
                except Exception:
                    continue
                if v:
                    result = str(v)
                    break
    if key is not None:
        _gun_cache[key] = (now, result)
        if len(_gun_cache) > 1024:
            _gun_cache.clear()
    return result


def _fetch_skeleton_joint(e, names):
    m = getattr(e, 'model', None)
    if m is None:
        return None
    for n in names:
        try:
            v = m.GetBoneWorldPosition(n)
            if v is not None:
                return v
        except Exception:
            pass
    return None


def _sample_joint_transform(e, which, ttl=0.008):
    """World-space joint transform, cached for `ttl` seconds.

    The ESP passes a longer ttl for the skeleton joints: sixteen transforms per
    visible player per frame is the single most expensive thing the runtime did,
    and a joint that is up to one frame older costs nothing visually because the
    skeleton is projected with the current camera every frame anyway. The aim
    path keeps the short ttl, where a stale hitbox would matter.
    """
    try:
        key = (getattr(e, 'id', None), int(which))
    except Exception:
        key = None
    now = time.time()
    if key is not None:
        c = _bone_cache.get(key)
        if c is not None and now - c[0] < ttl:
            return c[1]
    which_idx = int(which)
    if 0 <= which_idx < len(BONE_NAMES):
        v = _fetch_skeleton_joint(e, BONE_NAMES[which_idx])
    else:
        v = None
    if key is not None:
        _bone_cache[key] = (now, v)
        if len(_bone_cache) > 2048:
            _bone_cache.clear()
    return v


def _matrix_world_to_screen(v):
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
        return None


def _screen_coord_v2(v):
    p = _matrix_world_to_screen(v)
    return p if p is not None else (-1.0, -1.0)


def _line_of_sight_query(e, head, dist, max_dist):
    key = getattr(e, 'id', None)
    if key is None:
        return False
    point = _vec(head)
    if point is None or not all(math.isfinite(v) for v in point):
        return False
    cache_key = (key, tuple(round(v, 2) for v in point))
    now = time.time()
    c = _vis_cache.get(cache_key)
    if c is not None and now - c[0] < 0.12:
        return c[1]
    if dist > max_dist:
        _vis_cache[key] = (now, False)
        return False
    v = False
    try:
        from gclient import cconst
        cam = _get_cam()
        origin = cam.GetOrigin() if cam is not None else None
        if origin is not None:
            tgt = _v3(point)
            if tgt is not None:
                r = genv.space.RawRaycast(origin, max(1.0, dist + 2.0),
                                          cconst.PHYSICS_SHOOT_TEST, True, to_pos=tgt)
                if r is not None and getattr(r, 'IsHit', False):
                    body = getattr(r, 'Body', None)
                    owner = getattr(body, 'ownerid', None)
                    if owner is None:
                        owner = getattr(getattr(r, 'Actor', None), 'ownerid', None)
                    v = (owner == key)
    except Exception:
        v = False
    _vis_cache[cache_key] = _vis_cache[key] = (now, v)
    if len(_vis_cache) > 2048:
        _vis_cache.clear()
    return v


def _query_occlusion_cache(e, max_age=0.60):
    c = _vis_cache.get(getattr(e, 'id', None))
    if c is None or time.time() - c[0] > max_age:
        return None
    return c[1]


def _check_faction_relation(e, local_av, local_team, team, is_bot, is_monster):
    local_team = _team_id(local_av) or _normalize_team_id(local_team) or _team_id(getattr(genv, 'player', None))
    team = _team_id(e) or _normalize_team_id(team)
    # Team identity wins even in modes where friendly fire allows CanDamage.
    if local_team is not None and team is not None and local_team == team:
        return False
    try:
        space = getattr(e, 'space', None) or getattr(genv, 'space', None)
        gl = getattr(space, 'game_logic', None)
        source = local_av if local_av is not None else getattr(genv, 'player', None)
        if source is not None:
            teammate = getattr(gl, 'IsTeammate', None)
            if callable(teammate):
                try:
                    if teammate(source, e):
                        return False
                except Exception:
                    pass
            damage = getattr(gl, 'CanDamage', None)
            if callable(damage):
                try:
                    result = damage(source, e)
                    if result is not None:
                        return bool(result)
                except Exception:
                    pass
    except Exception:
        pass
    if local_team is not None and team is not None:
        return team != local_team
    # Unknown player relationships stay hidden by default until team data arrives.
    return bool(is_bot or is_monster)


def _collect_space_entities(space):
    """All entities in the space, cached for 30 ms.

    Do NOT call this from the aim/tracking path or anything else that runs while
    the player holds fire: it rebuilds the whole list (list() over every
    category, a set of ids) and walks entity_category keys with str().lower()
    per key. During a match that list is large and changes constantly, so the
    cache almost never hits. It is fine for the ESP gather (already 30 ms
    amortised) and for read-only scans, which is what it was written for.
    """
    global _ent_list_cache
    now = time.time()
    c = _ent_list_cache
    if c is not None and c[0] is space and now - c[1] < 0.03:
        return c[2]
    values = []
    seen = set()

    def add_src(src):
        if not src:
            return 0
        try:
            it = list(src.values())
        except Exception:
            try:
                it = list(src)
            except Exception:
                return 0
        n = 0
        for e in it:
            if e is None:
                continue
            k = id(e)
            if k in seen:
                continue
            seen.add(k)
            values.append(e)
            n += 1
        return n

    # 1. Primary combat avatars direct lookup
    add_src(getattr(space, 'combat_avatars', None))

    # 2. Categorized entities (airborne/parachute/aircraft/vehicles/units)
    cats = getattr(space, 'entity_category', None)
    if cats:
        try:
            keys = list(cats.keys())
        except Exception:
            keys = []
        for key in keys:
            low = str(key).lower()
            if any(w in low for w in ('avatar', 'robot', 'monster', 'combat', 'dummy',
                                      'guard', 'npc', 'enemy', 'unit', 'aircraft',
                                      'parachute', 'glider', 'skydive', 'vehicle', 'player')):
                add_src(cats.get(key))

    # 3. Space entities general pool
    add_src(getattr(space, 'entities', None))
    add_src(getattr(space, 'dummy_entities', None))

    _ent_list_cache = (space, now, values)
    return values


def _sample_target_motion(e, position, now):
    state = _sample_target_motion
    space = getattr(genv, 'space', None)
    if getattr(state, 'space', None) is not space:
        state.space, state.samples = space, {}
    samples = getattr(state, 'samples', None)
    if samples is None:
        samples = state.samples = {}
    position = _vec(position)
    if position is None or not all(math.isfinite(v) for v in position):
        samples.pop(id(e), None)
        return
    previous = samples.get(id(e))
    velocity = (0.0, 0.0, 0.0)
    if previous is not None and previous[0] is e:
        elapsed = now - previous[2]
        if elapsed < 0.001:
            return
        if elapsed <= 0.25:
            raw = tuple((position[i] - previous[1][i]) / elapsed for i in range(3))
            # Discontinuous corrections/teleports must not become a lead vector.
            if _len(raw) <= 100.0:
                alpha = 1.0 - math.exp(-elapsed / 0.04)
                velocity = raw if previous[3] is None else tuple(
                    previous[3][i] + alpha * (raw[i] - previous[3][i]) for i in range(3))
        else:
            velocity = None
    else:
        velocity = None
    if len(samples) >= 512 and id(e) not in samples:
        samples.clear()
    samples[id(e)] = (e, position, now, velocity)


def _calc_ballistic_velocity():
    try:
        case = _current_case(_local()[0])
        sources = [case]
        for name in ('equip_proto', 'body_equip_proto', 'weapon_proto', 'proto'):
            sources.append(getattr(case, name, None))
        for source in sources:
            for name in ('bullet_speed', 'projectile_speed', 'muzzle_velocity'):
                try:
                    value = source.get(name) if hasattr(source, 'get') else _call_attr(source, (name,))
                    speed = float(value)
                    if math.isfinite(speed) and speed > 0.0:
                        return speed
                except (TypeError, ValueError, AttributeError):
                    pass
    except Exception:
        pass
    return None


def _calc_tracking_ratio(scale, dt, theta):
    fraction = _human_frac(scale)
    if theta < 0.08:
        fraction *= max(0.30, theta / 0.08)
    step = dt if dt > 0.0001 else 0.0166667
    fraction = 1.0 - math.pow(max(0.001, 1.0 - fraction), step * 60.0)
    return max(0.08, min(0.80, fraction))


def _estimate_lead_position(e, point, origin, cfg, dt, cur_dir=None, tracking=True):
    if not _flag(cfg, F_TRACK_LEAD):
        return point
    sample = getattr(_sample_target_motion, 'samples', {}).get(id(e))
    now = time.monotonic()
    if sample is None or sample[0] is not e or getattr(_sample_target_motion, 'space', None) is not getattr(genv, 'space', None):
        return point
    age = now - sample[2]
    if age < 0.0 or age > 0.25 or sample[3] is None:
        return point
    velocity = sample[3]
    if not all(math.isfinite(v) for v in velocity):
        return point
    step = max(0.001, min(0.1, float(dt)))
    theta = math.pi
    if cur_dir is not None:
        direction = _norm(_sub(point, origin))
        theta = math.acos(max(-1.0, min(1.0, _dot(_norm(cur_dir), direction))))
    fraction = _calc_tracking_ratio(float(cfg[IDX_TRACK_SMOOTH]), step, theta) if tracking else 1.0
    # Match the camera's smoothing lag; bound it so low FPS does not overlead.
    lead = age + min(0.25, step * (1.0 - fraction) / fraction)
    speed = _calc_ballistic_velocity()
    if speed is not None:
        relative = tuple(point[i] + velocity[i] * lead - origin[i] for i in range(3))
        # Solve |relative + velocity*t| = projectile_speed*t for the first future intercept.
        a = _dot(velocity, velocity) - speed * speed
        b = 2.0 * _dot(relative, velocity)
        c = _dot(relative, relative)
        roots = []
        if abs(a) < 1e-6:
            if abs(b) > 1e-6:
                roots.append(-c / b)
        else:
            discriminant = b*b - 4.0*a*c
            if discriminant >= 0.0:
                root = math.sqrt(discriminant)
                roots.extend(((-b-root)/(2.0*a), (-b+root)/(2.0*a)))
        future = [t for t in roots if math.isfinite(t) and t >= 0.0]
        if future:
            lead += min(1.0, min(future))
    # shortcut: unknown projectile data gets tracking-lag compensation only, until weapon data is available.
    lead = min(1.25, lead)
    return tuple(point[i] + velocity[i] * lead for i in range(3))


def _query_visible_entities(max_dist, want_gun=True, want_bone=True):
    global _g_log_t, _g_slow
    t0 = time.time()
    cam = _get_cam()
    if cam is None:
        return None, None, []
    origin = _vec(cam.GetOrigin())
    if origin is None:
        return cam, None, []
    cam_fwd = _vec(_cam_dir(cam))

    def _front(p):
        vp = _vec(p)
        if vp is None:
            return False
        if cam_fwd is None:
            return True
        return _dot(_sub(vp, origin), cam_fwd) > -0.10

    space = getattr(genv, 'space', None)
    if space is None:
        return cam, origin, []
    local_av, local_id, local_team, _ = _local()
    out = []
    bone_budget = 10
    ents = _collect_space_entities(space)
    t_list = time.time()
    t_bone = 0.0
    for e in ents:
        try:
            if e is local_av:
                continue
            eid = getattr(e, 'id', None)
            if eid is None or (local_id is not None and eid == local_id):
                continue
            # Position and distance first: classifying an entity costs about two
            # dozen flag checks and most of a full space is outside the ESP range.
            pos_raw = _call_attr(e, ('position', 'pos', 'GetPosition'))
            if pos_raw is None:
                m = getattr(e, 'model', None)
                if m is not None:
                    pos_raw = _call_attr(m, ('Position', 'position', 'GetPosition'))
            feet = _vec(pos_raw)
            if feet is not None:
                _sample_target_motion(e, feet, time.monotonic())
                rough = _len(_sub(feet, origin))
                if rough > max_dist + 10.0:
                    continue
            tn = type(e).__name__.lower()
            is_bot, is_monster, is_ai = _ai_kind(e, tn)
            if not _is_combat(e):
                continue
            if not _alive(e):
                continue
            is_dying = _is_dying(e)
            # Behind the camera there is no box to draw, only an arrow, and the
            # arrow needs a direction, not a head joint - so the joint is only
            # fetched for targets that can actually appear on screen.
            in_front = True if feet is None else _front(feet)
            tb = time.time()
            head = _sample_joint_transform(e, 0) if (want_bone and in_front) else None
            t_bone += time.time() - tb
            offset_y = 0.45 if is_dying else 1.62
            if head is None and feet is not None:
                head = _v3((feet[0], feet[1] + offset_y, feet[2]))
            ht = _vec(head)
            if ht is None:
                continue
            if feet is None:
                feet = (ht[0], ht[1] - offset_y, ht[2])
            dist = _len(_sub(ht, origin))
            if dist <= 0.1 or dist > max_dist:
                continue
            ph = _matrix_world_to_screen(head) if _front(head) else None
            pf = _matrix_world_to_screen(feet) if _front(feet) else None
            # Bones and the weapon chip are drawn next to the box, so they are only
            # worth sampling for a target that is actually on screen. This used to
            # run for every entity inside the scan distance: 16 joints each, ~1700
            # engine calls per frame in a full match, which was most of the 8.5 ms
            # the runtime was spending inside a 13 ms frame.
            on_screen = (ph is not None and pf is not None and
                         -0.25 <= ph[0] <= 1.25 and -0.25 <= ph[1] <= 1.25)
            # Keep behind-camera targets for offscreen arrows; tracking checks its own FOV.
            if ph is None:
                ph = (-1.0, -1.0)
            if pf is None:
                pf = (-1.0, -1.0)
            bones_2d = []
            # Skeleton joints are published in WORLD space and projected by the
            # overlay, which has the camera position, forward vector, fov and
            # screen size. The engine's projection measured 0.01 px against the
            # overlay's own ([proj] samples=5 mean_err_px=0.03), so there is no
            # reason to pay a WorldToScreenPercent call per joint per player - that
            # was 16 calls per skeleton, every gather.
            if want_bone and not is_dying and on_screen and bone_budget > 0:
                bone_budget -= 1
                tb = time.time()
                for bi in range(16):
                    bp = _sample_joint_transform(e, bi, 0.016)
                    w3 = _vec(bp) if bp is not None else None
                    if w3 is not None:
                        bones_2d.extend(w3)
                    else:
                        bones_2d.extend((-1.0, -1.0, -1.0))
                t_bone += time.time() - tb
            else:
                bones_2d = [-1.0] * 48
            team = _team_id(e)
            is_team = not _check_faction_relation(e, local_av, local_team, team, is_bot, is_monster)
            tb = time.time()
            gun = _gun_name(e) if (want_gun and on_screen) else ''
            t_bone += time.time() - tb
            out.append((dist, e, eid, ht, feet, ph, pf, bones_2d, (-1.0, -1.0),
                        gun, team, is_team, is_dying))
        except Exception:
            pass
    out.sort(key=lambda x: x[0])
    total = time.time() - t0
    global _g_bone_t
    _g_bone_t = t_bone
    if total > 0.05 and t0 - _g_log_t >= 1.0:
        _g_log_t = t0
        _g_slow = total
        _sys_log('gather slow total=%.3f list=%.3f ents=%d bone=%.3f found=%d' % (
            total, t_list - t0, len(ents), t_bone, len(out)))
    return cam, origin, out[:MAX_ENTITIES]


def _owner_type_name(obj):
    """Class name of a synchronized entity object, as exposed by the game."""
    if obj is None:
        return ''
    try:
        for attr in ('__class__',):
            cls = getattr(obj, attr, None)
            name = getattr(cls, '__name__', '') if cls is not None else ''
            if name:
                return str(name)
    except Exception:
        pass
    try:
        return str(type(obj).__name__)
    except Exception:
        return ''


def _mui_keys(vk):
    if MUI is None:
        return None
    for fn_name in ('GetKeyState', 'IsKeyDown', 'IsKeyPressed', 'GetKeyDown', 'GetKey'):
        try:
            fn = getattr(MUI, fn_name, None)
        except Exception:
            fn = None
        if not callable(fn):
            continue
        try:
            r = fn(vk)
            if isinstance(r, (list, tuple)):
                r = bool(r[0]) if r else False
            if r:
                return True
        except Exception:
            pass
    try:
        keys = MUI.GetAllKeyState()
    except Exception:
        return None
    if keys is None:
        return None
    try:
        if isinstance(keys, dict):
            val = keys.get(vk, keys.get(str(vk), False))
            if isinstance(val, (list, tuple)):
                val = bool(val[0]) if val else False
            return bool(val)
        if isinstance(keys, (list, tuple)):
            if keys and all(isinstance(x, int) for x in keys) and len(keys) <= 32:
                return vk in keys
            if 0 <= vk < len(keys):
                val = keys[vk]
                if isinstance(val, (list, tuple)):
                    val = bool(val[0]) if val else False
                return bool(val)
    except Exception:
        pass
    return None


def _aim_key2(cfg):
    """Second aim key (0 = unbound). Absent on older configs -> unbound."""
    try:
        vk = int(cfg[IDX_AIM_KEY2]) & 0xFF
    except Exception:
        return 0
    return vk if 0 < vk < 256 else 0


def _poll_aim_keys(cfg, primary):
    """True while either the primary or the optional second aim key is held.

    Both keys behave identically; the second one exists so a user can keep the
    default mouse bind and add a keyboard bind without losing either.
    """
    if _poll_input_keystate(primary):
        return True
    second = _aim_key2(cfg)
    return bool(second) and _poll_input_keystate(second)


def _poll_input_keystate(vk):
    vk = int(vk) & 0xFF
    try:
        import ctypes
        if ctypes.windll.user32.GetAsyncKeyState(vk) & 0x8000:
            return True
        if ctypes.windll.user32.GetKeyState(vk) & 0x8000:
            return True
    except Exception:
        pass
    return bool(_mui_keys(vk))


def _key_diag(vk):
    vk = int(vk) & 0xFF
    parts = []
    try:
        parts.append('mui=%r' % (_mui_keys(vk),))
    except Exception as ex:
        parts.append('mui=err %r' % (ex,))
    try:
        import ctypes
        parts.append('async=0x%04X' % (ctypes.windll.user32.GetAsyncKeyState(vk) & 0xFFFF))
        parts.append('sync=0x%04X' % (ctypes.windll.user32.GetKeyState(vk) & 0xFFFF))
    except Exception as ex:
        parts.append('win=err %r' % (ex,))
    return ' '.join(parts)



_MOUSE_INPUT = None
_MOUSEEVENTF_LEFTDOWN = 0x0002
_MOUSEEVENTF_LEFTUP = 0x0004


def _mouse_input_struct():
    """Lazily build INPUT/MOUSEINPUT so a failed import is not cached."""
    global _MOUSE_INPUT
    if _MOUSE_INPUT is not None:
        return _MOUSE_INPUT
    import ctypes
    from ctypes import wintypes

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG),
                    ('mouseData', wintypes.DWORD), ('dwFlags', wintypes.DWORD),
                    ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.POINTER(wintypes.ULONG))]

    class _INPUT_UNION(ctypes.Union):
        _fields_ = [('mi', MOUSEINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [('type', wintypes.DWORD), ('u', _INPUT_UNION)]

    _MOUSE_INPUT = INPUT
    return INPUT


def _send_mouse(flags, dx=0, dy=0):
    """The single mouse output path for the aimbot: SendInput only.

    `mouse_event` is deliberately gone: SendInput is the documented,
    inspectable path and it carries the same MOUSEEVENTF flags. This is the only
    place the runtime injects mouse input, so there is exactly one call to
    audit. Aim movement itself stays on the game's own view placer (see
    _apply_view_transform); this is the click path.
    """
    try:
        import ctypes
        INPUT = _mouse_input_struct()
        inp = INPUT()
        inp.type = 0
        inp.u.mi.dx = int(dx)
        inp.u.mi.dy = int(dy)
        inp.u.mi.dwFlags = int(flags)
        ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(inp))
        return True
    except Exception:
        return False


def _mouse(down):
    """Left button press/release through the single SendInput path."""
    return _send_mouse(_MOUSEEVENTF_LEFTDOWN if down else _MOUSEEVENTF_LEFTUP)


def _trigger_primary_input(down):
    av = getattr(genv, 'avatar', None)
    pl = getattr(genv, 'player', None)
    names = (('StartFire', 'StartShoot', 'OnFire', 'Fire', 'SetFire', 'PressFire') if down else
             ('StopFire', 'StopShoot', 'OffFire', 'ReleaseFire', 'SetFireOff', 'ReleaseFireButton'))
    for obj in (av, pl):
        if obj is None:
            continue
        for n in names:
            try:
                fn = getattr(obj, n, None)
            except Exception:
                continue
            if callable(fn):
                try:
                    fn()
                    return True
                except Exception:
                    pass
    return False


def _cam_dir(cam):
    try:
        cx, cy = gui.device_screen_center
        d = cam.GetRayDirectionFromScreenPoint(int(cx), int(cy))
        d.normalize()
        return d
    except Exception:
        return None


def _placer_dir(placer):
    if placer is None:
        return None
    for obj in (getattr(placer, 'placer', None), placer):
        try:
            if obj is None:
                continue
            t = _vec(obj.Direction)
            if t is None:
                continue
            t = _norm(t)
            if _len(t) < 0.5:
                continue
            v = _v3(t)
            if v is not None:
                return v
        except Exception:
            continue
    return None


def _target_joint_indices(cfg, legacy_bone=0, force_head=False):
    if force_head:
        return (0,)
    options = _options(cfg)
    if options & OPT_TRACK_JOINTS_SET:
        bones = tuple(index for index, flag in ((0, OPT_TRACK_HEAD), (1, OPT_TRACK_NECK), (2, OPT_TRACK_BODY))
                      if options & flag)
        return bones or (0,)
    if _flag(cfg, F_TRACK_MULTIPOINT) or legacy_bone == 4:
        return (0, 1, 2)
    return (legacy_bone,) if legacy_bone in (0, 1, 2, 3) else (0,)


def _pick(items, origin, cam_dir, fov, max_dist, cfg, bone, check_team, check_visible,
          force_head=False, preferred_eid=None):
    try:
        fov = float(fov)
        max_dist = float(max_dist)
        if not math.isfinite(fov) or not math.isfinite(max_dist) or max_dist <= 0.0:
            return None
        half_fov = math.radians(max(1.0, min(180.0, fov)) * 0.5)
    except (TypeError, ValueError):
        return None
    best_dot = math.cos(half_fov)
    team_ok = _flag(cfg, F_TRACK_FACTION)
    vis_only = _flag(cfg, F_TRACK_OCCLUSION)
    best, best_rank = None, None
    bones = _target_joint_indices(cfg, bone, force_head)
    for item in items:
        dist, e, eid, ht = item[0], item[1], item[2], item[3]
        is_team = item[11] if len(item) > 11 else False
        is_dying = (item[12] if len(item) > 12 else False) or _is_dying(e)
        if is_dying and (_options(cfg) & OPT_TRACK_IGNORE_DOWNED):
            continue
        if not _alive(e) or (check_team and is_team and not team_ok):
            continue
        if not math.isfinite(dist) or dist > max_dist:
            continue
        point = None
        for bone_index in bones:
            tgt = _sample_joint_transform(e, bone_index)
            # Single legacy head targeting can use the collected head estimate.
            if tgt is None and bones == (0,):
                tgt = ht
            tt = _vec(tgt)
            if tt is None or not all(math.isfinite(v) for v in tt):
                continue
            td = _sub(tt, origin)
            distance = _len(td)
            if distance < 0.1 or distance > max_dist:
                continue
            dot = max(-1.0, min(1.0, _dot(_norm(cam_dir), _norm(td))))
            if dot < best_dot:
                continue
            if check_visible and vis_only and not _line_of_sight_query(e, tt, distance, max_dist):
                continue
            point = (tt, td, distance, dot)
            break
        if point is None:
            continue
        tt, td, distance, dot = point
        # Center alignment dominates; range breaks close choices without pulling to distant targets.
        score = 0.85 * math.acos(dot) / half_fov + 0.15 * distance / max_dist
        if preferred_eid is not None and str(eid) == str(preferred_eid):
            score -= 0.06
        # Switch out of allies/knocked targets before applying retention preference.
        rank = (bool(is_team) if check_team else False, bool(is_dying), score, distance, str(eid))
        if best_rank is None or rank < best_rank:
            best_rank = rank
            best = (e, tt, td, dist, dot)
    return best


def _pick_eid(items, origin, cam_dir, eid, max_dist, cfg, bone, check_team):
    # Retention must honor the same FOV, visibility, team and knock filters as acquisition.
    candidates = [item for item in items if str(item[2]) == str(eid)]
    return _pick(candidates, origin, cam_dir, float(cfg[IDX_TRACK_FOV]), max_dist, cfg,
                 bone, check_team, True, False, eid)


def _lerp(a, b, t):
    return a + (b - a) * t


def _bezier_ease(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def _ncaf(t):
    t = max(0.0, min(1.0, t))
    return 1.0 - math.pow(1.0 - t, 1.7)


def _human_frac(scale):
    base = max(0.0, min(1.0, scale))
    ease = base * base
    slow = _lerp(base, ease, 0.50)
    fast = _ncaf(base)
    return _lerp(slow, fast, 0.20)


_NOISE_OFF = (0.0, 0.0)


def _micro_jitter(dt, amplitude=0.0016):
    """Coherent, bounded aim-noise offset.

    Only the aimbot path passes a non-zero amplitude (see OPT2_AIM_NOISE and
    _noise_strength). Every other caller keeps the historical amplitude, so a
    noise setting can never leak into view tracking or anything the user did
    not enable it for.
    """
    global _jit_t
    amp = float(amplitude)
    if not math.isfinite(amp) or amp <= 0.0:
        return _NOISE_OFF
    step = dt if dt > 0.0001 else 0.0166667
    _jit_t += step
    yaw = math.sin(_jit_t * 11.7) * amp + math.sin(_jit_t * 29.3 + 1.9) * amp * 0.35
    pitch = math.sin(_jit_t * 9.3 + 0.7) * amp + math.sin(_jit_t * 23.1 + 2.4) * amp * 0.30
    return yaw, pitch


def _turn(cam_dir, td, scale, dt=0.0166667):
    try:
        from MType import Vector3
        from gshare import formula
        d3 = _norm(td)
        target_dir = Vector3(d3[0], d3[1], d3[2])
        cos_t = max(-1.0, min(1.0, cam_dir.dot(target_dir)))
        theta = math.acos(cos_t)
        if theta < 0.0012:
            return False
        frac = _human_frac(scale)
        if theta < 0.06:
            frac *= max(0.22, theta / 0.06)
        frac *= 1.0 + 0.12 * math.sin(_jit_t * 3.1)
        step = dt if dt > 0.0001 else 0.0166667
        frac = 1.0 - math.pow(max(0.001, 1.0 - frac), step * 60.0)
        frac = max(0.01, min(0.85, frac))
        axis = cam_dir.cross(target_dir)
        new_dir = cam_dir.rotate(axis, -theta)
        dy = formula.RadianDifferenceWithDir(cam_dir.yaw, new_dir.yaw)
        dp = formula.RadianDifferenceWithDir(cam_dir.pitch, new_dir.pitch)
        jy, jp = _micro_jitter(dt)
        placer = _get_placer()
        if placer is None or not hasattr(placer, 'TurnView'):
            return False
        placer.TurnView(-dy * frac + jy, -dp * frac + jp)
        return True
    except Exception:
        _sys_log(traceback.format_exc())
        return False


def _slerp_dir(a, b, t):
    dot = max(-1.0, min(1.0, _dot(a, b)))
    if dot > 0.9995:
        return _norm((a[0] + (b[0] - a[0]) * t,
                      a[1] + (b[1] - a[1]) * t,
                      a[2] + (b[2] - a[2]) * t))
    theta = math.acos(dot)
    s = math.sin(theta)
    wa = math.sin((1.0 - t) * theta) / s
    wb = math.sin(t * theta) / s
    return _norm((a[0] * wa + b[0] * wb,
                  a[1] * wa + b[1] * wb,
                  a[2] * wa + b[2] * wb))


def _begin_path(start_dir, target_dir, duration, curve, kind, target_e=None, fire=False):
    global _path
    start, end = _norm(start_dir), _norm(target_dir)
    dot = max(-1.0, min(1.0, _dot(start, end)))
    if dot < 0.0:
        _path = None
        return False
    axis = _norm(_cross(start, end))
    angle = math.acos(dot)
    count = max(3, min(8, int(math.degrees(angle) / 3.0) + 3))
    bend = min(abs(float(curve)), angle * 0.08)
    nodes = []
    for index in range(count):
        u = index / float(count - 1)
        direction = _slerp_dir(start, end, u)
        offset = bend * math.sin(math.pi * u)
        nodes.append(_norm(tuple(direction[i] + axis[i] * offset for i in range(3))))
    nodes[0], nodes[-1] = start, end
    _path = {'start': start, 'end': end, 'nodes': nodes, 't': 0.0,
             'dur': max(0.03, float(duration)), 'kind': kind, 'target': target_e,
             'space': getattr(genv, 'space', None), 'last': time.monotonic(),
             'fire': bool(fire), 'fired': False}
    return True


def _path_sample(path, u):
    nodes = path['nodes']
    u = max(0.0, min(1.0, u))
    if u >= 1.0:
        return nodes[-1]
    segment = u * (len(nodes) - 1)
    index = min(len(nodes) - 2, int(segment))
    t = segment - index
    p0, p1 = nodes[max(0, index-1)], nodes[index]
    p2, p3 = nodes[index+1], nodes[min(len(nodes)-1, index+2)]
    # Catmull-Rom joins node tangents without stopping at every intermediate point.
    return _norm(tuple(0.5 * (2*p1[i] + (-p0[i]+p2[i])*t +
        (2*p0[i]-5*p1[i]+4*p2[i]-p3[i])*t*t +
        (-p0[i]+3*p1[i]-3*p2[i]+p3[i])*t*t*t) for i in range(3)))


def _path_advance(dt):
    global _path
    path = _path
    if path is None:
        return None, True, None
    path['t'] = min(path['dur'], path['t'] + max(0.0, float(dt)))
    u = path['t'] / path['dur']
    if u >= 1.0:
        _path = None
        return path['end'], True, path
    return _path_sample(path, u*u*(3.0-2.0*u)), False, path


def _filter_view_direction(entity, current, target, cfg, dt):
    global _path
    target = _norm(target)
    if not _flag(cfg, F_TRACK_HUMANIZE):
        _path = None
        return target
    now = time.monotonic()
    smooth = float(cfg[IDX_HUMAN_SMOOTH])
    if not math.isfinite(smooth) or not 0.01 <= smooth <= 1.0:
        smooth = 0.5
    path = _path
    reset = (path is None or path['target'] is not entity or
             path['space'] is not getattr(genv, 'space', None) or
             now-path['last'] > 0.25 or dt > 0.15)
    if not reset:
        # Large direction changes need a fresh path from the camera's current heading.
        reset = _dot(path['end'], target) < math.cos(math.radians(12.0))
    if reset:
        duration = 0.04 + 0.20*smooth
        if not _begin_path(current, target, duration, 0.008*smooth, 'human', entity):
            return target
        path = _path
    else:
        delta = _sub(target, path['end'])
        count = len(path['nodes'])
        path['nodes'] = [_norm(tuple(node[j] + delta[j]*i/(count-1) for j in range(3)))
                         for i, node in enumerate(path['nodes'])]
        path['nodes'][-1] = target
        path['end'] = target
    path['last'] = now
    direction, done, completed = _path_advance(max(0.0, min(0.05, dt)))
    if done:
        # Completed paths track the live endpoint; do not restart on every moving frame.
        _path = completed
    return direction


def _apply_placer_delta(cam_dir, node_dir, dt=0.0166667, jitter_scale=0.6):
    try:
        from MType import Vector3
        from gshare import formula
        d3 = _norm(node_dir)
        target_dir = Vector3(d3[0], d3[1], d3[2])
        cos_t = max(-1.0, min(1.0, cam_dir.dot(target_dir)))
        theta = math.acos(cos_t)
        if theta < 0.0008:
            return False
        axis = cam_dir.cross(target_dir)
        new_dir = cam_dir.rotate(axis, -theta)
        dy = formula.RadianDifferenceWithDir(cam_dir.yaw, new_dir.yaw)
        dp = formula.RadianDifferenceWithDir(cam_dir.pitch, new_dir.pitch)
        jy, jp = _micro_jitter(dt)
        placer = _get_placer()
        if placer is None or not hasattr(placer, 'TurnView'):
            return False
        placer.TurnView(-dy + jy * jitter_scale, -dp + jp * jitter_scale)
        return True
    except Exception:
        _sys_log(traceback.format_exc())
        return False


def _cam_interp_interval(scale):
    s = max(0.01, min(1.0, float(scale)))
    return max(0.05, min(0.30, 0.05 + (1.0 - s) * 0.22))


def _wrap_pi(a):
    while a > math.pi:
        a -= 2.0 * math.pi
    while a < -math.pi:
        a += 2.0 * math.pi
    return a


def _calibrate_turn(placer):
    global _track_sign_y, _track_sign_p, _track_sign_ready
    try:
        from MType import Vector3
    except Exception:
        _track_sign_ready = True
        return
    try:
        d0 = _vec(_placer_dir(placer))
        if d0 is not None:
            v0 = Vector3(d0[0], d0[1], d0[2])
            placer.TurnView(0.01, 0.0)
            d1 = _vec(_placer_dir(placer))
            if d1 is not None:
                v1 = Vector3(d1[0], d1[1], d1[2])
                dyaw = _wrap_pi(v1.yaw - v0.yaw)
                if abs(dyaw) > 1e-5:
                    _track_sign_y = 1.0 if dyaw > 0.0 else -1.0
            placer.TurnView(-0.01, 0.0)
            d2 = _vec(_placer_dir(placer)) or d1 or d0
            v2 = Vector3(d2[0], d2[1], d2[2])
            placer.TurnView(0.0, 0.01)
            d3 = _vec(_placer_dir(placer))
            if d3 is not None:
                v3 = Vector3(d3[0], d3[1], d3[2])
                dpitch = _wrap_pi(v3.pitch - v2.pitch)
                if abs(dpitch) > 1e-5:
                    _track_sign_p = 1.0 if dpitch > 0.0 else -1.0
            placer.TurnView(0.0, -0.01)
    except Exception:
        _sys_log(traceback.format_exc())
    _track_sign_ready = True


def _apply_view_transform(placer, cur_dir, target_dir, scale, dt=0.0166667, noise=0.0):
    global _track_fallback_n
    try:
        from MType import Vector3
        tgt = _vec(target_dir)
        if tgt is None:
            return False
        tgt = _norm(tgt)
        if _len(tgt) < 0.5:
            return False
        base = _vec(_placer_dir(placer))
        if base is None:
            base = _vec(cur_dir)
        if base is None:
            return False
        base = _norm(base)
        cos_t = max(-1.0, min(1.0, _dot(base, tgt)))
        theta = math.acos(cos_t)
        if theta < 0.0015:
            return False
        frac = _calc_tracking_ratio(scale, dt, theta)
        # `noise` is the aimbot's OPT2_AIM_NOISE amplitude; 0 keeps this path
        # exactly as it was before the Comet port.
        jy, jp = _micro_jitter(dt, noise)
        if placer is None or not hasattr(placer, 'TurnView'):
            return False
        if not _track_sign_ready:
            _calibrate_turn(placer)
            base = _vec(_placer_dir(placer)) or base
        cy = Vector3(base[0], base[1], base[2])
        ty = Vector3(tgt[0], tgt[1], tgt[2])
        dy = _wrap_pi(ty.yaw - cy.yaw)
        dp = _wrap_pi(ty.pitch - cy.pitch)
        turn_y = _track_sign_y * dy * frac + jy
        turn_p = _track_sign_p * dp * frac + jp
        before = base
        try:
            placer.TurnView(turn_y, turn_p)
        except Exception:
            pass
        after = _vec(_placer_dir(placer)) or base
        moved = math.degrees(math.acos(max(-1.0, min(1.0, _dot(before, after)))))
        if moved < 0.0008:
            _track_fallback_n += 1
            rot = getattr(placer, 'Rotate', None)
            if callable(rot):
                try:
                    rot(turn_y, turn_p)
                except Exception:
                    pass
                after = _vec(_placer_dir(placer)) or base
                moved = math.degrees(math.acos(max(-1.0, min(1.0, _dot(before, after)))))
            if moved < 0.0008:
                step_dir = _slerp_dir(base, tgt, max(0.02, min(0.90, frac)))
                inner = getattr(placer, 'placer', None) or placer
                try:
                    inner.Direction = Vector3(step_dir[0], step_dir[1], step_dir[2])
                except Exception:
                    return False
                tgt_obj = getattr(placer, 'target', None)
                if tgt_obj is not None:
                    try:
                        tgt_obj.SetPitchAndYaw(0, inner.Direction.yaw)
                    except Exception:
                        pass
        return True
    except Exception:
        _sys_log(traceback.format_exc())
        return False


def _damp_impulse_call(placer, original, args, kwargs):
    saved = []
    try:
        # Magnitudes consumed by FpsPlacer impulse FSM.
        for name in ('RecoilYawRange', 'RecoilPitchRange', 'RecoilDownYawRange',
                     'RecoilDownPitchRange', 'RecoilDownCyclePct', 'forcePersistRecoilRecover'):
            if hasattr(placer, name):
                value = getattr(placer, name)
                setattr(placer, name, False if name == 'forcePersistRecoilRecover' else 0.0)
                saved.append((name, value))
        return original(*args, **kwargs)
    finally:
        for name, value in reversed(saved):
            setattr(placer, name, value)


def _restore_impulse_state():
    global _impulse_hooks
    state, _impulse_hooks = _impulse_hooks, None
    if state is None:
        return
    placer, hooks = state
    for name, wrapper, had_local, previous in reversed(hooks):
        if getattr(placer, name, None) is wrapper:
            if had_local:
                setattr(placer, name, previous)
            else:
                delattr(placer, name)


# ---------------------------------------------------------------------------
# No-recoil (recoil-probe 2026-11-05): hook WeaponCase class methods at source.
# Zeroing GetRealRecoilData / GetRealRecoilRandomData / GetRealRollShakeInfo
# triggers the FSM but with zero pattern/random/shake values, so the camera
# easing in StateRecoil.RecoilUp rotates to no offset. Keeps return shape so
# we don't break downstream consumers that expect tuples or objects.
# ---------------------------------------------------------------------------
_wc_damp_hooks = None  # (cls, [(name, wrapper, had_class_attr, original_attr), ...])

_IMPULSE_FIELD_KEYS = ('yaw', 'pitch', 'range', 'shake', 'roll', 'power',
                     'amount', 'strength', 'magnitude', 'intensity', 'force')
# Per-type cache so each recoil-data class pays introspection cost ONCE.
_impulse_field_cache = {}

def _suppress_impulse_metric(result):
    if result is None:
        return result
    t = type(result)
    if t is float: return 0.0
    if t is int:   return 0
    if t is tuple:
        # Hot path: pattern/random data is almost always a 2-tuple of floats.
        return (0.0,) * len(result)
    if t is list:
        return [0.0] * len(result)
    # Object. Use cached field list; build once per type.
    names = _impulse_field_cache.get(t)
    if names is None:
        try:
            vd = vars(result)
            names = tuple(n for n in vd
                          if not n.startswith('_') and any(k in n.lower() for k in _IMPULSE_FIELD_KEYS))
        except Exception:
            names = ()
        _impulse_field_cache[t] = names
    for n in names:
        try:
            v = getattr(result, n, None)
            if type(v) is float: setattr(result, n, 0.0)
            elif type(v) is int: setattr(result, n, 0)
        except Exception:
            pass
    return result


def _sync_weapon_case_damping(cfg, in_match):
    """Install/remove class-level hooks on WeaponCase so every weapon returns
    zero recoil pattern + zero random bounds + zero roll shake."""
    global _wc_damp_hooks
    want = bool(in_match) and _flag(cfg, F_DAMP_IMPULSE)

    if not want:
        if _wc_damp_hooks is not None:
            cls, hooks = _wc_damp_hooks
            for name, wrapper, had_attr, original in reversed(hooks):
                if getattr(cls, name, None) is wrapper:
                    if had_attr:
                        setattr(cls, name, original)
                    else:
                        try: delattr(cls, name)
                        except Exception: pass
            _wc_damp_hooks = None
        return

    if _wc_damp_hooks is not None:
        return  # already installed

    try:
        import gclient.gameplay.logic_base.equips.weapon_case as wc_mod
    except Exception:
        return
    cls = getattr(wc_mod, 'WeaponCase', None)
    if cls is None:
        return
    targets = ('GetRealRecoilData',
               'GetRealRecoilRandomData',
               'GetRealRollShakeInfo',
               'GetRealPersistRecoilDownTimeMax')
    hooks = []
    for name in targets:
        original = getattr(cls, name, None)
        if not callable(original):
            continue
        had_attr = name in cls.__dict__
        def make_wrap(orig, lbl):
            def wrapper(self, *args, **kwargs):
                result = orig(self, *args, **kwargs)
                return _suppress_impulse_metric(result)
            wrapper._m_recoil_orig = orig
            return wrapper
        wrapper = make_wrap(original, name)
        try:
            setattr(cls, name, wrapper)
            hooks.append((name, wrapper, had_attr, original))
        except Exception:
            continue
    _wc_damp_hooks = (cls, hooks)
    _sys_log('[recoil] weapon_case hooks installed=%d targets=%d' % (len(hooks), len(targets)))


def _sync_impulse_damping(cfg, in_match):
    global _impulse_hooks
    placer = _get_placer() if in_match and _flag(cfg, F_DAMP_IMPULSE) else None
    if _impulse_hooks is not None:
        if placer is _impulse_hooks[0]:
            return
        _restore_impulse_state()
    if placer is None:
        return
    methods = ('RecoilUp', 'RecoilFall', 'RecoilDown')
    if not all(callable(getattr(placer, name, None)) for name in methods):
        return
    hooks = []
    _impulse_hooks = (placer, hooks)
    try:
        for name in methods:
            current = getattr(placer, name)
            original = getattr(current, '_m_damp_orig', current)
            local = getattr(placer, '__dict__', {})
            restore = getattr(current, '_m_damp_rest', (name in local, local.get(name)))
            def wrapper(*args, _original=original, **kwargs):
                return _damp_impulse_call(placer, _original, args, kwargs)
            wrapper._m_damp_orig = original
            wrapper._m_damp_rest = restore
            setattr(placer, name, wrapper)
            hooks.append((name, wrapper, restore[0], restore[1]))
    except Exception:
        _restore_impulse_state()
        _sys_log('[sys:impulse] filter unavailable: ' + traceback.format_exc())


def _query_corrected_ray(worker, native):
    cfg = _read_cfg()
    if (not _flag(cfg, F_TRACK_VIEW) or not (_options(cfg) & OPT_RAY_REDIRECT) or
            _flag(cfg, F_MENU_OPEN) or bool(getattr(genv, 'is_in_hall', True)) or
            getattr(worker, 'caster', None) is not getattr(genv, 'player', None)):
        return native
    cached = _ray_target
    if cached is None or not _track_busy:
        return native
    entity, point, stamp, space = cached
    age = time.time() - stamp
    if space is not getattr(genv, 'space', None) or age < 0 or age > 0.10 or not _alive(entity):
        return native
    cam = _get_cam()
    origin = _vec(cam.GetOrigin()) if cam is not None else None
    if origin is None:
        return native
    delta = _sub(point, origin)
    distance = _len(delta)
    if not math.isfinite(distance) or distance < 0.1 or distance > float(cfg[IDX_TRACK_MAX_DIST]):
        return native
    direction = _norm(delta)
    forward = _cam_dir(cam)
    if forward is None or _dot(_norm(forward), direction) < math.cos(math.radians(float(cfg[IDX_TRACK_FOV]) * .5)):
        return native
    from MType import Vector3
    return Vector3(*direction)


def _sync_ray_interceptor(cfg, in_match):
    global _ray_hook, _ray_target
    enabled = in_match and _flag(cfg, F_TRACK_VIEW) and bool(_options(cfg) & OPT_RAY_REDIRECT)
    if not enabled:
        _ray_target = None
        if _ray_hook is not None:
            cls, wrapper, original = _ray_hook
            if getattr(cls, 'GetShootDir', None) is wrapper:
                cls.GetShootDir = original
            _ray_hook = None
        return
    module = sys.modules.get('gclient.gameplay.logic_base.spell.spell_core.gun_spell')
    cls = getattr(module, 'SpellWorker', None)
    current = getattr(cls, 'GetShootDir', None)
    if _ray_hook is not None and current is _ray_hook[1]:
        return
    if _ray_hook is not None:
        old_cls, old_wrapper, old_original = _ray_hook
        if getattr(old_cls, 'GetShootDir', None) is old_wrapper:
            old_cls.GetShootDir = old_original
        _ray_hook = None
    original = getattr(current, '_m_ray_orig', current)
    code = getattr(original, '__code__', None)
    if (code is None or code.co_argcount != 2 or code.co_varnames[:2] != ('self', 'shoot_pos') or
            'GetRayDirectionFromScreenPoint' not in code.co_names):
        return
    def wrapper(worker, shoot_pos):
        # Keep native scatter/heat bookkeeping before selecting this shot's direction.
        native = original(worker, shoot_pos)
        try:
            return _query_corrected_ray(worker, native)
        except Exception:
            return native
    wrapper._m_ray_orig = original
    cls.GetShootDir = wrapper
    _ray_hook = (cls, wrapper, original)


def _update_cam_trajectory(cfg, items, cam, origin, menu_open, dt=0.0166667):
    global _track_prev, _track_latched, _track_busy, _path
    global _track_hold_until, _track_diag_t, _track_fallback_n
    global _track_eid, _track_eid_t
    global _ray_target
    _ray_target = None
    redirect = bool(_options(cfg) & OPT_RAY_REDIRECT)
    now = time.time()
    track_key = int(cfg[IDX_TRACK_KEY])
    track_enabled = _flag(cfg, F_TRACK_VIEW)
    track_down = track_enabled and _poll_aim_keys(cfg, track_key)
    if track_down:
        _track_hold_until = now + 0.30
    held = track_down or now < _track_hold_until
    if _get_track_mode_toggle(cfg) and track_enabled:
        if track_down and not _track_prev:
            _track_latched = not _track_latched
        track_on = _track_latched
    else:
        _track_latched = False
        track_on = held
    if track_down != _track_prev:
        _sys_log('[sys:track] key %d %s held=%d %s' % (
            track_key, 'down' if track_down else 'up', 1 if held else 0, _key_diag(track_key)))
    _track_prev = track_down
    if menu_open:
        track_on = False
        _track_latched = False
        _path = None
    _track_busy = track_on
    if cam is None or origin is None:
        _path = None
        return
    placer = _get_placer()
    cur = _cam_dir(cam) or _placer_dir(placer)
    if cur is None:
        _path = None
        return

    if not track_on or not items:
        _track_eid, _track_eid_t = None, 0.0
        _path = None
        return

    if track_on:
        if not redirect and (placer is None or not hasattr(placer, 'TurnView')):
            if now - _track_diag_t >= 1.0:
                _track_diag_t = now
                _sys_log('[cam:track] no TurnView placer=%s turn=%s' %
                     (type(placer).__name__, hasattr(placer, 'TurnView')))
            return
        bone = int(cfg[IDX_TRACK_JOINT])
        max_dist = float(cfg[IDX_TRACK_MAX_DIST])
        preferred = _track_eid if now - _track_eid_t < 0.60 else None
        best = _pick(items, origin, cur, float(cfg[IDX_TRACK_FOV]), max_dist, cfg, bone, True, True, False, preferred)
        if best is None and _track_eid is not None and now - _track_eid_t < 0.60:
            best = _pick_eid(items, origin, cur, _track_eid, max_dist, cfg, bone, True)
        if best is None:
            _path = None
            _track_eid, _track_eid_t = None, 0.0
            if now - _track_diag_t >= 1.0:
                _track_diag_t = now
                _sys_log('[cam:track] no target in fov items=%d fov=%.1f placer=%s %s' % (
                    len(items), float(cfg[IDX_TRACK_FOV]), type(placer).__name__,
                    _key_diag(track_key)))
            return
        e, tt, td, dist, dot = best
        tt = _estimate_lead_position(e, tt, origin, cfg, dt, cur, tracking=not redirect)
        td = _sub(tt, origin)
        try:
            _track_eid = int(getattr(e, 'id', 0) or 0) or None
            _track_eid_t = now
        except Exception:
            _track_eid = None
        _track_busy = True
        if now - _track_diag_t >= 1.0:
            _track_diag_t = now
            c = _vec(cur) or (0.0, 0.0, 1.0)
            t = _norm(_vec(td) or (0.0, 0.0, 1.0))
            theta = math.degrees(math.acos(max(-1.0, min(1.0, _dot(_norm(c), t)))))
            _sys_log('[cam:track] target dist=%.1f theta=%.2f fallback=%d items=%d %s' % (
                dist, theta, _track_fallback_n, len(items), _key_diag(track_key)))
        if redirect:
            _path = None
            _ray_target = (e, tt, now, getattr(genv, 'space', None))
            return
        direction = _filter_view_direction(e, cur, td, cfg, dt)
        noise = _noise_strength(cfg) if _opt2(cfg, OPT2_AIM_NOISE) else 0.0
        _apply_view_transform(placer, cur, direction, float(cfg[IDX_TRACK_SMOOTH]), dt, noise)


def _current_case(entity):
    try:
        cur = _weapon_case(entity)
        if cur is not None:
            return cur
    except Exception:
        pass
    av = getattr(genv, 'avatar', None)
    if av is not None and av is not entity:
        try:
            cur = _weapon_case(av)
            if cur is not None:
                return cur
        except Exception:
            pass
    return None


def _all_cases(entity):
    cases = []
    seen = set()
    def add_case(c):
        if c is not None and id(c) not in seen:
            seen.add(id(c))
            cases.append(c)

    cur = _current_case(entity)
    add_case(cur)
    av = getattr(genv, 'avatar', None)
    for ent in (entity, av):
        if ent is None:
            continue
        hm = getattr(ent, 'hand_model', None)
        if hm is not None:
            for fn_name in ('GetCurClientWeaponCase', 'GetCurWeaponCase', 'GetWeaponCase'):
                try:
                    fn = getattr(hm, fn_name, None)
                    if callable(fn):
                        add_case(fn())
                except Exception:
                    pass
            for wd_name in ('weapon_dict', 'weapon_case_dict'):
                try:
                    wd = getattr(hm, wd_name, None)
                    if isinstance(wd, dict):
                        for c in list(wd.values()) + list(wd.keys()):
                            add_case(c)
                except Exception:
                    pass
        for m_name in ('model', 'hand_model'):
            md = getattr(ent, m_name, None)
            if md is not None:
                try:
                    wd = getattr(md, 'weapon_dict', None)
                    if isinstance(wd, dict):
                        for c in list(wd.values()) + list(wd.keys()):
                            add_case(c)
                except Exception:
                    pass
        for n in ('weapon_case_mgr', 'GetWeaponCaseMgr', 'weapon_case_dict'):
            try:
                m = getattr(ent, n, None)
            except Exception:
                continue
            if m is None:
                continue
            if callable(m):
                try:
                    m = m()
                except Exception:
                    continue
            if m is None:
                continue
            d = getattr(m, 'weapon_case_dict', None)
            if d is None and isinstance(m, dict):
                d = m
            if d:
                try:
                    for c in list(d.values()) + list(d.keys()):
                        add_case(c)
                except Exception:
                    pass
    return cases


def _find_case(entity, gid):
    if gid <= 0:
        return None
    for wc in _all_cases(entity):
        try:
            if int(getattr(wc, 'gun_id', 0) or 0) == gid:
                return wc
        except Exception:
            pass
    return None


def _call_path(obj, names, variants):
    for name in names:
        try:
            fn = getattr(obj, name, None)
        except Exception:
            continue
        if not callable(fn):
            continue
        for args in variants:
            try:
                fn(*args)
                return name
            except TypeError:
                continue
            except Exception:
                continue
    return ''


def _item_transient_check(proto):
    params = proto.get('use_code_params') or {}
    return bool(params.get('use_time') or proto.get('end_time') or
                proto.get('limited_item_time') or proto.get('use_code') == 'PlaceholderItem')


def _target_mesh_avatar():
    if bool(getattr(genv, 'is_in_hall', True)):
        return None
    player = getattr(genv, 'player', None)
    candidates = [player]
    if player is not None:
        candidates.append(getattr(player, 'combat_avatar', None))
    candidates.extend((_local()[0], getattr(genv, 'avatar', None)))
    for entity in candidates:
        if entity is None:
            continue
        for name in ('IsPlayerCombatAvatar', 'IsCombatAvatar'):
            value = getattr(entity, name, False)
            if callable(value):
                value = value()
            if value:
                return entity
    return None


def _slot_is_melee_profile(case):
    for cls in type(case).__mro__:
        if cls.__name__ in ('MeleeCase', 'DualMeleeCase', 'HeirloomCase'):
            return True
    value = getattr(case, 'IsMelee', False)
    return bool(value() if callable(value) else value)


def _slot_weapon_identifier(case):
    gid = int(getattr(case, 'gun_id', 0) or 0)
    if gid:
        return gid
    for name in ('equip_proto', 'body_equip_proto'):
        proto = getattr(case, name, None)
        if proto is not None:
            gid = int(proto.get('gun_id') or 0)
            if gid:
                return gid
    return 0


def _resolve_asset_variant(selection, typ):
    import gclient.data.lobby_item_data as lobby
    import gclient.data.gun_skin_template_data as gun
    wanted = (510, 520, 530, 540, 550, 560) if typ == 2 else (201, 202, 203, 206, 207, 210)
    item = lobby.data.get(selection)
    template_id = 0
    if item is not None:
        if int(item.get('sub_type') or 0) not in wanted:
            raise ValueError('selected item does not belong to this category')
        if _item_transient_check(item):
            raise ValueError('selected item is temporary or a placeholder')
        template_id = int((item.get('guise') or {}).get('template_id') or 0)
    else:
        template_id = int(selection)
        revision = (id(lobby.data), len(lobby.data))
        if getattr(_resolve_asset_variant, 'template_revision', None) != revision:
            _resolve_asset_variant.template_items = {}
            _resolve_asset_variant.template_revision = revision
        cached = _resolve_asset_variant.template_items
        key = (typ, template_id)
        if key not in cached:
            item = None
            for candidate in lobby.data.values():
                if int(candidate.get('sub_type') or 0) not in wanted or _item_transient_check(candidate):
                    continue
                if int((candidate.get('guise') or {}).get('template_id') or 0) != template_id:
                    continue
                if item is None or int(candidate['id']) < int(item['id']):
                    item = candidate
            # Missing saved templates must not rescan the catalog every tick.
            cached[key] = item
        item = cached[key]
    if item is None or not template_id:
        raise ValueError('no permanent item matches the selected template')
    template = gun.data.get(template_id)
    if template is None:
        raise ValueError('item template is missing')
    if int(item.get('sub_type') or 0) == 210:
        raise ValueError('Artifact requires blade/handle/core/effect attributes; a template ID alone is insufficient')
    return item, template, template_id


_visual_fx_bindings = {}


def _resolve_visual_descriptor(info, caster_id=None, weapon_hint=None):
    if not hasattr(info, 'get'):
        return info
    local_id = getattr(getattr(genv, 'player', None), 'id', None)
    caster = info.get('caster_id') or info.get('killer_id') or caster_id
    if not local_id or str(caster) != str(local_id):
        return info
    weapon_id = info.get('weapon_id') or (weapon_hint or {}).get('weapon_id')
    gun_id = info.get('gun_id')
    binding = _visual_fx_bindings.get(weapon_id)
    if binding is None and gun_id:
        binding = next((b for b in _visual_fx_bindings.values() if b['gun_id'] == gun_id), None)
    if binding is None:
        template_id = info.get('guise_template_id')
        module = sys.modules.get('gclient.data.gun_skin_template_data')
        template = getattr(module, 'data', {}).get(template_id) or {}
        gun_id = template.get('gun_id')
        if gun_id:
            binding = next((b for b in _visual_fx_bindings.values() if b['gun_id'] == gun_id), None)
    if (binding is None or binding['space'] is not getattr(genv, 'space', None) or
            str(binding['owner_id']) != str(local_id)):
        return info
    result = dict(info)
    result.update(guise_item_id=binding['item_id'], guise_template_id=binding['template_id'])
    if info.get('kill_team_guise_item_id'):
        result.update(kill_team_guise_item_id=binding['item_id'],
                      kill_team_guise_template_id=binding['template_id'])
    return result


def _proxy_visual_attribute(original, field):
    def getter(case):
        binding = getattr(case, '_m_fx_meta', None)
        if (binding is not None and binding['space'] is getattr(genv, 'space', None) and
                str(binding['owner_id']) == str(getattr(getattr(genv, 'player', None), 'id', None)) and
                binding['template_id'] == getattr(case, 'guise_id', None)):
            features = sys.modules.get('gclient.data.item_feature_data')
            templates = sys.modules.get('gclient.data.gun_skin_template_data')
            feature = getattr(features, 'data', {}).get(binding['item_id']) or {}
            template = getattr(templates, 'data', {}).get(binding['template_id']) or {}
            keys = ('fps_tracer_bullet_id', 'fps_adstracer_bullet_id', 'tps_tracer_bullet_id',
                    'fps_simple_tracer_bullet_id', 'extra_ballistic_tracer_bullet_id') if field == 'bullet_trace' else ('skin_hit_object_effect_id',)
            return bool(feature.get(field) and any(template.get(k) for k in keys))
        return original(case)
    getter.__name__ = original.__name__
    getter._m_fx_orig = original
    getter._m_fx_owner = globals()
    return getter


def _proxy_hud_effect(original):
    def push(component, info, *args, **kwargs):
        return original(component, _resolve_visual_descriptor(info), *args, **kwargs)
    push._m_fx_orig = original
    push._m_fx_owner = globals()
    return push


def _proxy_death_effect(original):
    def play(model, info, *args, **kwargs):
        import types
        owner = getattr(model, 'owner', None)
        raw = getattr(owner, 'final_kill_info', None)
        decoder = original.__globals__.get('cPickle')
        if not raw or decoder is None:
            return original(model, info, *args, **kwargs)
        try:
            decoded = decoder.loads(raw)
            cooked = _resolve_visual_descriptor(decoded, getattr(owner, 'dead_killer', None), info)
        except Exception:
            return original(model, info, *args, **kwargs)
        if cooked is decoded:
            return original(model, info, *args, **kwargs)
        # Isolate decoding for this render call; replicated entity data stays intact.
        namespace = dict(original.__globals__)
        namespace['cPickle'] = types.SimpleNamespace(loads=lambda value: cooked if value is raw else decoder.loads(value))
        render = types.FunctionType(original.__code__, namespace, original.__name__,
                                    original.__defaults__, original.__closure__)
        render.__kwdefaults__ = original.__kwdefaults__
        return render(model, info, *args, **kwargs)
    play._m_fx_orig = original
    play._m_fx_owner = globals()
    return play


def _on_elimination_event(original, entity):
    def callback(caster, *args, **kwargs):
        case = _current_case(entity)
        hint = {'weapon_id': getattr(case, 'weapon_id', None)} if case is not None else None
        return original(_resolve_visual_descriptor(caster, getattr(entity, 'id', None), hint), *args, **kwargs)
    callback._m_fx_orig = original
    callback._m_fx_owner = globals()
    return callback


def _install_visual_proxies():
    module = sys.modules.get('gclient.gameplay.logic_base.equips.weapon_case_base')
    cls = getattr(module, 'WeaponCaseBase', None)
    if cls is not None:
        for name, field in (('show_guise_bullet_trace', 'bullet_trace'), ('show_guise_hit_effect', 'hit_effect')):
            descriptor = vars(cls).get(name)
            current = getattr(descriptor, 'func', None)
            if callable(current) and getattr(current, '_m_fx_owner', None) is not globals():
                descriptor.func = _proxy_visual_attribute(getattr(current, '_m_fx_orig', current), field)
    module = sys.modules.get('gclient.gameplay.uicomponents.hud_killrecord_comp')
    for cls in list(vars(module).values()) if module is not None else ():
        if not isinstance(cls, type):
            continue
        current = vars(cls).get('PushKillRecord')
        if callable(current) and getattr(current, '_m_fx_owner', None) is not globals():
            cls.PushKillRecord = _proxy_hud_effect(getattr(current, '_m_fx_orig', current))
    module = sys.modules.get('gclient.gameplay.logic_base.models.avatar_model')
    cls = getattr(module, 'AvatarModel', None)
    current = getattr(cls, 'PlaySkinDeadAnimAndEffect', None)
    if callable(current) and getattr(current, '_m_fx_owner', None) is not globals():
        cls.PlaySkinDeadAnimAndEffect = _proxy_death_effect(getattr(current, '_m_fx_orig', current))
    entity = getattr(genv, 'player', None)
    for name in ('KillAvatar', 'KnockDownAvatar'):
        current = getattr(entity, name, None)
        if callable(current) and getattr(current, '_m_fx_owner', None) is not globals():
            setattr(entity, name, _on_elimination_event(getattr(current, '_m_fx_orig', current), entity))


def _bind_visual_metadata(case, template_id, item_id):
    binding = {'template_id': template_id, 'item_id': item_id,
               'gun_id': _slot_weapon_identifier(case), 'space': getattr(genv, 'space', None),
               'owner_id': getattr(getattr(genv, 'player', None), 'id', None)}
    previous = getattr(case, '_m_fx_meta', None)
    extra = getattr(case, 'extra', None)
    if (previous == binding and int(getattr(case, 'guise_item_id', 0) or 0) == item_id and
            (extra is None or extra.get('guise_item_id') == item_id)):
        return
    case._m_fx_meta = binding
    try:
        case.guise_item_id = item_id
        if extra is not None:
            extra['guise_item_id'] = item_id
        clear = getattr(case, 'ClearGuiseItemCache', None)
        if callable(clear):
            clear()
        init = getattr(case, 'InitFpsProperty', None)
        if callable(init) and getattr(case, 'is_fps_weapon', False):
            init()
        refresh = getattr(case, 'RefreshRacerForChangeGuise', None)
        if callable(refresh):
            refresh()
    except Exception:
        case._m_fx_meta = None
        raise
    if _visual_fx_bindings:
        prior = next(iter(_visual_fx_bindings.values()))
        if prior['space'] is not binding['space'] or prior['owner_id'] != binding['owner_id']:
            _visual_fx_bindings.clear()
    _visual_fx_bindings[int(getattr(case, 'weapon_id', 0) or binding['gun_id'])] = binding


def _update_model_appearance(entity, item, typ):
    try:
        selected, template, template_id = _resolve_asset_variant(int(item), int(typ))
        gid = int(template.get('gun_id') or 0)
        cases = []
        for case in _all_cases(entity):
            if not callable(getattr(case, 'ChangeWeaponGuise', None)):
                continue
            if not bool(getattr(case, 'is_in_combat', False)):
                continue
            if _slot_is_melee_profile(case) != (typ == 3):
                continue
            if typ == 2 and _slot_weapon_identifier(case) != gid:
                continue
            cases.append(case)
        if typ == 2 and gid <= 0:
            raise ValueError('selected item has no gun_id')
        if not cases:
            raise ValueError('matching combat weapon case is not loaded; equip the selected weapon first')
        expected_equip = int(template.get('replace_equip_id') or 0)
        for case in cases:
            if typ == 3 and expected_equip and int(getattr(case, 'weapon_id', 0) or 0) != expected_equip:
                import gclient.data.equip_data as equips
                target = equips.data.get(expected_equip) or {}
                current = getattr(case, 'equip_proto', None) or {}
                if not target.get('equip_case') or target.get('equip_case') != current.get('equip_case'):
                    raise ValueError('selected melee skin requires a different weapon class; equip that melee class first')
        _install_visual_proxies()
        accepted = 0
        for case in cases:
            item_id = int(selected['id'])
            if int(getattr(case, 'guise_id', 0) or 0) != template_id:
                # Dispatch through the actual case subclass, including DualMeleeCase.
                case.ChangeWeaponGuise(template_id, guise_item_id=item_id)
            if int(getattr(case, 'guise_id', 0) or 0) != template_id:
                raise ValueError('weapon case did not accept the template (model/resources may be unavailable)')
            _bind_visual_metadata(case, template_id, item_id)
            accepted += 1
        return 'ChangeWeaponGuise template=%d item=%d cases=%d' % (template_id, int(selected['id']), accepted)
    except Exception as error:
        _sys_log('[sys:asset] request failed type=%d selection=%d: %s: %s' %
                 (typ, item, type(error).__name__, error))
        return ''



def _apply_hero(entity, item):
    import gclient.data.lobby_item_data as lobby
    import gshare.consts as consts
    try:
        proto = lobby.data.get(item)
        if proto is None or int(proto.get('sub_type') or 0) != 300 or _item_transient_check(proto):
            raise ValueError('not a permanent character item')
        current_hero = _hero_of(entity) or _local_hero_id()
        wanted_hero = int(proto.get('hero_id') or 0)
        if not wanted_hero or current_hero != wanted_hero:
            raise ValueError('character hero=%d does not match current combat hero=%d' % (wanted_hero, current_hero))
        clothes = getattr(entity, 'arm_clothes', None)
        callback = getattr(entity, '_on_set_arm_clothes', None)
        display = getattr(entity, 'GetDisplayHeroSkinItemId', None)
        if clothes is None or not callable(callback) or not callable(display):
            raise ValueError('combat avatar clothing callback is unavailable')
        suit_key = consts.ItemSubType.Suit
        old = dict(clothes)
        if int(display() or 0) == item:
            return 'arm_clothes already selected item=%d' % item
        try:
            clothes[suit_key] = item
            if int(display() or 0) != item:
                raise ValueError('display state did not accept the selected clothing item')
            callback(old)
        except Exception:
            if suit_key in old:
                clothes[suit_key] = old[suit_key]
            else:
                clothes.pop(suit_key, None)
            raise
        return 'combat arm_clothes -> ShowModel item=%d' % item
    except Exception as error:
        _sys_log('[sys:asset] character request failed item=%d: %s: %s' % (item, type(error).__name__, error))
        return ''



def _find_doll():
    try:
        for e in getattr(genv.space, 'entities', {}).values():
            if type(e).__name__ == 'Doll':
                return e
    except Exception:
        pass
    return None


def _apply(cfg):
    global _last_apply_seq, _last_apply_try, _apply_tries, _apply_seq_seen
    seq, typ, item = int(cfg[IDX_APPLY_SEQ]), int(cfg[IDX_APPLY_TYPE]), int(cfg[IDX_APPLY_ITEM])
    if typ not in (1, 2, 3) or not item:
        return
    if int(cfg[IDX_AUTO_THEME]):
        _last_apply_seq = seq
        return
    if seq != _apply_seq_seen:
        _apply_seq_seen, _apply_tries = seq, 0
    if seq == _last_apply_seq or time.time() - _last_apply_try < 0.25:
        return
    entity = _target_mesh_avatar()
    if entity is None:
        return
    _last_apply_try = time.time()
    try:
        via = _apply_hero(entity, item) if typ == 1 else _update_model_appearance(entity, item, typ)
        if via:
            _last_apply_seq, _apply_tries = seq, 0
            _sys_log('[sys:asset] update requested type=%d item=%d via=%s' % (typ, item, via))
        else:
            _apply_tries += 1
            if _apply_tries >= 8:
                _last_apply_seq = seq
    except Exception:
        _sys_log(traceback.format_exc())



def _enqueue_appearance_task(typ, item):
    if not item:
        return
    if typ == 1 and _asset_preserve_active:
        try:
            import gclient.data.lobby_item_data as lobby
            want = _hero_of(lobby.data.get(item))
            cur = _local_hero_id()
            if want and cur and want != cur:
                return
        except Exception:
            pass
    key = (typ, int(item))
    if key not in _appearance_task:
        _appearance_task.append(key)


def _dispatch_appearance_queue():
    global _appearance_task_t
    now = time.time()
    if not _appearance_task or now - _appearance_task_t < 0.30:
        return
    entity = _target_mesh_avatar()
    if entity is None:
        return
    _appearance_task_t = now
    typ, item = _appearance_task.pop(0)
    key = (typ, item)
    try:
        via = _apply_hero(entity, item) if typ == 1 else _update_model_appearance(entity, item, typ)
        if via:
            _appearance_fail.pop(key, None)
            _sys_log('[sys:asset] update requested type=%d item=%d via=%s' % (typ, item, via))
        else:
            count = _appearance_fail.get(key, 0) + 1
            _appearance_fail[key] = count
            if count < 6:
                _appearance_task.append(key)
            else:
                _sys_log('[sys:asset] retries stopped type=%d item=%d; see request error above' % key)
    except Exception:
        _sys_log(traceback.format_exc())



def _cached_manifest_path():
    import os
    local = os.environ.get('LOCALAPPDATA')
    if local:
        return os.path.join(local, 'Comet', 'BloodStrike', 'skin_inventory.bin')
    return os.path.join(IPC_DIR, 'skin_inventory.bin')


def _read_asset_manifest(force=False):
    import os
    now = time.time()
    cached = getattr(_read_asset_manifest, 'records', None)
    if not force and now < getattr(_read_asset_manifest, 'next_poll', 0):
        return cached
    _read_asset_manifest.next_poll = now + 1.0
    try:
        path = _cached_manifest_path()
        stat = os.stat(path)
        stamp = (stat.st_mtime_ns, stat.st_size)
        if stamp == getattr(_read_asset_manifest, 'stamp', None):
            return cached
        with open(path, 'rb') as stream:
            raw = stream.read(12 + 4096 * 64 + 1)
        if len(raw) < 12:
            raise ValueError('inventory header is incomplete')
        magic, version, count = struct.unpack_from('<III', raw)
        if magic != 0x42534956 or version != 1 or count > 4096 or len(raw) != 12 + count * 64:
            raise ValueError('inventory format or size is invalid')
        active = {}
        seen = set()
        for offset in range(12, len(raw), 64):
            typ, slot, selection, enabled, _ = struct.unpack_from('<IIII48s', raw, offset)
            if typ not in (1, 2, 3) or not slot or not selection or enabled not in (0, 1):
                raise ValueError('invalid inventory entry')
            if (typ, selection) in seen or (enabled and (typ, slot) in active):
                raise ValueError('duplicate inventory entry or active slot')
            seen.add((typ, selection))
            if enabled:
                active[(typ, slot)] = (typ, slot, selection)
        _read_asset_manifest.records = tuple(active.values())
        _read_asset_manifest.stamp = stamp
        _read_asset_manifest.error = None
    except FileNotFoundError:
        _read_asset_manifest.records = None
        _read_asset_manifest.stamp = None
        _read_asset_manifest.error = None
    except Exception as error:
        message = '%s: %s' % (type(error).__name__, error)
        if message != getattr(_read_asset_manifest, 'error', None):
            _sys_log('[sys:asset] manifest could not be loaded: ' + message)
        _read_asset_manifest.error = message
        # Keep the last valid inventory; never replace a damaged file with cfg defaults.
        if cached is None:
            _read_asset_manifest.records = ()
    return getattr(_read_asset_manifest, 'records', None)


def _resolve_manifest_targets(entity, records):
    import gclient.data.lobby_item_data as lobby
    import gclient.data.equip_data as equips
    hero = _hero_of(entity) or _local_hero_id()
    cases = [case for case in _all_cases(entity)
             if callable(getattr(case, 'ChangeWeaponGuise', None)) and
             bool(getattr(case, 'is_in_combat', False))]
    loaded_guns = {_slot_weapon_identifier(case) for case in cases if not _slot_is_melee_profile(case)}
    has_melee = any(_slot_is_melee_profile(case) for case in cases)
    targets = []
    for typ, slot, selection in records:
        try:
            if typ == 1:
                proto = lobby.data.get(selection) or {}
                if (int(proto.get('sub_type') or 0) == 300 and
                        not _item_transient_check(proto) and
                        int(proto.get('hero_id') or 0) == hero == slot):
                    targets.append((typ, selection))
                continue
            if (typ == 2 and slot not in loaded_guns) or (typ == 3 and not has_melee):
                continue
            proto, template, _ = _resolve_asset_variant(selection, typ)
            if typ == 2:
                gid = int(template.get('gun_id') or 0)
                if gid == slot and gid > 0 and any(not _slot_is_melee_profile(case) and
                        _slot_weapon_identifier(case) == gid for case in cases):
                    targets.append((typ, selection))
            else:
                if int(proto.get('sub_type') or 0) * 100 + 2 != slot:
                    continue
                expected = int(template.get('replace_equip_id') or 0)
                wanted_class = (equips.data.get(expected) or {}).get('equip_case')
                for case in cases:
                    if not _slot_is_melee_profile(case):
                        continue
                    current = getattr(case, 'equip_proto', None) or {}
                    if not expected or expected == int(getattr(case, 'weapon_id', 0) or 0) or (
                            wanted_class and wanted_class == current.get('equip_case')):
                        # Overlapping melee categories use the most recently selected compatible skin.
                        targets = [target for target in targets if target[0] != 3]
                        targets.append((typ, selection))
                        break
        except (ValueError, TypeError, KeyError):
            # A removed/outdated catalog item must not block the other saved skins.
            continue
    return tuple(targets)


def _sync_configured_appearance(cfg):
    global _asset_auto_seen, _asset_last_sync, _asset_preserve_active
    if not int(cfg[IDX_AUTO_THEME]):
        _appearance_task[:] = []
        _asset_auto_seen = None
        return
    now = time.time()
    _asset_preserve_active = _flag(cfg, F_THEME_PRESERVE)
    entity = _target_mesh_avatar()
    if entity is None:
        _asset_auto_seen = None
        _appearance_task[:] = []
        _appearance_fail.clear()
        return
    seq = int(cfg[IDX_APPLY_SEQ])
    records = _read_asset_manifest(seq != getattr(_sync_configured_appearance, 'inventory_seq', None))
    _sync_configured_appearance.inventory_seq = seq
    if records is None:
        # Compatibility for configurations saved before Inventory existed.
        want = tuple((typ, int(cfg[index])) for typ, index in
                     ((1, IDX_THEME_HERO), (2, IDX_THEME_GUN), (3, IDX_THEME_KNIFE))
                     if int(cfg[index]))
    else:
        want = _resolve_manifest_targets(entity, records)
    cases = tuple((id(case), _slot_weapon_identifier(case), int(getattr(case, 'weapon_id', 0) or 0),
                   bool(getattr(case, 'is_in_combat', False))) for case in _all_cases(entity))
    state = (want, seq, id(entity), id(getattr(genv, 'space', None)),
             _hero_of(entity) or _local_hero_id(), cases)
    if state != _asset_auto_seen or now - _asset_last_sync > 20.0:
        _asset_auto_seen = state
        _asset_last_sync = now
        _appearance_fail.clear()
        _appearance_task[:] = []
        for typ, item in want:
            _enqueue_appearance_task(typ, item)




UNLOCK_TRUE = {
    'unlock', 'unlocked', 'is_unlock', 'is_unlocked', 'is_owned', 'owned', 'is_get',
    'is_have', 'have', 'can_use', 'usable', 'is_valid', 'is_open', 'open', 'enable',
    'enabled', 'is_enable', 'is_enabled', 'is_active', 'active', 'is_obtain', 'obtained',
    'is_received', 'received', 'is_free', 'free',
}
UNLOCK_FALSE = {
    'lock', 'locked', 'is_lock', 'is_locked', 'disable', 'disabled', 'is_disable',
    'is_disabled', 'forbid', 'forbidden', 'is_forbid', 'need_unlock',
}


def _iter_data_entries():
    for name, mod in list(sys.modules.items()):
        # Yield even for skipped modules/rows, so next() cannot spend an
        # unbounded time searching before the caller can check its budget.
        yield None
        if not name.startswith('gclient.data'):
            continue
        try:
            data = getattr(mod, 'data', None)
        except Exception:
            continue
        if data is None:
            continue
        try:
            if hasattr(data, 'values'):
                vals = data.values()
            elif isinstance(data, (list, tuple)):
                vals = data
            else:
                continue
        except Exception:
            continue
        try:
            for v in vals:
                yield v if hasattr(v, 'keys') else None
        except Exception:
            pass


_entry_pending = None      # (row, remaining_keys): a row being evaluated


def _eval_cosmetic_entry(entry, budget=None):
    """Flip cosmetic flags on one data row.

    With `budget`, stops after that many seconds and resumes on the next call
    from `_entry_pending`. A single row here can hold thousands of keys, and one
    such row is what turned a 17 ms frame into a 25 ms `unlock=` spike even
    though the caller checked its budget between rows.
    """
    global _entry_pending
    changed = 0
    if _entry_pending is not None and _entry_pending[0] is not entry:
        _entry_pending = None
    try:
        keys = _entry_pending[1] if _entry_pending is not None else list(entry.keys())
    except Exception:
        _entry_pending = None
        return 0
    deadline = (time.time() + budget) if budget else 0.0
    while keys:
        k = keys.pop()
        try:
            kl = str(k).lower()
        except Exception:
            continue
        try:
            if kl in UNLOCK_TRUE:
                if not entry.get(k):
                    entry[k] = 1
                    changed += 1
            elif kl in UNLOCK_FALSE:
                if entry.get(k):
                    entry[k] = 0
                    changed += 1
        except Exception:
            pass
        if deadline and keys and time.time() > deadline:
            _entry_pending = (entry, keys)
            return changed
    _entry_pending = None
    return changed


def _sync_cosmetics_budget(budget):
    global _unlock_iter, _unlock_changed
    changed = 0
    if _unlock_iter is None:
        _unlock_iter = _iter_data_entries()
    t0 = time.time()
    while time.time() - t0 < budget:
        # Finish a row that was too big for the previous tick before pulling the
        # next one, otherwise its remaining keys are dropped on the floor.
        if _entry_pending is not None:
            changed += _eval_cosmetic_entry(_entry_pending[0], budget)
            if _entry_pending is not None:
                _unlock_changed += changed
                return changed, False
            continue
        try:
            entry = next(_unlock_iter)
        except StopIteration:
            _unlock_iter = None
            _unlock_changed += changed
            return changed, True
        except Exception:
            _unlock_iter = None
            _unlock_changed += changed
            return changed, True
        if entry is not None:
            changed += _eval_cosmetic_entry(entry, budget)
            if _entry_pending is not None:
                _unlock_changed += changed
                return changed, False
    _unlock_changed += changed
    return changed, False


def _sync_cosmetics_batch():
    changed = 0
    for entry in _iter_data_entries():
        if entry is not None:
            changed += _eval_cosmetic_entry(entry)
    return changed


_unlock_dir_cache = {}   # module name -> (id(module), [attribute names])


def _iter_cosmetic_predicates():
    # One yield per module/attribute lets the caller spread introspection across
    # ticks instead of doing the whole scan at once on the game thread.
    for name, mod in list(sys.modules.items()):
        yield None
        if not (name.startswith('gclient') or name.startswith('gshare')):
            continue
        # dir() on a game module is the expensive call here (the engine's
        # importer may decrypt a module off disk while answering it), so keep the
        # attribute list until the module object itself is replaced.
        cached = _unlock_dir_cache.get(name)
        if cached is None or cached[0] != id(mod):
            try:
                attrs = dir(mod)
            except Exception:
                continue
            if len(_unlock_dir_cache) > 4096:
                _unlock_dir_cache.clear()
            _unlock_dir_cache[name] = cached = (id(mod), attrs)
        for i, attr in enumerate(cached[1]):
            yield None
            yield None
            low = attr.lower()
            if not any(x in low for x in ('unlock', 'owned', 'is_have', 'can_use', 'obtain')):
                continue
            if not (low.startswith('is') or low.startswith('has') or low.startswith('check') or low.startswith('can')):
                continue
            key = (name, attr)
            if key in _wrapped_preds:
                continue
            try:
                fn = getattr(mod, attr, None)
            except Exception:
                continue
            if not callable(fn) or not hasattr(fn, '__code__'):
                continue
            # Only zero-argument predicates are safe to force. Anything that takes
            # parameters is real logic - `can_use(slot, item)` and friends - and
            # replacing it with a constant is exactly how a click handler ends up
            # acting on data that is not there, which the engine reports as a crash.
            if getattr(fn.__code__, 'co_argcount', 0) - getattr(fn.__code__, 'co_posonlyargcount', 0):
                continue
            if getattr(fn.__code__, 'co_kwonlyargcount', 0):
                continue

            def make(orig):
                def pred(*args, **kwargs):
                    return True
                pred._m_wrapped = True
                return pred

            try:
                setattr(mod, attr, make(fn))
                _wrapped_preds.add(key)
            except Exception:
                pass


_unlock_interval = 120.0    # seconds between passes; grows while they do nothing


def _refresh_runtime_cosmetics(lobby=True):
    global _last_unlock, _unlock_done, _unlock_iter, _unlock_pred_iter
    global _unlock_scan_t, _unlock_changed, _unlock_step_t, _unlock_interval
    global _wrapped_logged
    # Unlocking cosmetics is a lobby/menu effect, and the pass walks every game
    # module and every catalog row: the logs show whole passes taking 96 s of wall
    # time while reporting changed=0, overlapping a live match. Nothing here can
    # affect a match that has already loaded, so it does not run during one.
    if not lobby:
        return
    now = time.time()
    # The scan previously used 3 ms on every frame, enough by itself to turn
    # 250 FPS into ~140 FPS while a pass runs. Spread the same work over time;
    # visual entity gathering and projection remain at the full frame rate.
    if now - _unlock_step_t < 0.016:
        return
    _unlock_step_t = now
    if _unlock_iter is None and _unlock_pred_iter is None:
        if now - _last_unlock < _unlock_interval:
            return
        _last_unlock = now
        _unlock_scan_t = now
        _unlock_changed = 0
        _unlock_iter = _iter_data_entries()
    try:
        if _unlock_iter is not None:
            _, done = _sync_cosmetics_budget(0.0005)
            if not done:
                return
            if not _unlock_done:
                _unlock_pred_iter = _iter_cosmetic_predicates()
                return
        if _unlock_pred_iter is not None:
            deadline = time.time() + 0.001
            steps = 0
            while time.time() < deadline and steps < 8:
                steps += 1
                step_at = time.time()
                try:
                    next(_unlock_pred_iter)
                except StopIteration:
                    _unlock_pred_iter = None
                    _unlock_done = True
                    break
                if time.time() - step_at > 0.002:
                    # A single step (dir()/getattr on a lazily imported module)
                    # can cost more than the whole budget. Stop instead of
                    # walking on: this is the 25 ms `unlock=` spike seen inside a
                    # 17 ms frame.
                    break
            if _unlock_pred_iter is not None:
                return
        took = time.time() - _unlock_scan_t
        _sys_log('unlock pass done changed=%d preds=%d took=%.2fs' % (
            _unlock_changed, len(_wrapped_preds), took))
        if _wrapped_preds and not _wrapped_logged:
            _wrapped_logged = True
            names = sorted('%s.%s' % (m, a) for m, a in _wrapped_preds)
            _sys_log('[sys:sec] forced predicates: %s' % ', '.join(names[:24]))
        # Nothing to force: a pass that changes nothing is pure cost, so back off
        # instead of repeating the whole module walk every two minutes.
        if _unlock_changed:
            _unlock_interval = 120.0
        else:
            _unlock_interval = min(_unlock_interval * 2.0, 900.0)
    except Exception:
        _unlock_iter = None
        _unlock_pred_iter = None
        _sys_log(traceback.format_exc())


_killfx_family_best = None   # family_key -> {'kill_hud_id':int, 'hit_effect':int, 'trail_effect':int, 'template_id':int}
_killfx_catalog_rev = None   # (len, mtime surrogate) to detect catalog reload
_killfx_rebuild_t = 0.0      # last full family rebuild, see _killfx_scan
_killfx_log_t = 0.0
_killfx_filled = 0

def _killfx_family_key(proto):
    """Group skins so premium members can lend their effect IDs to fellow family
    members. Members of a weapon-skin family share hero_id + guise.template_id
    (model) OR are upgrade siblings (same root). Fall back to gun_id when no
    tighter grouping is available."""
    hero = proto.get('hero_id') or proto.get('hero_skin_id')
    template = int((proto.get('guise') or {}).get('template_id') or 0)
    if hero and template:
        return ('ht', int(hero), template)
    if template:
        return ('t', template)
    gun = int(proto.get('gun_id') or 0)
    if gun:
        return ('g', gun)
    return ('id', int(proto.get('id') or 0))


def _killfx_build_family_best(rows):
    """Walk the catalog once and, for each skin family, pick the top-quality
    kill_hud_id / hit_effect / trail_effect / template_id across its members."""
    best = {}
    for proto in rows.values():
        if not isinstance(proto, dict):
            continue
        sub = int(proto.get('sub_type') or 0)
        is_gun = sub in (510, 520, 530, 540, 550, 560)
        is_knife = sub in (201, 202, 203, 206, 207, 210)
        if not (is_gun or is_knife):
            continue
        q = int(proto.get('quality') or 0)
        key = _killfx_family_key(proto)
        slot = best.setdefault(key, {'quality': 0, 'kill_hud_id': 0, 'hit_effect': 0, 'trail_effect': 0, 'template_id': 0})
        template = int((proto.get('guise') or {}).get('template_id') or 0)
        for field in ('kill_hud_id', 'hit_effect', 'trail_effect'):
            v = proto.get(field)
            if v and (not slot[field] or q >= slot['quality']):
                slot[field] = int(v)
        if template and (not slot['template_id'] or q > slot['quality']):
            slot['template_id'] = template
        if q > slot['quality']:
            slot['quality'] = q
    return best


def _killfx_scan():
    """Build the family-best map, refreshed when the catalog reloads or every 60s.
    Returns (rows_dict, family_best_dict) or (None, None) on failure."""
    global _killfx_family_best, _killfx_catalog_rev
    try:
        import gclient.data.lobby_item_data as lobby
    except Exception:
        return None, None
    rows = getattr(lobby, 'data', None)
    if not isinstance(rows, dict):
        return None, None
    # `len(rows)` moves whenever the lobby catalog is added to or trimmed, and a
    # full family rebuild over every skin is tens of milliseconds on the game
    # thread. Gate it to one rebuild per 5 s; while it is skipped the previous
    # map is still used, or the caller sees None and waits a tick.
    rev = (len(rows), id(rows))
    if _killfx_family_best is None or _killfx_catalog_rev != rev:
        now = time.time()
        if now - _killfx_rebuild_t < 5.0:
            return rows, _killfx_family_best
        started = time.time()
        _killfx_family_best = _killfx_build_family_best(rows)
        _killfx_catalog_rev = rev
        _killfx_rebuild_t = now
        cost = time.time() - started
        if cost > 0.01:
            _sys_log('[sys:perf] killfx family scan %.1f ms rows=%d'
                     % (cost * 1000.0, len(rows)))
    return rows, _killfx_family_best


_killfx_patched_rev = None
_killfx_local_pass_t = 0.0
_killfx_patch_iter = None      # resumable iterator over rows.values()
_killfx_patch_filled = 0

def _killfx_patch_catalog_step(rows, best, budget=0.002):
    """Incremental catalog patch, budgeted so one frame never blocks on a huge
    rows dict. Resumes across ticks until every proto is visited, then returns
    done=True so the caller stamps the revision."""
    global _killfx_patch_iter, _killfx_patch_filled, _killfx_filled
    if _killfx_patch_iter is None:
        _killfx_patch_iter = iter(rows.values())
        _killfx_patch_filled = 0
    t0 = time.time()
    try:
        while time.time() - t0 < budget:
            proto = next(_killfx_patch_iter)
            if not isinstance(proto, dict):
                continue
            sub = int(proto.get('sub_type') or 0)
            if sub not in (510, 520, 530, 540, 550, 560, 201, 202, 203, 206, 207, 210):
                continue
            slot = best.get(_killfx_family_key(proto))
            if not slot:
                continue
            for field in ('kill_hud_id', 'hit_effect', 'trail_effect'):
                v = slot[field]
                if v and not proto.get(field):
                    proto[field] = v
                    _killfx_patch_filled += 1
    except StopIteration:
        _killfx_filled = _killfx_patch_filled
        _killfx_patch_iter = None
        return True
    return False


def _force_killfx_on_equipped():
    """Two passes: (1) incrementally backfill the catalog so every owned skin
    carries valid kill_hud_id + hit_effect + trail_effect; (2) copy the family-
    best values onto the local weapon cases so the engine picks them up. Pass 1
    is time-budgeted (2 ms/tick); pass 2 is throttled to 2 Hz."""
    global _killfx_log_t, _killfx_patched_rev, _killfx_local_pass_t
    rows, best = _killfx_scan()
    if not rows or not best:
        return

    # Pass 1: incremental catalog patch, resumed across ticks until the
    # current revision is fully walked.
    if _killfx_patched_rev != _killfx_catalog_rev:
        if _killfx_patch_catalog_step(rows, best):
            _killfx_patched_rev = _killfx_catalog_rev
            _sys_log('force_killfx catalog patched fields_filled=%d families=%d' % (_killfx_filled, len(best)))
        # Don't proceed to pass 2 until pass 1 is done — avoids compounding work on the same frame.
        return

    # Pass 2: apply onto local weapon cases (2 Hz).
    now = time.time()
    if now - _killfx_local_pass_t < 0.5:
        return
    _killfx_local_pass_t = now
    local, _av, _pos, _case = _local()
    if local is None:
        return
    cases = []
    for name in ('gun_case', 'weapon_case', 'main_case', 'cur_weapon_case'):
        wc = getattr(local, name, None)
        if wc is not None:
            cases.append(wc)
    try:
        slots = getattr(local, 'weapon_cases', None)
        if slots:
            for c in (slots.values() if hasattr(slots, 'values') else slots):
                if c is not None:
                    cases.append(c)
    except Exception:
        pass
    changed = 0
    for case in cases:
        for pname in ('equip_proto', 'body_equip_proto', 'weapon_proto', 'proto'):
            proto = getattr(case, pname, None)
            if not isinstance(proto, dict):
                continue
            slot = best.get(_killfx_family_key(proto))
            if not slot:
                continue
            for field in ('kill_hud_id', 'hit_effect', 'trail_effect'):
                v = slot[field]
                if v and proto.get(field) != v:
                    proto[field] = v
                    changed += 1
    now = time.time()
    if changed and now - _killfx_log_t > 10.0:
        _killfx_log_t = now
        _sys_log('force_killfx live changes=%d cases=%d families=%d' % (changed, len(cases), len(best)))


def _serialize_frame_data(cfg, cam, in_match, origin, lpos, items):
    global _state_seq
    fwd = (0.0, 0.0, -1.0)
    yaw = 0.0
    pitch = 0.0
    fov = 0.0
    if cam is not None:
        fwd = _vec(_cam_dir(cam)) or _vec(getattr(getattr(cam, 'Transform', None), 'z_axis', None)) or fwd
        try:
            cam2 = getattr(genv, 'camera', None)
            placer = getattr(cam2, 'placer', None)
            direc = getattr(placer, 'Direction', None)
            if direc is not None:
                yaw = float(getattr(direc, 'yaw', 0.0))
                pitch = float(getattr(direc, 'pitch', 0.0))
        except Exception:
            pass
        try:
            fov = float(getattr(cam, 'FieldOfView', 0.0) or 0.0)
        except Exception:
            pass
    sw, sh = 0.0, 0.0
    try:
        sz = gui.cocos_screen_size
        sw, sh = float(sz[0]), float(sz[1])
    except Exception:
        try:
            if MUI is not None:
                sz = MUI.GetScreenSize()
                sw, sh = float(sz[0]), float(sz[1])
        except Exception:
            pass
    _state_seq = (_state_seq + 1) & 0xFFFFFFFF
    try:
        lhid = int(_local_hero_id()) & 0xFFFF
    except Exception:
        lhid = 0
    buf = bytearray(STATE_SIZE)
    struct.pack_into(STATE_FMT, buf, 0, STATE_MAGIC, VERSION, _state_seq, 0,
                     int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF,
                     sw, sh,
                     *(lpos or (0.0, 0.0, 0.0)),
                     *(origin if origin is not None else (0.0, 0.0, 0.0)),
                     *fwd, yaw, pitch, fov,
                     1 if in_match else 0, 1 if origin is not None else 0, lhid)
    off = STATE_HEAD_SIZE
    n = 0
    if not in_match:
        _team_number(None, reset=True)
    else:
        try:
            # Seeded before any target, so the numbering always starts at the team
            # the player is on: Team 1 is yours, Team 2 is theirs.
            _team_number(_local_team_raw)
        except Exception:
            pass
    if in_match and items:
        do_vis = _flag(cfg, F_RENDER_OCCLUSION) or _flag(cfg, F_TRACK_OCCLUSION)
        vis_budget = 64
        max_dist = float(cfg[IDX_RENDER_MAX_DIST])
        for item in items:
            dist, e, eid, ht, feet, ph, pf, ps, pn, gun, eteam, is_team = item[:12]
            is_dying = item[12] if len(item) > 12 else _is_dying(e)
            if dist > max_dist:
                continue
            if is_team and not _flag(cfg, F_RENDER_FACTION):
                continue
            vis = 1
            if do_vis:
                fresh = _query_occlusion_cache(e, 0.12)
                if fresh is not None:
                    vis = 1 if fresh else 0
                elif vis_budget > 0:
                    vis_budget -= 1
                    vis = 1 if _line_of_sight_query(e, ht, dist, max_dist) else 0
                else:
                    stale = _query_occlusion_cache(e)
                    if stale is not None:
                        vis = 1 if stale else 0
            eid_i = _id_u64(eid)
            try:
                team_i = _team_number(eteam)
            except Exception:
                team_i = 0
            try:
                hero_i = int(_hero_of(e) or 0) & 0xFFFF
            except Exception:
                hero_i = 0
            try:
                bones_data = ps if (ps and len(ps) == 48) else ([-1.0] * 48)
                struct.pack_into(ENT_FMT, buf, off,
                                 eid_i, _bytes(_ename(e), 64), _bytes(gun, 32),
                                 ht[0], ht[1], ht[2], feet[0], feet[1], feet[2],
                                 ph[0], ph[1], pf[0], pf[1],
                                 float(dist), _hp(e), _max_hp(e), _armor(e), _max_armor(e), team_i,
                                 vis, 1 if is_team else 0, 1 if getattr(e, 'IsMonster', False) else 0, 1,
                                 1 if is_dying else 0,
                                 *bones_data,
                                 hero_i, 1 if is_dying else 0)
            except Exception:
                continue
            off += ENT_SIZE
            n += 1
            if n >= MAX_ENTITIES:
                break
    struct.pack_into('<I', buf, 12, n)
    # Trailer header. Ground-loot publishing was removed, so the count is always
    # zero; it stays in the packet so a reader can rely on the header instead of
    # guessing the packet length.
    off = STATE_HEAD_SIZE + n * ENT_SIZE
    # No ground-loot publishing any more (removed at the user's request). The
    # count field stays so the overlay's reader keeps its expected layout.
    loot_count = 0
    struct.pack_into('<I', buf, off, loot_count)
    return buf[:off + LOOT_HEAD]


def _on_story_tick(dt):
    global _last_state, _fire_release_at, _last_catalog_check, _track_busy
    global _last_gather, _gather_cache, _last_hooks, _last_scrub, _diag_t
    global _perf_t, _perf_last
    global _perf_tick_window, _perf_tick_count, _perf_tick_sum, _perf_tick_max, _perf_tick_dt
    global _perf_phase_sum, _appearance_sync_t, _damping_t, _perf_items, _unlock_t
    global _perf_bone_sum
    if int(getattr(builtins, '_m_generation', 0) or 0) != _my_gen:
        return
    try:
        t0 = time.time()
        cfg = _read_cfg()
        if cfg[IDX_MAGIC] != CFG_MAGIC:
            cfg = DEFAULT_CFG
        now = t0
        if dt > 0.4 and now - _perf_t >= 1.0:
            _perf_t = now
            _sys_log('perf stutter dtime=%.3f' % (dt,))
        if now - _last_hooks >= 2.0:
            _last_hooks = now
            _install_instance_hooks()

        if now - _last_catalog_check >= 5.0:
            _last_catalog_check = now
            _save_template_cache()
        if _fire_release_at and now >= _fire_release_at:
            _trigger_primary_input(False)
            _mouse(False)
            _fire_release_at = 0.0
        menu_open = _flag(cfg, F_MENU_OPEN)
        hooks_off = _no_hooks_requested()
        in_match = not bool(getattr(genv, 'is_in_hall', True))
        _sync_ray_interceptor(cfg, in_match)
        _apply(cfg)
        # Skin/theme reconciliation walks every equipped case and reads the hero
        # and the manifest; at 100 Hz in a match it measured 0.89 ms of the frame
        # in `apply=` for a change nobody can see. Five times a second is enough
        # for an icon that only changes when the player changes something.
        if now - _appearance_sync_t >= 0.2:
            _appearance_sync_t = now
            _sync_configured_appearance(cfg)
            _dispatch_appearance_queue()
        t_apply = time.time()
        # Twice a second, not sixty times. This pair is a catalogue walk plus
        # effect application; measured at 0.45-0.5 ms per frame in the lobby and as
        # a 68 ms outlier when it rebuilt (`max_ms=70.338` in [sys:tick]). Nothing
        # it changes is visible before the next half second anyway.
        if now - _unlock_t >= 0.5:
            _unlock_t = now
            if _flag(cfg, F_UNLOCK_EFFECTS) and not hooks_off:
                _refresh_runtime_cosmetics(not in_match)
            if _flag(cfg, F_FORCE_KILLFX) and not hooks_off:
                _force_killfx_on_equipped()
        t_unlock = time.time()
        cam = None
        origin = None
        items = []
        track_active = _flag(cfg, F_TRACK_VIEW)
        render_on = _flag(cfg, F_RENDER_ENTITIES)
        if in_match and (track_active or render_on):
            scan = max(float(cfg[IDX_RENDER_MAX_DIST]), float(cfg[IDX_TRACK_MAX_DIST]))
            # At most 60 gathers a second. The gather is the most expensive thing
            # the runtime does - 2.7 ms per call in a fight even after the offscreen
            # fix - and it was being paid on every frame. The ESP is a 60 Hz overlay
            # in effect: at 100 fps this halves what it costs per frame, and above
            # that it drops further. Positions 16 ms old are what a 60 fps ESP always
            # looked like, and the aim path predicts ahead regardless.
            interval = 1.0 / 60.0
            want_gun = render_on and _flag(cfg, F_RENDER_EQUIPMENT)
            want_bone = render_on and _flag(cfg, F_RENDER_SKELETON)
            if now - _last_gather >= interval or _gather_cache[1] is None:
                cam, origin, items = _query_visible_entities(scan, want_gun, want_bone)
                _gather_cache = (now, cam, origin, items)
                _last_gather = now
            else:
                _, cam, origin, items = _gather_cache
            if render_on and now - _diag_t >= 5.0:
                _diag_t = now
                try:
                    total = len(_collect_space_entities(getattr(genv, 'space', None)))
                except Exception:
                    total = -1
                try:
                    cats = getattr(getattr(genv, 'space', None), 'entity_category', None) or {}
                    brief = ','.join('%s=%d' % (k, len(v)) for k, v in list(cats.items())[:12])
                except Exception:
                    brief = ''
                _sys_log('diag match=%s hall=%s ents=%d items=%d track=%d render=%d cats[%s]' %
                     (in_match, getattr(genv, 'is_in_hall', None), total, len(items),
                      1 if track_active else 0, 1 if render_on else 0, brief))
                for it in items[:4]:
                    try:
                        _sys_log('diag ent type=%s id=%s dist=%.1f team=%s is_team=%s scr=(%.3f,%.3f)' %
                             (type(it[1]).__name__, it[2], it[0], it[10], it[11],
                              it[5][0], it[5][1]))
                    except Exception:
                        pass
        else:
            try:
                cam = _get_cam()
                origin = _vec(cam.GetOrigin()) if cam is not None else None
            except Exception:
                cam = None
                origin = None
            _gather_cache = (now, cam, origin, [])
        # Every tick, exactly as before. This is the aim path, not the ESP: it
        # publishes `_ray_target` and holds `_track_busy` up for the silent-aim
        # redirect, and it integrates the view turn with `dt`. Throttling it made
        # `_track_busy` drop to False on the skipped ticks, so `_query_corrected_ray`
        # returned the unmodified ray on roughly half of them and silent aim
        # stopped landing. The gather is safely rate-limited because stale item
        # positions are predicted forward; this is not.
        if items and track_active:
            t_gather = time.time()
            _update_cam_trajectory(cfg, items, cam, origin, menu_open, dt)
        else:
            t_gather = time.time()
            _track_busy = False
        t_track = time.time()
        # These walk engine state every call and only matter at the rate the ESP is
        # updated at; 60 Hz is that rate.
        if now - _damping_t >= 1.0 / 60.0:
            _damping_t = now
            _sync_impulse_damping(cfg, in_match)
            _sync_weapon_case_damping(cfg, in_match)
        t_sync = time.time()
        _last_state = now
        av, lid, team, lpos = _local()
        t_local = time.time()
        # The per-tick ground-loot scan is gone (it walked the whole space and
        # projected every candidate); nothing calls it any more.
        buf = _serialize_frame_data(cfg, cam, in_match, origin, lpos, items)
        t_serial = time.time()
        if buf is not None:
            _write(STATE_PATH, buf)
        t1 = time.time()
        total = t1 - t0
        # Rolling per-tick cost. `frame_ms` is how often the engine calls us, so
        # `avg_ms / frame_ms` is the share of the frame the runtime takes - the
        # number that decides whether a low frame rate can be ours at all.
        _perf_tick_count += 1
        _perf_tick_sum += total
        _perf_phase_sum[0] += t_apply - t0
        _perf_phase_sum[1] += t_unlock - t_apply
        _perf_phase_sum[2] += t_gather - t_unlock
        _perf_phase_sum[3] += t_track - t_gather
        _perf_phase_sum[4] += t1 - t_track
        _perf_bone_sum += _g_bone_t
        _perf_items = len(items)
        _perf_tick_dt += dt
        if total > _perf_tick_max:
            _perf_tick_max = total
        if _perf_tick_window and t0 - _perf_tick_window >= 10.0:
            ticks = _perf_tick_count or 1
            _sys_log('[sys:tick] n=%d avg_ms=%.3f max_ms=%.3f frame_ms=%.2f in_match=%d '
                     'apply=%.3f unlock=%.3f gather=%.3f track=%.3f rest=%.3f bone=%.3f items=%d'
                     % (ticks, 1000.0 * _perf_tick_sum / ticks, 1000.0 * _perf_tick_max,
                        1000.0 * _perf_tick_dt / ticks, 1 if in_match else 0,
                        1000.0 * _perf_phase_sum[0] / ticks, 1000.0 * _perf_phase_sum[1] / ticks,
                        1000.0 * _perf_phase_sum[2] / ticks, 1000.0 * _perf_phase_sum[3] / ticks,
                        1000.0 * _perf_phase_sum[4] / ticks,
                        1000.0 * _perf_bone_sum / ticks,
                        _perf_items), force=True)
            _perf_tick_window = t0
            _perf_tick_count = 0
            _perf_tick_sum = 0.0
            for _i in range(5):
                _perf_phase_sum[_i] = 0.0
            _perf_bone_sum = 0.0
            _perf_tick_max = 0.0
            _perf_tick_dt = 0.0
        elif not _perf_tick_window:
            _perf_tick_window = t0
        if (total > 0.06 or t_track - t_gather > 0.04) and t0 - _perf_last >= 1.0:
            _perf_last = t0
            _sys_log('[sys:perf] dt=%.3f total=%.3f apply=%.4f unlock=%.4f gather=%.4f track=%.4f state=%.4f sync=%.4f local=%.4f serialize=%.4f write=%.4f items=%d' % (
                dt, total, t_apply - t0, t_unlock - t_apply, t_gather - t_unlock,
                t_track - t_gather, t1 - t_track, t_sync - t_track,
                t_local - t_sync, t_serial - t_local, t1 - t_serial, len(items)))
    except Exception:
        _sys_log(traceback.format_exc())


_on_story_tick.__name__ = 'OnEngineViewportUpdate'
_on_story_tick.__qualname__ = 'OnEngineViewportUpdate'


def _install_fault_recorder():
    """Record fatal Python faults that would otherwise vanish.

    The game installs its own excepthook and, in places, its own threading
    hooks; when a hook of ours raises where the engine swallows it, the only
    symptom is that the tick stops. Write a marker so a crash report can tell
    "our runtime raised" apart from "the process died in native code".
    """
    marker = os.path.join(IPC_DIR, 'runtime_fatal.txt')
    try:
        def hook(exc_type, exc, tb):
            try:
                with open(marker, 'a', encoding='utf-8', errors='replace') as f:
                    f.write('%s FATAL %s\n' % (time.strftime('%H:%M:%S'), exc_type.__name__))
                    f.write(''.join(traceback.format_exception(exc_type, exc, tb)))
            except Exception:
                pass
            return _orig_excepthook(exc_type, exc, tb) if _orig_excepthook else None
        sys.excepthook = hook
    except Exception:
        pass


def start():
    global _last_apply_seq, _booted, _my_gen
    _my_gen = int(getattr(builtins, '_m_generation', 0) or 0)
    if _booted:
        return True
    os.makedirs(IPC_DIR, exist_ok=True)
    _make_cfg_default()
    _save_template_cache()
    _install_hooks()
    _install_fault_recorder()
    try:
        import gclient.framework.util.story_tick as st
        def OnEngineViewportUpdate(dt):
            return _on_story_tick(dt)
        OnEngineViewportUpdate.__name__ = 'OnEngineViewportUpdate'
        OnEngineViewportUpdate.__qualname__ = 'OnEngineViewportUpdate'
        st.StoryTick().Add(OnEngineViewportUpdate, 0)
    except Exception:
        _sys_log(traceback.format_exc())
        return False
    _booted = True
    _last_apply_seq = -1
    try:
        cfg = _read_cfg()
        cam, origin, items = _query_visible_entities(float(cfg[IDX_RENDER_MAX_DIST]))
        av, lid, team, lpos = _local()
        _write(STATE_PATH, _serialize_frame_data(cfg, cam, not bool(getattr(genv, 'is_in_hall', True)), origin, lpos, items))
    except Exception:
        _sys_log(traceback.format_exc())
    return True


def status():
    cfg = _read_cfg()
    try:
        catsize = os.path.getsize(CATALOG_PATH)
    except Exception:
        catsize = 0
    try:
        statesize = os.path.getsize(STATE_PATH)
    except Exception:
        statesize = 0
    try:
        head = _read(CATALOG_PATH, CAT_HEAD_SIZE)
        hc, gc = struct.unpack(CAT_HEAD_FMT, head)[2:4] if head and len(head) == CAT_HEAD_SIZE else (0, 0)
    except Exception:
        hc, gc = 0, 0
    return {
        'cfg_magic': hex(cfg[IDX_MAGIC]),
        'flags': hex(int(cfg[IDX_FLAGS])),
        'render': _flag(cfg, F_RENDER_ENTITIES),
        'track': _flag(cfg, F_TRACK_VIEW),
        'flick': False,
        'menu': _flag(cfg, F_MENU_OPEN),
        'catalog': catsize,
        'heroes': hc,
        'guns': gc,
        'knife_gids': sum(1 for k, v in _melee_cache.items() if v),
        'state': statesize,
        'in_hall': bool(getattr(genv, 'is_in_hall', True)),
        'hooks': _hooks_done,
        'hero_id': _local_hero_id(),
        'unlock': _flag(cfg, F_UNLOCK_EFFECTS),
        'block_report': _flag(cfg, F_BLOCK_REPORT),
    }


def rebuild_catalog():
    _save_template_cache(True)
    return True

# Boot marker. If this line is missing from runtime_err.txt, the module did not
# finish importing and no feature (ESP, skins, aim) can work - the menu is drawn by
# the native side and keeps working, which is exactly the "menu but no features"
# state.
try:
    _sys_log('[sys:boot] runtime module loaded', force=True)
except Exception:
    pass
