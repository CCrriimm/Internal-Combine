"""Per-point raycast regression: the head result cannot be reused for neck/body."""
import ast,math,sys,time,types,unittest
from pathlib import Path
from unittest.mock import patch

class PointVisibilityTests(unittest.TestCase):
    def test_raycasts_requested_points_and_separates_cache(self):
        source=Path(__file__).resolve().parents[1]/'pyexec/bs_runtime.py'
        node=next(n for n in ast.parse(source.read_text(encoding='utf-8')).body
                  if isinstance(n,ast.FunctionDef) and n.name=='_line_of_sight_query')
        rays=[]
        head,neck=(0,1.8,100),(0,1.5,100)
        def raycast(origin,distance,mask,flag,to_pos):
            rays.append(to_pos)
            return types.SimpleNamespace(IsHit=True,Body=types.SimpleNamespace(ownerid=999 if to_pos==head else 2))
        ns={'math':math,'time':time,'_vis_cache':{},'_vec':lambda p:tuple(p),'_v3':lambda p:p,
            '_get_cam':lambda:types.SimpleNamespace(GetOrigin=lambda:(0,0,0)),
            'genv':types.SimpleNamespace(space=types.SimpleNamespace(RawRaycast=raycast))}
        exec(compile(ast.Module(body=[node],type_ignores=[]),str(source),'exec'),ns)
        package=types.ModuleType('gclient');package.cconst=types.SimpleNamespace(PHYSICS_SHOOT_TEST=1)
        entity=types.SimpleNamespace(id=2)
        with patch.dict(sys.modules,{'gclient':package}):
            self.assertFalse(ns['_line_of_sight_query'](entity,head,100,400))
            self.assertTrue(ns['_line_of_sight_query'](entity,neck,100,400))
            self.assertTrue(ns['_line_of_sight_query'](entity,neck,100,400))
        self.assertEqual(rays,[head,neck])

if __name__=='__main__':unittest.main()
