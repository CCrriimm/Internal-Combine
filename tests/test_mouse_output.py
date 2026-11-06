"""Aimbot mouse output: SendInput is the only injection path.

The runtime's ctypes imports are stubbed with a fake `ctypes.windll.user32` so
the exact call the game process would make is observable without touching the
real cursor or requiring Windows.
"""
import ast
import math
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
TEXT = SOURCE.read_text('utf-8-sig')

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def functions(names, namespace):
    nodes = [node for node in ast.parse(TEXT).body
             if isinstance(node, ast.FunctionDef) and node.name in names]
    if {node.name for node in nodes} != names:
        raise AssertionError('Missing runtime function')
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace


class _StructBase:
    """Stand-in for ctypes.Structure / ctypes.Union.

    The runtime subclasses these (`class MOUSEINPUT(ctypes.Structure)`), so this
    is a real class; instances record the scalar fields the driver writes.
    """

    _fields_ = []

    def __init__(self, *args, **kwargs):
        object.__setattr__(self, 'fields', {})

    def __setattr__(self, name, value):
        if name in ('fields', 'type'):
            object.__setattr__(self, name, value)
            return
        if name in ('dx', 'dy', 'dwFlags', 'mouseData'):
            self.fields[name] = value
            return
        object.__setattr__(self, name, value)

    @property
    def u(self):
        return self

    @property
    def mi(self):
        return self


class _Wintypes(types.ModuleType):
    LONG = 'long'
    DWORD = 'dword'
    ULONG = 'ulong'

    def __init__(self):
        super().__init__('ctypes.wintypes')

    @staticmethod
    def HANDLE():
        return 'handle'

    @staticmethod
    def BOOL():
        return 'bool'

    @staticmethod
    def LPCSTR():
        return 'lpcstr'

    @staticmethod
    def LPVOID():
        return 'lpvoid'


class FakeCtypes(types.ModuleType):
    """Minimal ctypes surface: Structure/Union, byref, sizeof, windll."""

    class ArgumentError(Exception):
        pass

    def __init__(self):
        super().__init__('ctypes')
        self.calls = []
        self.mouse_events = []
        self.sent = None
        outer = self

        class _User32:
            @staticmethod
            def SendInput(count, pointer, size):
                outer.calls.append((count, size))
                outer.sent = pointer()
                return count

            @staticmethod
            def mouse_event(*args):
                outer.mouse_events.append(args)

        self.windll = types.SimpleNamespace(user32=_User32())
        self.wintypes = _Wintypes()
        # Real class objects: the runtime subclasses Structure/Union, so these
        # must be usable in a class statement.
        self.Structure = _StructBase
        self.Union = _StructBase
        self.POINTER = lambda *args, **kwargs: 'pointer'
        self.byref = lambda obj: (lambda: obj)
        self.sizeof = lambda obj: 40
        # `from ctypes import wintypes` consults sys.modules first; expose the
        # same object under both names.
        self.wintypes.__name__ = 'ctypes.wintypes'


class MouseOutputTests(unittest.TestCase):
    def setUp(self):
        self.ctypes = FakeCtypes()
        patcher = patch.dict(sys.modules, {'ctypes': self.ctypes,
                                           'ctypes.wintypes': self.ctypes.wintypes})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.ns = {
            'math': math,
            'ctypes': self.ctypes,
            '_MOUSE_INPUT': None,
            '_MOUSEEVENTF_LEFTDOWN': MOUSEEVENTF_LEFTDOWN,
            '_MOUSEEVENTF_LEFTUP': MOUSEEVENTF_LEFTUP,
        }
        functions({'_mouse_input_struct', '_send_mouse', '_mouse'}, self.ns)

    def test_click_uses_sendinput_and_never_mouse_event(self):
        self.assertTrue(self.ns['_mouse'](True))
        self.assertEqual(self.ctypes.sent.fields['dwFlags'], MOUSEEVENTF_LEFTDOWN)
        self.assertTrue(self.ns['_mouse'](False))
        self.assertEqual(self.ctypes.sent.fields['dwFlags'], MOUSEEVENTF_LEFTUP)
        self.assertEqual(self.ctypes.mouse_events, [])       # legacy path is gone
        self.assertEqual(len(self.ctypes.calls), 2)

    def test_legacy_mouse_event_is_not_referenced_anywhere(self):
        # Not just unused: the old API must not be reachable from the runtime.
        # Comments may name it (they explain its removal); no code may call it.
        self.assertNotIn('windll.user32.mouse_event', TEXT)
        self.assertNotIn('.mouse_event(', TEXT)

    def test_only_send_input_touches_the_mouse(self):
        # Every mouse injection goes through the one SendInput helper.
        self.assertEqual(TEXT.count('SendInput('), TEXT.count('def _send_mouse'),
                         'SendInput must be called from exactly one place')

    def test_failed_send_is_reported_not_raised(self):
        def boom(*args):
            raise OSError('no input desktop')
        with patch.object(self.ctypes.windll.user32, 'SendInput', boom):
            self.assertFalse(self.ns['_mouse'](True))

    def test_nan_delta_is_rejected(self):
        self.assertFalse(self.ns['_send_mouse'](0x0001, float('nan'), 1))
        self.assertEqual(self.ctypes.calls, [])


class NoiseScopeTests(unittest.TestCase):
    """Input noise is aimbot-only: everything else keeps amplitude 0."""

    def namespace(self, noise_enabled):
        ns = {'math': math, '_jit_t': 0.0, '_NOISE_OFF': (0.0, 0.0)}
        functions({'_micro_jitter'}, ns)
        return ns

    def test_zero_or_negative_amplitude_is_exactly_off(self):
        ns = self.namespace(False)
        jitter = ns['_micro_jitter']
        self.assertEqual(jitter(1 / 60, 0.0), (0.0, 0.0))
        self.assertEqual(jitter(1 / 60, -1.0), (0.0, 0.0))
        self.assertEqual(jitter(1 / 60, float('nan')), (0.0, 0.0))

    def test_amplitude_scales_the_offset(self):
        ns = self.namespace(True)
        jitter = ns['_micro_jitter']
        ns['_jit_t'] = 0.0
        small = jitter(1 / 60, 0.001)
        ns['_jit_t'] = 0.0
        large = jitter(1 / 60, 0.004)
        self.assertGreater(abs(large[0]), abs(small[0]))
        # Bounded: the configured maximum never turns into a visible flick.
        self.assertLess(max(abs(large[0]), abs(large[1])), 0.008 * 1.5)

    def test_history_default_amplitude_is_kept(self):
        # Callers that do not pass an amplitude (view tracking) keep the old
        # 0.0016 default, so the tracker's output is bit-for-bit unchanged.
        ns = self.namespace(True)
        ns['_jit_t'] = 0.0
        default_call = ns['_micro_jitter'](1 / 60)
        ns['_jit_t'] = 0.0
        explicit = ns['_micro_jitter'](1 / 60, 0.0016)
        self.assertAlmostEqual(default_call[0], explicit[0], places=12)
        self.assertAlmostEqual(default_call[1], explicit[1], places=12)

    def test_apply_view_transform_noise_defaults_to_off(self):
        signature = next(node for node in ast.parse(TEXT).body
                         if isinstance(node, ast.FunctionDef)
                         and node.name == '_apply_view_transform')
        default = signature.args.defaults[-1]
        self.assertIsInstance(default, ast.Constant)
        self.assertEqual(default.value, 0.0)

    def test_only_the_aim_path_supplies_noise(self):
        """One call site passes an amplitude; the rest inherit the 0 default."""
        passing = []
        for node in ast.walk(ast.parse(TEXT)):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if isinstance(func, ast.Name) and func.id == '_apply_view_transform':
                if len(node.args) >= 6 or any(k.arg == 'noise' for k in node.keywords):
                    passing.append(node.lineno)
        self.assertEqual(len(passing), 1, passing)


if __name__ == '__main__':
    unittest.main()
