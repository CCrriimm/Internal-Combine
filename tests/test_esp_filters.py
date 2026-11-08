"""Offline ESP filtering checks; extract functions without starting the game runtime."""
import ast
import math
import os
from pathlib import Path
import time
import types
import unittest


class EspFilterTests(unittest.TestCase):
    def setUp(self):
        source = Path(os.environ.get('ESP_RUNTIME_SOURCE', Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'))
        names = {'_call_attr', '_entity_flag', '_normalize_team_id', '_team_id', '_is_vehicle', '_combat_type',
                 '_is_combat', '_is_skill_object', '_ai_kind', '_check_faction_relation', '_local', '_max_hp',
                 '_collect_space_entities', '_query_visible_entities', '_sample_target_motion', '_target_joint_indices', '_pick', '_pick_eid', '_options', '_flag'}
        tree = ast.parse(source.read_text(encoding='utf-8-sig'))
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        self.assertEqual({n.name for n in nodes}, names)
        self.local = self.entity(1, '7')
        self.space = types.SimpleNamespace(entities={}, game_logic=types.SimpleNamespace())
        self.genv = types.SimpleNamespace(player=self.local, avatar=self.local, space=self.space, is_in_hall=False)
        self.ns = {'time': time, 'genv': self.genv, '_local_cache': None, '_ent_list_cache': None,
                   '_vec': lambda p: tuple(p) if p is not None else None, '_v3': lambda p: p,
                   '_get_cam': lambda: types.SimpleNamespace(GetOrigin=lambda: (0, 0, 0)),
                   '_cam_dir': lambda _: (0, 0, 1), '_sub': lambda a, b: tuple(x-y for x, y in zip(a, b)),
                   '_dot': lambda a, b: sum(x*y for x, y in zip(a, b)), '_len': lambda v: math.sqrt(sum(x*x for x in v)),
                   '_alive': lambda e: True, '_is_dying': lambda e: getattr(e, 'knocked', False),
                   'math': math, '_norm': lambda v: tuple(x / math.sqrt(sum(y*y for y in v)) for x in v),
                   'IDX_FLAGS': 3, 'IDX_OPTIONS': 17, 'IDX_TRACK_FOV': 10, 'OPT_TRACK_IGNORE_DOWNED': 2,
                   'OPT_TRACK_JOINTS_SET': 4, 'OPT_TRACK_HEAD': 256, 'OPT_TRACK_NECK': 512, 'OPT_TRACK_BODY': 1024,
                   'F_TRACK_MULTIPOINT': 1 << 24, 'F_TRACK_FACTION': 1 << 15, 'F_TRACK_OCCLUSION': 1 << 14,
                   '_line_of_sight_query': lambda *args: True,
                   '_sample_joint_transform': lambda *args: None,
                   '_matrix_world_to_screen': lambda p: (.5, .5), '_gun_name': lambda e: '',
                   '_g_log_t': 0, '_g_slow': 0, '_sys_log': lambda _: None, 'MAX_ENTITIES': 256,
                   '_combat_type_cache': {},
                   # The per-entity verdict cache: module-level, so a gather that
                   # runs second must see the same dict, not a fresh one.
                   '_combat_cache': None, '_combat_cache_space': None,
                   'COMBAT_CACHE_TTL': 0.5, '_faction_cache': None,
                   'FACTION_CACHE_TTL': 1.0}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), self.ns)

    def entity(self, eid=2, team='8', **extra):
        return types.SimpleNamespace(id=eid, combat_team_id=team, position=(0, 0, 10),
                                     IsPlayerCombatAvatar=True, **extra)

    def relation(self, target, bot=False):
        return self.ns['_check_faction_relation'](target, self.local, self.ns['_team_id'](self.local),
                                                 self.ns['_team_id'](target), bot, False)

    def gather(self, *entities):
        self.space.entities = {e.id: e for e in entities}
        self.ns['_ent_list_cache'] = None
        self.ns['_combat_cache'] = None
        self.ns['_faction_cache'] = None
        return self.ns['_query_visible_entities'](400, False, False)[2]

    def test_skill_objects_are_excluded_even_with_avatar_flags(self):
        for marker in ('IsMagicField', 'IsAttackDrone', 'IsHolographicRobot', 'IsSkillEntity',
                       'IsSkillObject', 'IsProjectile', 'IsBullet', 'IsTrap', 'IsTurret', 'IsSummon', 'IsDecoy'):
            with self.subTest(marker=marker):
                skill = self.entity(hp=100, **{marker: True})
                self.assertFalse(self.ns['_is_combat'](skill))
                self.assertEqual(self.gather(skill), [])

    def test_generic_combat_units_and_monsters_are_not_player_esp(self):
        for flag in ('IsCombatAvatar', 'IsMonster', 'IsSimpleCombatUnit'):
            self.assertFalse(self.ns['_is_combat'](types.SimpleNamespace(**{flag: True})))

    def test_remote_combat_avatar_variants_are_gathered(self):
        for name in ('CombatAvatarSquadFight', 'CombatAvatarBattleRoyal'):
            with self.subTest(name=name):
                base = type('CombatAvatar', (), {})
                remote = type(name, (base,), {})()
                remote.__dict__.update(id=2, combat_team_id='8', position=(0, 0, 10),
                                       IsCombatAvatar=True, IsPlayerCombatAvatar=False)
                self.assertEqual(len(self.gather(remote)), 1)
                remote.IsMagicField = True
                self.assertEqual(self.gather(remote), [])

    def test_current_max_hp_overrides_template_hp(self):
        self.assertEqual(self.ns['_max_hp'](types.SimpleNamespace(cur_maxhp=225, maxhp=125)), 225)
        self.assertEqual(self.ns['_max_hp'](types.SimpleNamespace(maxhp=125)), 125)
        self.assertEqual(self.ns['_max_hp'](types.SimpleNamespace(cur_maxhp=None, max_hp=150)), 150)

    def test_remote_avatar_uses_team_identity(self):
        remote = type('CombatAvatarSquadFight', (), {})()
        remote.__dict__.update(id=2, combat_team_id='7', position=(0, 0, 10), IsCombatAvatar=True)
        self.assertTrue(self.gather(remote)[0][11])
        remote.combat_team_id = '8'
        self.assertFalse(self.gather(remote)[0][11])

    def test_combat_avatar_class_without_marker_is_not_accepted(self):
        remote = type('CombatAvatarSquadFight', (), {'IsCombatAvatar': False})()
        self.assertFalse(self.ns['_is_combat'](remote))

    def test_active_combat_player_takes_precedence_over_stale_avatar(self):
        self.local.IsCombatAvatar = True
        self.genv.avatar = self.entity(99, 'old-team')
        av, eid, team, pos = self.ns['_local']()
        self.assertIs(av, self.local)
        self.assertEqual((eid, team), (1, '7'))

    def test_player_buffs_and_skill_fields_do_not_hide_player(self):
        player = self.entity(skill_id=123, active_skill_id=456, shield=100, magic_field_id=789,
                             IsSkillEntity=lambda: False)
        self.assertTrue(self.ns['_is_combat'](player))
        self.assertEqual(len(self.gather(player)), 1)

    def test_concrete_training_bots_and_guards_still_qualify(self):
        for flag in ('IsRobotCombatAvatar', 'IsRobotCombatAvatarZombie', 'IsDummyCombatAvatar', 'IsATMGuardAI'):
            self.assertTrue(self.ns['_is_combat'](types.SimpleNamespace(**{flag: True})))

    def test_projectile_class_is_excluded_without_type_flags(self):
        skill = type('FireProjectile', (), {'IsCombatAvatar': True, 'IsRobotCombatAvatar': True})()
        self.assertFalse(self.ns['_is_combat'](skill))

    def test_magic_field_with_health_and_combat_flags_is_excluded(self):
        field = self.entity(IsMagicField=True, hp=100, IsCombatAvatar=True)
        self.assertFalse(self.ns['_is_combat'](field))
        self.assertEqual(self.gather(field), [])

    def test_callable_magic_field_flag_is_excluded(self):
        self.assertFalse(self.ns['_is_combat'](self.entity(IsMagicField=lambda: True)))
        self.assertTrue(self.ns['_is_combat'](self.entity(IsMagicField=lambda: False)))

    def test_magic_field_subclass_is_excluded_without_marker(self):
        base = type('Magic_Field', (), {})
        field = type('FireWall', (base,), {'id': 4, 'IsCombatAvatar': True, 'position': (0,0,10)})()
        self.assertFalse(self.ns['_is_combat'](field))
        self.assertEqual(self.gather(field), [])

    def test_player_with_magic_field_effect_remains_eligible(self):
        player = self.entity(magic_field_id=123, IsMagicField=False)
        self.assertTrue(self.ns['_is_combat'](player))
        self.assertEqual(len(self.gather(player)), 1)

    def test_vehicle_with_health_and_team_is_excluded(self):
        vehicle = types.SimpleNamespace(id=3, hp=1000, combat_team_id='8', position=(0, 0, 10), IsVehicle=True)
        self.assertFalse(self.ns['_is_combat'](vehicle))
        self.assertEqual(self.gather(vehicle), [])

    def test_even_vehicle_marked_combat_is_excluded(self):
        vehicle = self.entity(IsVehicle=lambda: True)
        self.assertFalse(self.ns['_is_combat'](vehicle))

    def test_vehicle_class_fallback_is_excluded(self):
        vehicle = type('HelicopterVehicle', (), {'hp': 100, 'IsCombatAvatar': True})()
        self.assertFalse(self.ns['_is_combat'](vehicle))

    def test_player_riding_vehicle_is_kept(self):
        player = self.entity(vehicle_id=900)
        self.assertEqual(len(self.gather(player)), 1)

    def test_behind_camera_player_is_kept_for_offscreen_arrow(self):
        player = self.entity()
        player.position = (0, 0, -10)
        items = self.gather(player)
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0][5:7], ((-1.0, -1.0), (-1.0, -1.0)))

    def test_offscreen_projection_is_preserved_for_arrow_direction(self):
        self.ns['_matrix_world_to_screen'] = lambda p: (1.4, -.2)
        self.assertEqual(self.gather(self.entity())[0][5], (1.4, -.2))

    def test_health_or_team_alone_does_not_make_a_player(self):
        for attrs in ({'hp': 100}, {'combat_team_id': '8'}, {'hp': 100, 'combat_team_id': '8'}):
            self.assertFalse(self.ns['_is_combat'](types.SimpleNamespace(**attrs)))

    def test_false_callable_combat_flag_is_not_truthy(self):
        self.assertFalse(self.ns['_is_combat'](types.SimpleNamespace(IsPlayerCombatAvatar=lambda: False)))

    def test_team_string_and_integer_match_even_with_friendly_fire(self):
        self.space.game_logic.CanDamage = lambda *args: True
        self.assertFalse(self.relation(self.entity(team=7)))

    def test_friendly_bot_is_not_forced_enemy(self):
        bot = self.entity(team='7', IsRobotCombatAvatar=True)
        self.assertFalse(self.relation(bot, True))
        self.assertTrue(self.gather(bot)[0][11])

    def test_teammate_api_overrides_different_ids(self):
        self.space.game_logic.IsTeammate = lambda *args: True
        self.space.game_logic.CanDamage = lambda *args: True
        self.assertFalse(self.relation(self.entity()))

    def test_damage_api_false_marks_friend(self):
        self.space.game_logic.CanDamage = lambda *args: False
        self.assertFalse(self.relation(self.entity()))

    def test_different_known_teams_are_enemies(self):
        self.assertTrue(self.relation(self.entity()))

    def test_empty_primary_team_falls_back_to_valid_team(self):
        e = self.entity(team='', team_id=7)
        self.assertEqual(self.ns['_team_id'](e), '7')
        self.assertFalse(self.relation(e))

    def test_unknown_player_relation_is_hidden_until_resolved(self):
        self.local.combat_team_id = ''
        target = self.entity(team='')
        self.space.game_logic.CanDamage = lambda *args: None
        self.assertFalse(self.relation(target))
        self.assertTrue(self.gather(target)[0][11])

    def test_hostile_training_bot_without_teams_is_kept(self):
        self.local.combat_team_id = ''
        bot = self.entity(team='', IsRobotCombatAvatar=True)
        self.assertTrue(self.relation(bot, True))

    def test_local_team_uses_player_when_avatar_team_is_empty(self):
        self.genv.avatar = self.entity(eid=1, team='')
        self.assertEqual(self.ns['_local']()[2], '7')

    def test_gather_keeps_team_marker_and_excludes_local_and_car(self):
        mate, enemy = self.entity(2, 7), self.entity(3, 8)
        car = self.entity(4, 8, IsVehicle=True)
        rows = self.gather(self.local, mate, enemy, car)
        self.assertEqual({r[2]: r[11] for r in rows}, {2: True, 3: False})


    def pick(self, rows, ignore_knocked, retained=False):
        cfg = [0] * 18
        cfg[17] = 2 if ignore_knocked else 0
        cfg[10] = 90
        if retained:
            return self.ns['_pick_eid'](rows, (0, 0, 0), (0, 0, 1), rows[0][2], 400, cfg, 0, True)
        return self.ns['_pick'](rows, (0, 0, 0), (0, 0, 1), 90, 400, cfg, 0, True, True)

    def test_aim_knock_filter_rejects_new_and_retained_targets(self):
        rows = self.gather(self.entity(knocked=True))
        self.assertIsNone(self.pick(rows, True))
        self.assertIsNone(self.pick(rows, True, retained=True))

    def test_aim_knock_filter_off_still_allows_knocked_target(self):
        target = self.entity(knocked=True)
        rows = self.gather(target)
        self.assertIs(self.pick(rows, False)[0], target)
        self.assertIs(self.pick(rows, False, retained=True)[0], target)

    def test_aim_knock_filter_keeps_standing_enemy(self):
        standing, knocked = self.entity(2), self.entity(3, knocked=True)
        self.assertIs(self.pick(self.gather(standing, knocked), True)[0], standing)

    def test_aim_drops_target_knocked_since_last_gather(self):
        target = self.entity()
        rows = self.gather(target)
        self.assertIsNotNone(self.pick(rows, True))
        target.knocked = True
        self.assertIsNone(self.pick(rows, True))
        self.assertIsNone(self.pick(rows, True, retained=True))

    def test_aim_knock_filter_can_toggle_without_gathering_again(self):
        rows = self.gather(self.entity(knocked=True))
        self.assertIsNotNone(self.pick(rows, False))
        self.assertIsNone(self.pick(rows, True))
        self.assertIsNotNone(self.pick(rows, False))

    def test_aim_hold_toggle_bit_does_not_enable_knock_filter(self):
        rows = self.gather(self.entity(knocked=True))
        cfg = [0] * 18
        cfg[17] = 1
        self.assertIsNotNone(self.ns['_pick'](rows, (0, 0, 0), (0, 0, 1), 90, 400, cfg, 0, True, True))


    def target_row(self, eid, angle=0, distance=100, team=False, knocked=False):
        e = self.entity(eid, knocked=knocked)
        angle = math.radians(angle)
        point = (math.sin(angle)*distance, 0, math.cos(angle)*distance)
        return (distance, e, eid, point, point, None, None, [], None, '', '8', team, knocked)

    def select_rows(self, rows, preferred=None, flags=0):
        cfg = [0] * 18
        cfg[3], cfg[10] = flags, 90
        return self.ns['_pick'](rows, (0,0,0), (0,0,1), 90, 400, cfg, 0, True, True, False, preferred)

    def test_best_target_prioritizes_center_alignment(self):
        centered, close = self.target_row(2, 1, 150), self.target_row(3, 20, 20)
        self.assertIs(self.select_rows([close, centered])[0], centered[1])

    def test_best_target_uses_distance_for_similar_angles(self):
        near, far = self.target_row(2, 3, 20), self.target_row(3, 2, 350)
        self.assertIs(self.select_rows([far, near])[0], near[1])

    def test_best_target_keeps_near_equal_current_target(self):
        current, rival = self.target_row(2, 3), self.target_row(3, 2)
        self.assertIs(self.select_rows([rival, current], preferred=2)[0], current[1])

    def test_best_target_switches_for_clearly_better_candidate(self):
        current, rival = self.target_row(2, 10), self.target_row(3, 0)
        self.assertIs(self.select_rows([current, rival], preferred=2)[0], rival[1])

    def test_best_target_releases_knocked_preference_for_standing_enemy(self):
        current, standing = self.target_row(2, 0, knocked=True), self.target_row(3, 10)
        self.assertIs(self.select_rows([current, standing], preferred=2)[0], standing[1])

    def test_best_target_prefers_enemy_over_allowed_teammate(self):
        mate, enemy = self.target_row(2, 0, team=True), self.target_row(3, 10)
        self.assertIs(self.select_rows([mate, enemy], preferred=2, flags=1<<15)[0], enemy[1])

    def test_best_target_tie_is_stable_across_entity_order(self):
        a, b = self.target_row(2), self.target_row(3)
        self.assertIs(self.select_rows([a,b])[0], self.select_rows([b,a])[0])

    def test_retained_target_cannot_bypass_visibility(self):
        row = self.target_row(2)
        self.ns['_line_of_sight_query'] = lambda *args: False
        cfg = [0] * 18
        cfg[3], cfg[10] = 1<<14, 90
        self.assertIsNone(self.select_rows([row], flags=1<<14))
        self.assertIsNone(self.ns['_pick_eid']([row], (0,0,0), (0,0,1), 2, 400, cfg, 0, True))

    def test_retained_target_cannot_bypass_fov(self):
        row = self.target_row(2, 60)
        cfg = [0] * 18
        cfg[10] = 90
        self.assertIsNone(self.select_rows([row], preferred=2))
        self.assertIsNone(self.ns['_pick_eid']([row], (0,0,0), (0,0,1), 2, 400, cfg, 0, True))

    def test_best_target_filters_dead_and_out_of_range(self):
        dead, far, valid = self.target_row(2), self.target_row(3, distance=500), self.target_row(4, 10)
        dead[1].dead = True
        self.ns['_alive'] = lambda e: not getattr(e, 'dead', False)
        self.assertIs(self.select_rows([dead,far,valid], preferred=2)[0], valid[1])


    def pick_points(self, points, mask=0x700, blocked=(), retained=False):
        row=self.target_row(2)
        self.ns['_sample_joint_transform']=lambda e,i: points.get(i)
        self.ns['_line_of_sight_query']=lambda e,p,*args: p not in blocked
        cfg=[0]*18;cfg[3]=1<<14;cfg[10]=90;cfg[17]=4|mask
        if retained:
            return self.ns['_pick_eid']([row],(0,0,0),(0,0,1),2,400,cfg,0,True)
        return self.ns['_pick']([row],(0,0,0),(0,0,1),90,400,cfg,0,True,True)

    def test_multipoint_head_wins_even_when_body_is_closer_to_center(self):
        head,neck,body=(5,1.8,100),(2,1.5,100),(0,1,100)
        self.assertEqual(self.pick_points({0:head,1:neck,2:body})[1],head)

    def test_multipoint_occluded_head_falls_back_to_neck(self):
        head,neck,body=(0,1.8,100),(0,1.5,100),(0,1,100)
        self.assertEqual(self.pick_points({0:head,1:neck,2:body},blocked=(head,))[1],neck)

    def test_multipoint_occluded_head_neck_falls_back_to_body(self):
        head,neck,body=(0,1.8,100),(0,1.5,100),(0,1,100)
        self.assertEqual(self.pick_points({0:head,1:neck,2:body},blocked=(head,neck))[1],body)

    def test_multipoint_only_samples_selected_bones(self):
        head,neck,body=(0,1.8,100),(0,1.5,100),(0,1,100)
        self.assertEqual(self.pick_points({0:head,1:neck,2:body},mask=1024)[1],body)

    def test_multipoint_missing_head_uses_selected_neck(self):
        neck=(0,1.5,100)
        self.assertEqual(self.pick_points({1:neck})[1],neck)

    def test_multipoint_all_missing_does_not_invent_head_position(self):
        self.assertIsNone(self.pick_points({}))

    def test_multipoint_fov_is_checked_for_each_point(self):
        head,neck=(100,1.8,1),(0,1.5,100)
        self.assertEqual(self.pick_points({0:head,1:neck})[1],neck)

    def test_multipoint_range_is_checked_for_each_point(self):
        head,neck=(0,1.8,500),(0,1.5,100)
        self.assertEqual(self.pick_points({0:head,1:neck})[1],neck)

    def test_multipoint_retention_keeps_same_priority_and_visibility(self):
        head,neck=(0,1.8,100),(0,1.5,100)
        self.assertEqual(self.pick_points({0:head,1:neck},blocked=(head,),retained=True)[1],neck)

    def test_multipoint_fully_occluded_target_is_rejected(self):
        head,neck=(0,1.8,100),(0,1.5,100)
        self.assertIsNone(self.pick_points({0:head,1:neck},blocked=(head,neck)))

    def test_multipoint_mask_migration_preserves_priority(self):
        cfg=[0]*18;cfg[17]=4|256|1024
        self.assertEqual(self.ns['_target_joint_indices'](cfg,4),(0,2))
        cfg[17]=0;cfg[3]=1<<24
        self.assertEqual(self.ns['_target_joint_indices'](cfg,0),(0,1,2))
        cfg[3]=0
        self.assertEqual(self.ns['_target_joint_indices'](cfg,1),(1,))
        cfg[17]=4
        self.assertEqual(self.ns['_target_joint_indices'](cfg,0),(0,))


if __name__ == '__main__':
    unittest.main()
