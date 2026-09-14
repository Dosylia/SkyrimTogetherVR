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

## 1. Sync features still off

Most sync hooks got their VR address from the TiltedEvolutionVR fork's table (see `VRAddressOverrides.h`).

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 37905 | `s_initiateMountPackage` (Actor.cpp) | Mounting a remote horse. Candidate VR 0x60e300; left off on purpose. | 0x14062D1D0 | SE 36881 → VR 0x60e300 (fp 2/5) | AE 37904 = SE 36880 = VR 0x60e130 | AE 37907 = SE 36883 = VR 0x60ee10 |

## 2. No VR address: patches and plumbing that are off

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 34452 | `hookLoc` (Projectile.cpp) | Null check patch in projectile launch. | 0x14056B8F0 | SE 33672 → VR 0x554980 (fp 3/5) | AE 34451 = SE 33671 = VR 0x5547a0 | AE 34456 = SE 33676 = VR 0x555020 |
| 82082 | `ProcessHook` (UI.cpp) | Menu queue patch while connected. | 0x140F04C90 | - | AE 82081 = SE 79944 = VR 0xf1a680 | AE 82167 = SE 80059 = VR 0xf1fbc0 |
| 51538 | `FavoritesCanProcess` (UI.cpp) | Favorites menu numbering while connected. | 0x1408A6B40 | SE 50644 → VR 0x8a3ee0 (fp 3/5) | AE 51537 = SE 50643 = VR 0x8a3e60 | AE 51539 = SE 50645 = VR 0x8a3f30 |
| 36548 | `MainInit` (UI.cpp) | Skipping the intro movie. | 0x1405D52E0 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 52510 | `ProcessMessage` (SkillsMenu.cpp) | Skills menu fix (with 52518). | 0x1408EE960 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 77226 | `initWindowLoc` / `renderInit` (BSGraphicsRenderer.cpp) | Renderer init hook (with 68781, 77246). Not needed: the VR menu is a SteamVR dashboard overlay. | 0x140DA3850 | - | AE 76844 = SE 75076 = VR 0xda4be0 | AE 77228 = SE 75447 = VR 0xdbabc0 |
| 68617 | `pollInputDevices` (BSInputDeviceManager.cpp) | Input focus check. Not needed on VR. | 0x140C3B360 | - | AE 68553 = SE 67253 = VR 0xc4e900 | AE 68618 = SE 67316 = VR 0xc51ac0 |
| 68276 | `unsignedInt` (BSRandom.cpp) | Game random numbers (with 14774); falls back to the minimum. | 0x140C2D180 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |
| 68261 | `threadInit` (BSThread.cpp) | Thread names for debugging (with 69554). | 0x140C2CD40 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |

## 3. No VR address: low priority

| AE id | Used as | What is off on VR | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 36564 | `cMainLoop` (SkyrimVM64.cpp) | Empty placeholder hook. Candidate VR 0x5bab10. | 0x1405D9F50 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 40412 | `cVMDestructor` (SkyrimVM64.cpp) | Empty placeholder hook. | 0x1406C19F0 | SE 39341 → VR 0x6a3a30 (fp 2/5) | AE 40411 = SE 39340 = VR 0x6a26a0 | AE 40414 = SE 39343 = VR 0x6bae40 |
| 37313 | `s_ForceState` (Actor.cpp) | Placeholder hook. | 0x1405F8860 | - | AE 37275 = SE 36286 = VR 0x5daae0 | AE 37334 = SE 36344 = VR 0x5de910 |
| 104359 | `s_signaturesMatch` (BSScript.cpp) | Pass-through hook. | 0x141366590 | - | AE 104321 = SE 97536 = VR 0x12708c0 | AE 104434 = SE 97692 = VR 0x12a0c50 |

## 4. Worked around, address still wanted

| AE id | Function | Current workaround |
| --- | --- | --- |
| 104788 | RegisterPapyrusFunction | Hooked from the VM vtable instead (VR 0x1278410 is known, but hooking both would conflict). |
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
| 14375 | `0x19c0c0` | SE 14257 TESNPC::SetLeveledNpc |
| 14529 | `0x19f970` | SE 14383 TESContainer::GetItemCount |
| 14953 | `0x1b0900` | SE 14775 TESTexture ctor |
| 15002 | `0x1b1f00` | SE address from the symbol name |
| 15006 | `0x1b1a30` | SE address from the symbol name |
| 16142 | `0x1fce80` | SE 15902 InventoryChanges::Save |
| 16143 | `0x1fcfb0` | SE 15903 InventoryChanges::Load |
| 18518 | `0x25b7c0` | SE 18133 BGSWorldLocation distance |
| 18563 | `0x25da30` | SE 18178 ImageSpaceModifierInstance stop |
| 19075 | `0x27a4c0` | SE 18606 TESObjectCELL::GetCOCPlacementInfo |
| 19364 | `0x28ccd0` | SE 18949 EventDispatcher::PushEvent |
| 19689 | `0x29f110` | SE 19263 TESObjectREFR::RemoveItem |
| 19702 | `0x29fac0` | SE 19276 TESObjectREFR::GetContainer |
| 19708 | `0x29fdb0` | SE 19282 TESObjectREFR::AddObjectToContainer |
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
| 27244 | `0x3f2ff0` | SE 26576 experience calculation |
| 32525 | `0x510b20` | SE 32525 combat target selector sort predicate |
| 32802 | `0x4fd1c0` | SE 32048 animation action helper |
| 32803 | `0x4fd300` | SE 32049 animation action helper |
| 32883 | `0x500890` | SE 32139 IAnimationGraphManagerHolder::RevertAnimationGraphManager |
| 33235 | `0x50e480` | SE 32488 CombatController::SetTarget |
| 33261 | `0x50ff00` | SE 32512 combat movement check |
| 33285 | `0x510c90` | SE 32528 combat target array quick sort |
| 34401 | `0x550540` | SE 33623 MagicCaster::CastSpell |
| 34512 | `0x557070` | SE 33728 MagicTarget::DispelAllSpells |
| 34525 | `0x557830` | SE 33741 MagicTarget::CheckAddEffectTargetData |
| 34529 | `0x557f80` | SE 33745 MagicTarget::GetTargetAsActor |
| 34989 | `0x569920` | SE 34193 SummonCreatureEffect start |
| 35086 | `0x56e070` | SE 34286 ValueModifierEffect::ApplyActorEffect |
| 35269 | `0x574bf0` | SE 34444 MenuTopicManager::PlayDialogueOption |
| 36000 | `0x59f160` | SE 35107 BGSLoadFormBuffer::ReadFormId |
| 36048 | `0x5a1070` | SE 35158 BGSSaveFormBuffer::WriteFormId |
| 36525 | `0x5ef640` | SE 36525 Actor::AddObjectToContainer |
| 36527 | `0x5efa10` | SE 36527 Actor::GetGoldAmount |
| 36741 | `0x604f30` | SE 36741 Actor::GetDetectionState |
| 37147 | `0x5d5120` | SE 36174 animation helper |
| 37175 | `0x5d63a0` | SE 36196 Character dtor |
| 37356 | `0x5e0e20` | SE 36365 Actor process (AI) update |
| 37511 | `0x5ee4f0` | SE 36511 TESObjectREFR::PayGoldToContainer |
| 37521 | `0x5eeca0` | SE 36521 Actor::PickUpObject |
| 37577 | `0x5f5280` | SE 36575 Actor::IsFleeing |
| 37677 | `0x600220` | SE 36669 Actor::SetFactionRank |
| 38899 | `0x640f30` | SE 37943 ActorEquipManager::UnequipAll |
| 38935 | `0x643910` | SE 37980 EquipManager internal UnequipShout |
| 38952 | `0x644070` | SE 37998 ActorMediator::PerformIdleAction |
| 38959 | `0x644510` | SE 38005 AI package init |
| 39002 | `0x645b10` | SE 38046 animation helper |
| 39114 | `0x64c170` | SE 38156 AIProcess::CheckForNewPackage |
| 39643 | `0x66dd50` | SE 38612 dialogue response processing |
| 40454 | `0x6c00f0` | SE 39382 Actor::DropObject |
| 40533 | `0x6c6e00` | SE 39456 PlayerCharacter::PickUpObject |
| 40535 | `0x6c74d0` | SE 39458 PlayerCharacter::SetWaypoint |
| 40536 | `0x6c7630` | SE 39459 PlayerCharacter::RemoveWaypoint |
| 42704 | `0x743170` | SE 41626 Actor::UpdateDetectionState |
| 47196 | `0x7e1ac0` | SE 45923 combat target validity check |
| 47303 | `0x7e7e90` | SE 46039 Actor::HasEquippedRangedWeapon |
| 47307 | `0x7e8020` | SE 46043 Actor world location check |
| 52627 | `0x8f9ea0` | SE 51754 SubtitleManager::HideSubtitle |
| 53926 | `0x95c670` | SE 53115 SkyrimVM::Update |
| 54425 | `0x976690` | SE 53604 EventDispatcher::RegisterSink |
| 55497 | `0x9ad8a0` | SE 54864 beast form change |
| 57185 | `0xa052c0` | SE 56763 behavior symbol lookup |
| 58377 | `0xa2e820` | SE 57804 hkbBehaviorGraph::HandleEvents |
| 58378 | `0xa2ea40` | SE 57805 hkbBehaviorGraph event helper |
| 59310 | `0xa4dca0` | SE 58656 behavior generator event |
| 60079 | `0xa88950` | SE 59405 behavior graph update |
| 63362 | `0xb1cfa0` | SE 62420 BSAnimationGraphManager send event |
| 63372 | `0xb1d8b0` | SE 62430 BSAnimationGraphManager event helper |
| 68221 | `0xc413f0` | SE 66964 CRC hash stub |
| 68545 | `0xc4e600` | SE 67245 BSInputEnableManager::EnableOtherEvent |
| 69165 | `0xc6dc90` | SE 67823 BSFixedString::Set |
| 70639 | `0xcac090` | SE 69269 NiCamera::WorldPtToScreenPt3 |
| 70717 | `0xcaef60` | SE 69335 FaceGen CreateTexture |
| 76207 | `0xd8a900` | SE 74481 BSFaceGenNiNode::GetObjectByName |
| 82074 | `0xf1a3b0` | SE 79937 UI::IsMenuOpen |
| 82088 | `0xf1bf10` | SE 79951 UI close all menus |
| 382393 | `0x1eb1d08` | combat detection time limit setting value |
| 382400 | `0x1eb1d24` | combat recent LOS time limit setting value |
| 400312 | `0x1f8319c` | invalid reference handle |
| 401100 | `0x2fc4880` | animation global |
| 403568 | `0x2febcc0` | SE 517060 animation global (next to 403566/403567) |
| 403988 | `0x2fff008` | animation global |
| 405282 | `0x3010168` | combat setting value |
| 414391 | `0x3422948` | script object handle policy |
| 414675 | `0x3423e20` | SE 527752 NiMaskedShader NiRTTI |

### From the SE/VR binary-diff table only

The least trusted: taken from a diff table that was wrong for every RTTI entry it was tested on.

| AE id | VR address | Used as |
| --- | --- | --- |
| 53112 | `0x95c430` | MapMenu hookLoc |
