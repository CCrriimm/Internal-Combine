# BloodStrike in-match appearance functions

Captured on 2026-11-04 from the embedded Python 3.14 runtime. See [scope](combat_scope.md). This report describes the original metadata/bytecode discovery pass, which did not apply skins or trace their execution in a match.

## Gun model and material paths

`WeaponCase.ChangeWeaponGuise(self, guise_id, data_changed=True, need_callback=True, guise_item_id=None, clear_anim=True)` is defined in game module `gclient/gameplay/logic_base/equips/weapon_case.py:4210`.

- `guise_id` identifies the appearance template; `guise_item_id` identifies the separate lobby item.
- The method checks simplified-model/resource state and returns early when the template has not changed.
- It updates the case and weapon-body guise IDs, clears cached item properties, refreshes resource state and calls `weapon_body.WeaponBodyIllusion(need_callback)`.
- It refreshes weapon/hand/dual-weapon animation, first-person properties in combat, materials and the kill counter.
- The method is reused outside combat. The object and `is_in_combat`, `is_fps_weapon` and `is_tps_weapon` flags identify the gameplay context.

`WeaponGuiseComponent.WeaponBodyIllusion(self, need_callback)`, in `gclient/util/gun_skin_util.py:2180`, clears old effects, initializes the guise and calls InitWeapon to recreate weapon parts.

`WeaponCase.ChangeWeaponSkin(self, skin_id, data_changed=True, force_refresh=False)`, line 4181, updates body/charger/part materials through ChangePartSkinRecursively and LoadModelSkin. It is the material/camo path; an arbitrary item ID is not interchangeable with every parameter named skin.

`WeaponCase.ChangeWeaponDecorate(self, skin_id, guise_id, hangings_id, sticker_info)`, line 4162, coordinates ChangeWeaponGuise, ChangeWeaponSkin, ChangeWeaponHangings and ChangeWeaponSticker. The active first-person combat branch also handles ADS and RaiseWeapon.

`AvatarModel.AddWeapon(guid)` connects this path to combat equipment: it reads the backpack entry's display_equip_id, skin_template_id and guise_template_id, then uses EquipCaseFactory.Create and InitWeapon.

## Melee and Artifact paths

`MeleeCase.ChangeWeaponGuise(self, guise_id, guise_item_id=None)`, in game module `melee_case.py:153`, selects the model from the template and calls ChangePartModel with OnWeaponModelLoad. The captured bytecode does not consume guise_item_id. Its later scabbard branch applies outside combat.

`MeleeCase.RefreshModelSkin(self)`, line 439, loads skin material from guise_id when gun_skin_part_mat_list exists. Material replacement does not replace the model-change step.

`DualMeleeCase.ChangeWeaponGuise(self, guise_id, guise_item_id=None)`, in `dual_melee_case.py:169`, calls its parent and also changes the left-hand model. Cross-class melee replacement was not established by this capture.

`HeirloomCase.ChangeHeirloomAttr(self, blade_attr=None, handle_attr=None, core_attr=None, effect_attr=None, level=None)`, in `heirloom_case.py:1184`, calls _SetHeirloomAttrs and RefreshWeaponAppearanceAttrs. A single Artifact item ID cannot substitute for those inputs. `RefreshGuiseAppearance`, line 1256, refreshes the loaded appearance and material through GetSkinDictByAttrs and weapon_body.LoadModelSkin.

## Character path

`PlayerCombatAvatarMember._on_set_arm_clothes(self, old)`, in game module `cimp_model.py:469`, recomputes GetHeroSkinProto, calls ShowModel and broadcasts ON_PLAYER_COMBAT_ARM_CLOTHES_CHANGED. The old argument is the previous clothing value, not the requested skin ID.

`ShowModel(self)`, line 713, uses model.Load for initial creation and model.Reload for an existing model. The reload path includes camera/ADS handling and first-person hand-model refresh.

## Evidence and call paths

`bloodstrike_combat_functions.json` contains captured signatures and code metadata; `bloodstrike_combat_bytecode.txt` contains the focused disassembly. Discovery examined 968 related functions and selected 22 code objects for closer analysis. Every Frida session was detached in cleanup.

- Gun: combat backpack entry -> EquipCaseFactory -> WeaponCase.ChangeWeaponGuise -> WeaponBodyIllusion -> InitWeapon.
- Melee: ChangeWeaponGuise -> ChangePartModel -> OnWeaponModelLoad -> RefreshModelSkin; dual melee also changes the left-hand model.
- Character: arm_clothes callback -> GetHeroSkinProto -> ShowModel -> model Load/Reload -> hand-model refresh.

These are bytecode-supported call-flow findings. The original pass did not establish server synchronization, appearance to other players, compatibility of every item, or a universal ApplySkin(item_id) function. Later integration results are recorded separately.
