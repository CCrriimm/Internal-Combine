"""Offline tests for the humanized multi-node direction path."""
import ast
import math
from pathlib import Path
import types
import unittest


class HumanPathTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        names = {'_begin_path','_path_sample','_path_advance','_filter_view_direction','_slerp_dir','_flag'}
        nodes = [n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
                 if isinstance(n,ast.FunctionDef) and n.name in names]
        self.assertEqual({n.name for n in nodes}, names)
        self.now=1.0
        self.entity=object()
        self.ns={'math':math,'time':types.SimpleNamespace(monotonic=lambda:self.now),'_path':None,
                 'genv':types.SimpleNamespace(space=object()),'IDX_FLAGS':3,'F_TRACK_HUMANIZE':1<<17,'IDX_HUMAN_SMOOTH':13,
                 '_norm':lambda v:tuple(x/(math.sqrt(sum(y*y for y in v)) or 1) for x in v),
                 '_dot':lambda a,b:sum(x*y for x,y in zip(a,b)),
                 '_sub':lambda a,b:tuple(x-y for x,y in zip(a,b)),
                 '_cross':lambda a,b:(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])}
        exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),self.ns)
        self.cfg=[0]*40;self.cfg[3]=1<<17;self.cfg[13]=.5
        self.start=(0,0,1);self.end=(math.sin(.2),0,math.cos(.2))

    def step(self, target=None, current=None, dt=1/60):
        self.now+=dt
        return self.ns['_filter_view_direction'](self.entity,current or self.start,target or self.end,self.cfg,dt)

    def test_path_has_multiple_normalized_nodes_and_exact_endpoints(self):
        self.step();p=self.ns['_path']
        self.assertGreaterEqual(len(p['nodes']),3);self.assertLessEqual(len(p['nodes']),8)
        self.assertEqual(p['nodes'][0],self.start);self.assertEqual(p['nodes'][-1],self.end)
        for n in p['nodes']:self.assertAlmostEqual(sum(x*x for x in n),1)

    def test_path_progresses_without_instant_snap_and_finishes(self):
        first=self.step();self.assertGreater(first[0],0);self.assertLess(first[0],self.end[0])
        for _ in range(30):last=self.step()
        self.assertEqual(last,self.end)

    def test_completed_path_updates_live_endpoint_without_restarting(self):
        for _ in range(20):self.step()
        p=self.ns['_path'];new=(math.sin(.21),0,math.cos(.21))
        result=self.step(new)
        self.assertIs(self.ns['_path'],p);self.assertEqual(result,new)

    def test_target_switch_restarts_from_current_camera(self):
        self.step();old=self.ns['_path'];self.entity=object()
        current=(math.sin(.1),0,math.cos(.1));self.step(current=current)
        self.assertIsNot(self.ns['_path'],old);self.assertEqual(self.ns['_path']['start'],current)

    def test_humanize_off_clears_path_and_returns_direct_target(self):
        self.step();self.cfg[3]=0
        self.assertEqual(self.step(),self.end);self.assertIsNone(self.ns['_path'])

    def test_smoothing_changes_path_duration(self):
        self.cfg[13]=.1;self.step();short=self.ns['_path']['dur']
        self.ns['_path']=None;self.cfg[13]=.9;self.step()
        self.assertGreater(self.ns['_path']['dur'],short)

    def test_legacy_value_and_stutter_are_safe(self):
        self.cfg[13]=15;result=self.step(dt=.5)
        self.assertTrue(all(math.isfinite(x) for x in result))
        self.assertAlmostEqual(self.ns['_path']['dur'],.14)

    def test_catmull_rom_is_continuous_at_nodes(self):
        self.step();p=self.ns['_path'];sample=self.ns['_path_sample']
        for i in range(1,len(p['nodes'])-1):
            u=i/(len(p['nodes'])-1)
            before,after=sample(p,u-1e-6),sample(p,u+1e-6)
            self.assertLess(sum((a-b)**2 for a,b in zip(before,after)),1e-8)

    def test_space_change_restarts_motion(self):
        self.step();old=self.ns['_path'];self.ns['genv'].space=object();self.step()
        self.assertIsNot(self.ns['_path'],old)


if __name__=='__main__':unittest.main()
