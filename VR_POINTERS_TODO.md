# VR pointers still to find

Game addresses Skyrim Together still lacks, or has never confirmed, on Skyrim VR 1.4.15.
Regenerated 2026-09-13 from the current code. Resolved addresses live in `Code/client/VRAddressOverrides.h`.

## Read this first: the ids are AE ids

Every id in the Skyrim Together source is an **Anniversary Edition (1.6.x)** Address Library id. VR
uses SE numbering, so the same number in the SE or VR table is a *different* function. Always go
**AE id → SE id → VR address**. Example: AE 32883 is SE 32139 (VR `0x140500890`); SE 32883 is
something else.

## How to find and confirm one

1. Start from the AE 1.6.318 address, or the name in "Used as".
2. The two nearest known neighbours are given as `AE id = SE id = VR address`. Ids are roughly in
   address order, so the target usually sits between those two VR addresses.
3. Confirm with function sizes: the neighbours in VR have the same sizes and order as in SE. This
   rule was right on 153 of 153 known answers.
4. Check the candidate in the disassembly (callers, arguments), then add it to
   `VRAddressOverrides.h` as `SE <id> <name>, checked`.

## 1. No VR address: sync features that are off

Ordered by how much it affects co-op.

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 37356 | `s_actorProcess` (Actor.cpp) | **Remote NPCs still run their own AI.** The hook skips AI for actors another player owns, so both copies act. | 0x1405FD7E0 | - | AE 37335 = SE 36345 = VR 0x5de930 | AE 37383 = SE 36392 = VR 0x5e2ec0 |
| 37525 | `s_addInventoryItem` (Actor.cpp) | Items added to actors are not sent to other players. | - | - | - | - |
| 19708 | `s_addInventoryItem` (TESObjectREFR.cpp) | Items added to containers are not sent to other players. | 0x1402A0930 | - | AE 19703 = SE 19277 = VR 0x29faf0 | AE 19728 = SE 19301 = VR 0x2a1c60 |
| 37521 | `s_pickUpObject` (Actor.cpp) | NPC item pickups are not synced. | 0x14060C510 | - | AE 37491 = SE 36492 = VR 0x5eba50 | AE 37532 = SE 36532 = VR 0x5efc20 |
| 40533 | `s_pickUpObject` (PlayerCharacter.cpp) | Player item pickups are not synced. | 0x1406CD770 | - | AE 40501 = SE 39425 = VR 0x6c38a0 | AE 40548 = SE 39471 = VR 0x6cbce0 |
| 33282 | `sortTargetSelectors` (CombatController.cpp) | NPC combat target selection with remote players. | - | - | - | - |
| 37577 | `isFleeing` (Actor.cpp) | Fleeing state of NPCs. | 0x140612EF0 | - | AE 37560 = SE 36559 = VR 0x5f44a0 | AE 37610 = SE 36602 = VR 0x5f6460 |
| 38959 | `s_initFromPackage` (References.cpp) | AI package sync (only with AI_SYNC). | 0x140661820 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 34989 | `s_start` (SummonCreatureEffect.h) | Summoned creatures. | 0x140580040 | SE 34193 → VR 0x569920 (fp 2/5) | AE 34988 = SE 34192 = VR 0x5698d0 | AE 34990 = SE 34194 = VR 0x5699e0 |
| 34140 | `s_interruptCast` (ActorMagicCaster.cpp) | Interrupted spell casts. | 0x14055C700 | - | AE 34130 = SE 33348 = VR 0x5494f0 | AE 34144 = SE 33363 = VR 0x546260 |
| 34370 | `s_finish` (InvisibilityEffect.cpp) | End of invisibility. | 0x1405661D0 | - | AE 34343 = SE 33561 = VR 0x54d8b0 | AE 34400 = SE 33622 = VR 0x5503e0 |
| 21622 | `hasPerk` (MagicTarget.cpp) | Perk checks for remote spell effects. | 0x1402F1260 | - | - | AE 21910 = SE 21425 = VR 0x2f9900 |
| 34053 | `adjustForPerks` (MagicTarget.cpp) | Perk adjustments for remote spell effects. | 0x140557F30 | - | AE 33917 = SE 33138 = VR 0x9257f0 | AE 34057 = SE 33282 = VR 0x540ea0 |
| 35269 | `s_playDialogueOption` (MenuTopicManager.cpp) | Dialogue choices. | 0x14058B5D0 | - | AE 35220 = SE 34413 = VR 0x572fd0 | AE 35287 = SE 34460 = VR 0x5766f0 |
| 39643 | `s_processResponse` (Actor.cpp) | Dialogue responses of remote players. | 0x14068BEE0 | SE 38612 → VR 0x66dd50 (fp 1/5) | AE 39642 = SE 38611 = VR 0x66d9a0 | AE 39647 = SE 38616 = VR 0x670830 |
| 37542 | `s_speakSoundFunction` (Actor.cpp) | Dialogue voice lines. | 0x14060E8E0 | - | AE 37537 = SE 36537 = VR 0x5f0560 | AE 37560 = SE 36559 = VR 0x5f44a0 |
| 52626 | `s_showSubtitle` (SubtitleManager.cpp) | Subtitles (with 52627 HideSubtitle). | 0x1408FBD50 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 14375 | `s_SetLeveledNpc` (TESNPC.cpp) | Leveled NPCs can pick a different base NPC per player. | 0x140196810 | - | AE 14299 = SE 14190 = VR 0x199cf0 | AE 14383 = SE 14261 = VR 0x19c3b0 |
| 36291 | `s_SimulateTime` (TimeManager.cpp) | Waiting and sleeping time. | 0x1405C7220 | - | AE 36215 = SE 35320 = VR 0x5aa320 | AE 36311 = SE 35413 = VR 0x5add00 |
| 27244 | `s_calculateExperience` (PlayerCharacter.cpp) | Shared experience. | 0x1403FC280 | - | AE 27203 = SE 26570 = VR 0x3f0e00 | AE 27263 = SE 26592 = VR 0x3f4420 |
| 55497 | `s_setBeastForm` (PlayerCharacter.cpp) | Werewolf and vampire lord transformations. | 0x14099C360 | - | AE 55495 = SE 54862 = VR 0x76b780 | AE 55506 = SE 54939 = VR 0x9b3800 |
| 40535 | `s_setWaypoint` (PlayerCharacter.cpp) | Shared map markers (with 40536 RemoveWaypoint). | 0x1406CDE40 | - | AE 40501 = SE 39425 = VR 0x6c38a0 | AE 40548 = SE 39471 = VR 0x6cbce0 |
| 37905 | `s_initiateMountPackage` (Actor.cpp) | Mounting a remote horse. Parked on purpose. | 0x14062D1D0 | SE 36881 → VR 0x60e300 (fp 2/5) | AE 37904 = SE 36880 = VR 0x60e130 | AE 37907 = SE 36883 = VR 0x60ee10 |
| 32803 | `sub_1404ED090` (AnimationExperiments.cpp) | Replaying complex actions. | 0x140505C10 | - | AE 32486 = SE 31718 = VR 0x4e8240 | AE 32884 = SE 32192 = VR 0x502890 |

## 2. No VR address: patches and plumbing that are off

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 34452 | `hookLoc` (Projectile.cpp) | Null check patch in projectile launch. | 0x14056B8F0 | SE 33672 → VR 0x554980 (fp 3/5) | AE 34451 = SE 33671 = VR 0x5547a0 | AE 34456 = SE 33676 = VR 0x555020 |
| 82082 | `ProcessHook` (UI.cpp) | Menu queue patch while connected. | 0x140F04C90 | - | AE 82081 = SE 79944 = VR 0xf1a680 | AE 82167 = SE 80059 = VR 0xf1fbc0 |
| 51538 | `FavoritesCanProcess` (UI.cpp) | Favorites menu numbering while connected. | 0x1408A6B40 | SE 50644 → VR 0x8a3ee0 (fp 3/5) | AE 51537 = SE 50643 = VR 0x8a3e60 | AE 51539 = SE 50645 = VR 0x8a3f30 |
| 36548 | `MainInit` (UI.cpp) | Skipping the intro movie. | 0x1405D52E0 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 52510 | `ProcessMessage` (SkillsMenu.cpp) | Skills menu fix (with 52518). | 0x1408EE960 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 77226 | `initWindowLoc` / `renderInit` (BSGraphicsRenderer.cpp) | Renderer init hook (with 68781, 77246). Not needed while there is no VR overlay. | 0x140DA3850 | - | AE 76844 = SE 75076 = VR 0xda4be0 | AE 77228 = SE 75447 = VR 0xdbabc0 |
| 68617 | `pollInputDevices` (BSInputDeviceManager.cpp) | Input focus check. Not needed on VR. | 0x140C3B360 | - | AE 68553 = SE 67253 = VR 0xc4e900 | AE 68618 = SE 67316 = VR 0xc51ac0 |
| 68276 | `unsignedInt` (BSRandom.cpp) | Game random numbers (with 14774); falls back to the minimum. | 0x140C2D180 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |
| 68261 | `threadInit` (BSThread.cpp) | Thread names for debugging (with 69554). | 0x140C2CD40 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |
| 37527 | `s_getGoldAmount` (Actor.cpp) | Gold amount, always 0. | - | - | - | - |

## 3. No VR address: low priority

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 36564 | `cMainLoop` (SkyrimVM64.cpp) | Empty placeholder hook. | 0x1405D9F50 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 40412 | `cVMDestructor` (SkyrimVM64.cpp) | Empty placeholder hook. | 0x1406C19F0 | SE 39341 → VR 0x6a3a30 (fp 2/5) | AE 40411 = SE 39340 = VR 0x6a26a0 | AE 40414 = SE 39343 = VR 0x6bae40 |
| 37313 | `s_ForceState` (Actor.cpp) | Placeholder hook. | 0x1405F8860 | - | AE 37275 = SE 36286 = VR 0x5daae0 | AE 37334 = SE 36344 = VR 0x5de910 |
| 104359 | `s_signaturesMatch` (BSScript.cpp) | Pass-through hook. | 0x141366590 | - | AE 104321 = SE 97536 = VR 0x12708c0 | AE 104434 = SE 97692 = VR 0x12a0c50 |
| 37757 | `getDetectionState` (CombatView.cpp) | Debug view only. | - | - | - | - |

## 4. Worked around, address still wanted

| AE id | Function | Current workaround |
| --- | --- | --- |
| 38533 | SetNoBleedoutRecovery | Calls the Papyrus native instead. |
| 37677 | SetFactionRank | Calls the Papyrus native instead. |
| 104788 | RegisterPapyrusFunction | Hooked from the VM vtable instead. |
| 16113 | InventoryChanges::GetArmorInSlot | Not a function on VR (CommonLibVR reimplements it); the naked-NPC fix is off. |

## 5. Has an address, never confirmed

These work in play so far, but were only matched by neighbours and function sizes. Confirm them in the
disassembly when a crash points near one.

| AE id | VR address | Matched as |
| --- | --- | --- |
| 11612 | `0x11e470` | SE 11466 ExtraDataList::SetWorn |
| 11616 | `0x11ea00` | SE 11470 ExtraDataList::SetHealth |
| 11619 | `0x11ede0` | SE 11473 ExtraDataList::SetCharge |
| 11620 | `0x11ef40` | SE 11474 ExtraDataList::SetSoul |
| 11822 | `0x12a640` | SE 11676 ExtraDataList::SetPoison |
| 12401 | `0x1454a0` | SE 12274 Lock::SetLock |
| 13718 | `0x17c4e0` | SE 13620 ModManager::GetCellFromCoordinates |
| 14529 | `0x19f970` | SE 14383 TESContainer::GetItemCount |
| 14953 | `0x1b0900` | SE 14775 TESTexture ctor |
| 16142 | `0x1fce80` | SE 15902 InventoryChanges::Save |
| 16143 | `0x1fcfb0` | SE 15903 InventoryChanges::Load |
| 19075 | `0x27a4c0` | SE 18606 TESObjectCELL::GetCOCPlacementInfo |
| 19364 | `0x28ccd0` | SE 18949 EventDispatcher::PushEvent |
| 19689 | `0x29f110` | SE 19263 TESObjectREFR::RemoveItem |
| 19702 | `0x29fac0` | SE 19276 TESObjectREFR::GetContainer |
| 19784 | `0x2a7ba0` | SE 19357 TESObjectREFR::GetWorldLocation |
| 19846 | `0x2ad090` | SE 19418 TESObjectREFR::GetHandle |
| 20203 | `0x2b8480` | SE 19798 GetLocationEncounterZone |
| 20221 | `0x2b89e0` | SE 19816 TESObjectREFR::CreateLock |
| 23231 | `0x332a90` | SE 22754 TESQuest::CompleteAllObjectives |
| 24568 | `0x367980` | SE 24065 TESIdleForm property |
| 24987 | `0x37f980` | SE 24468 TESQuest::SetStopped |
| 25004 | `0x3803d0` | SE 24482 TESQuest::SetStage |
| 26244 | `0x3c4970` | SE 25697 Sky::ReleaseWeatherOverride |
| 27040 | `0x3eada0` | SE 26454 FaceGen CreateTints |
| 32883 | `0x500890` | SE 32139 IAnimationGraphManagerHolder::RevertAnimationGraphManager |
| 34401 | `0x550540` | SE 33623 MagicCaster::CastSpell |
| 34512 | `0x557070` | SE 33728 MagicTarget::DispelAllSpells |
| 34525 | `0x557830` | SE 33741 MagicTarget::CheckAddEffectTargetData |
| 34529 | `0x557f80` | SE 33745 MagicTarget::GetTargetAsActor |
| 35086 | `0x56e070` | SE 34286 ValueModifierEffect::ApplyActorEffect |
| 36000 | `0x59f160` | SE 35107 BGSLoadFormBuffer::ReadFormId |
| 36048 | `0x5a1070` | SE 35158 BGSSaveFormBuffer::WriteFormId |
| 37511 | `0x5ee4f0` | SE 36511 TESObjectREFR::PayGoldToContainer |
| 38899 | `0x640f30` | SE 37943 ActorEquipManager::UnequipAll |
| 38935 | `0x643910` | SE 37980 EquipManager internal UnequipShout |
| 39114 | `0x64c170` | SE 38156 AIProcess::CheckForNewPackage |
| 40454 | `0x6c00f0` | SE 39382 Actor::DropObject |
| 42704 | `0x743170` | SE 41626 Actor::UpdateDetectionState |
| 47303 | `0x7e7e90` | SE 46039 Actor::HasEquippedRangedWeapon |
| 47307 | `0x7e8020` | SE 46043 Actor world location check |
| 53926 | `0x95c670` | SE 53115 SkyrimVM::Update |
| 54425 | `0x976690` | SE 53604 EventDispatcher::RegisterSink |
| 58377 | `0xa2e820` | SE 57804 hkbBehaviorGraph::HandleEvents |
| 58378 | `0xa2ea40` | SE 57805 hkbBehaviorGraph event helper |
| 68221 | `0xc413f0` | SE 66964 CRC hash stub |
| 68545 | `0xc4e600` | SE 67245 BSInputEnableManager::EnableOtherEvent |
| 69165 | `0xc6dc90` | SE 67823 BSFixedString::Set |
| 70639 | `0xcd5180` | SE 70273 NiCamera::WorldPtToScreenPt3 |
| 70717 | `0xcaef60` | SE 69335 FaceGen CreateTexture |
| 76207 | `0xd8a900` | SE 74481 BSFaceGenNiNode::GetObjectByName |
| 82074 | `0xf1a3b0` | SE 79937 UI::IsMenuOpen |
| 414675 | `0x3423e20` | SE 527752 NiMaskedShader NiRTTI |

### From the SE/VR binary-diff table only

The least trusted: taken from a diff table that was wrong for every RTTI entry it was tested on.

| AE id | VR address | Used as |
| --- | --- | --- |
| 15002 | `0x1bb6a0` | AnimationExperiments sub_1401A2220 |
| 15006 | `0x1bb820` | AnimationExperiments sub_1401A1D50 |
| 18518 | `0x275180` | CombatView getModifiedDistance |
| 18563 | `0x277910` | ImageSpaceModifierInstance clearImageSpaceModifier |
| 32802 | `0x52cf30` | AnimationExperiments sub_1404ECF50 |
| 33235 | `0x533c10` | CombatController setTarget |
| 33261 | `0x5402d0` | CombatView checkMovement |
| 33285 | `0x5410a0` | CombatController arrayQuickSort |
| 36544 | `0x5f2410` | TiltedOnlineApp winMain |
| 37147 | `0x61e640` | AnimationExperiments sub_1405CCB20 |
| 37175 | `0x61f300` | Actor characterDtor |
| 38952 | `0x68aef0` | AnimationExperiments PerformIdleAction |
| 39002 | `0x68e580` | AnimationExperiments sub_14063CAA0 |
| 47196 | `0x818380` | CombatView isValidTarget |
| 53112 | `0x95c430` | MapMenu hookLoc |
| 57185 | `0xa10880` | AnimationExperiments sub_1409CA9D0 |
| 59310 | `0xa7b730` | AnimationExperiments sub_140A13150 |
| 60079 | `0xaa6570` | AnimationExperiments sub_140A4DFA0 |
| 63362 | `0xb57580` | AnimationExperiments BSSendEvent |
| 63372 | `0xb57b00` | AnimationExperiments sub_140AE2DB0 |
| 82088 | `0xf93250` | UI CloseAll |
| 382393 | `0x1bb3870` | CombatView value |
| 382400 | `0x1bb38f4` | CombatView value |
| 400312 | `0x1c39c68` | TESObjectREFR nullHandle |
| 401100 | `0x1c3cdc0` | AnimationExperiments qword_142EFF990 |
| 403568 | `0x1c494c8` | AnimationExperiments qword_142F271C8 |
| 403988 | `0x1c4bde0` | AnimationExperiments qword_142F3A1E8 |
| 404125 | `0x1c4d1b0` | AITimer value |
| 405282 | `0x1c53d48` | CombatView value |
| 406126 | `0x1c58540` | HUDMenuUtils matrix |
| 406160 | `0x1c587d8` | HUDMenuUtils port |
| 414391 | `0x1c8d5e8` | BSScript policy |
