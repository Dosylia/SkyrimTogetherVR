# VR pointers still to find

Game addresses Skyrim Together still lacks, or has never confirmed, on Skyrim VR 1.4.15.
Regenerated 2026-09-16 from the current code, in SE numbering. Resolved addresses live in `Code/client/VRAddressOverrides.h`.

## Everything here is in SE numbering

Ids and addresses below are **SE (1.5.97)**, the numbering the VR Address Library uses. The last
column keeps the AE (1.6.x) id, because that is what the Skyrim Together source uses and it is the
only way to find the line to change; ignore it otherwise. The same number is a different function in
each numbering: AE 32883 is SE 32139 (VR `0x140500890`).

A `?` in the SE columns means no source pairs that function with an SE id, so only the AE address is
known and the search has to start there. Everything that could be paired was, on 2026-09-16, from four
sources: the SE/AE table, CommonLib's `RELOCATION_ID(se, ae)` pairs, this repo's own SE-era code
(commit `5da6679e`, which still used literal SE addresses), and reverse lookup of a known VR address in
the VR Address Library.

"not in the VR library" means the SE id is known but the library has no VR address for it, so it still
needs the neighbour and size work. A VR address in brackets means the library has it and no guessing is
needed.

## The VR Address Library has a few wrong entries

Ids and addresses both run in address order, so an entry sitting far from its neighbours is wrong. Eight
of the library's 13291 entries are more than 1MB out of place: **39346, 51859, 74237, 74491, 75445,
100997, 257153, 306372**. SE 75445 is the one that was caught in practice (the library says 0x5be850; the
function is at 0xdba850). None of the eight is an address this client resolves, checked 2026-09-16.

So a library entry is worth a size check before it is trusted for a hook or a byte patch: take the SE size
from the id's neighbours, measure the VR function, and confirm it chains onto the next known id.

Sources used below, most to least trusted: the official VR Address Library CSV, `vr_address_tools/database.csv`
(names and VR addresses, with a confidence column), the SE/AE pair table, then neighbours and sizes.

## How to find and confirm one

1. Start from the SE 1.5.97 address, or the name in "Used as".
2. The two nearest known neighbours are given as `SE id = VR address`. Ids are roughly in address
   order, so the target usually sits between those two VR addresses.
3. Confirm with function sizes: the neighbours in VR have the same sizes and order as in SE. This
   rule was right on 153 of 153 known answers.
4. Check the candidate in the disassembly (callers, arguments), then add it to
   `VRAddressOverrides.h` as `SE <id> <name>, checked`.

## 1. Sync features still off

Most sync hooks got their VR address from the TiltedEvolutionVR fork's table (see `VRAddressOverrides.h`).

| SE id | SE 1.5.97 address | Used as | What is off on VR | VR candidate | Neighbour below | Neighbour above | AE id in the source |
| --- | --- | --- | --- | --- | --- | --- | --- |

## 2. No VR address: patches and plumbing that are off

| SE id | SE 1.5.97 address | Used as | What is off on VR | VR candidate | Neighbour below | Neighbour above | AE id in the source |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 51638 | 0x1408BF360 | `ProcessMessage` (SkillsMenu.cpp) | Skills menu fix, only needed if the skills menu is unpaused on VR again (unpaused without it, it was black). Confirmed 2026-09-16. Of its three NOPs, the freeze-frame one is located on VR: AE +0xA10 = **VR +0xBB6** (`or dword ptr [rsi+0x1c], 0x20`, 4 bytes, sets kFreezeFrameBackground). The other two (AE +0x84E, 6 bytes, "menu not appearing"; AE +0x1040, 2 bytes, "keep the menu updated") are Skyrim Together's own and have no VR offset yet. The control patch AE 52518 is `StatsMenu::CanProcess` +0x46 (6 bytes), VR address unknown. | 0x8ec3e0 (confirmed) | SE 51618 = VR 0x8ead70 | SE 51755 = VR 0x8fa0a0 | 52510 @ 0x1408EE960 |
| 75445 | 0x140D68DD0 | `initWindowLoc` / `renderInit` (BSGraphicsRenderer.cpp) | Renderer init hook (with AE 68781). Not needed: the VR menu is a SteamVR dashboard overlay, and the frame end call it came with is resolved (SE 75461, VR 0xdbbdd0, now used by the body sync). | **0xdba850** (confirmed 2026-09-16: SE size 0x350, VR 0x34a, and it chains exactly onto SE 75446 then SE 75447 at 0xdbabc0). The VR Address Library says 0x5be850 for this id, which is wrong. | SE 75076 = VR 0xda4be0 | SE 75447 = VR 0xdbabc0 | 77226 @ 0x140DA3850 |
| 67315 | 0x140C150B0 | `pollInputDevices` (BSInputDeviceManager.cpp) | **Stays off on purpose.** It skips input polling while the game window is not focused, which in a headset is most of the time (the desktop mirror). Address confirmed 2026-09-16. | 0xc519e0 (confirmed) | SE 67253 = VR 0xc4e900 | SE 67316 = VR 0xc51ac0 | 68617 @ 0x140C3B360 |
| ? | ? | `unsignedInt` (BSRandom.cpp) | **Nothing calls this helper**, on any platform, so the missing address costs nothing. | - | SE 66988 = VR 0xc42730 | SE 67151 = VR 0xc485e0 | 68276 @ 0x140C2D180 |
| ? | ? | `threadInit` (BSThread.cpp) | Thread names for debugging. Its companion AE 69554 is SE 68203 @ 0x140C39950 (CommonLib pair), which is not in the VR library. | - | SE 66988 = VR 0xc42730 | SE 67151 = VR 0xc485e0 | 68261 @ 0x140C2CD40 |

## 3. No VR address: low priority

| SE id | SE 1.5.97 address | Used as | What is off on VR | VR candidate | Neighbour below | Neighbour above | AE id in the source |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 35565 | 0x1405B2FF0 | `cMainLoop` (SkyrimVM64.cpp) | Empty placeholder hook, so nothing is lost by leaving it off. | 0x5bab10 (in the VR library as SE 35565) | SE 35492 = VR 0x5b1710 | SE 51246 = VR 0x8d0500 | 36564 @ 0x1405D9F50 |
| 39341 | 0x140699DF0 | `cVMDestructor` (SkyrimVM64.cpp) | Empty placeholder hook, so nothing is lost by leaving it off. | 0x6a3a30 (confirmed 2026-09-16: follows the constructor as on SE, and starts by writing vtable pointers) | SE 39340 = VR 0x6a26a0 | SE 39343 = VR 0x6bae40 | 40412 @ 0x1406C19F0 |
| 36323 | 0x1405D4090 | `s_ForceState` (Actor.cpp) | Placeholder hook. | - (SE 36323 is not in the VR library) | SE 36286 = VR 0x5daae0 | SE 36344 = VR 0x5de910 | 37313 @ 0x1405F8860 |
| ? | ? | `s_signaturesMatch` (BSScript.cpp) | Pass-through hook. | - | SE 97536 = VR 0x12708c0 | SE 97692 = VR 0x12a0c50 | 104359 @ 0x141366590 |

## 4. Worked around, address still wanted

| SE id | SE 1.5.97 address | Function | Current workaround | AE id in the source |
| --- | --- | --- | --- | --- |
| 98065 | 0x141258CB0 | RegisterPapyrusFunction | Hooked from the VM vtable instead (VR 0x1278410 is known, but hooking both would conflict). SE 98065 is not in the VR library. | 104788 |
| 15873 | 0x1401E7570 | InventoryChanges::GetArmorInSlot | Not a function on VR (CommonLibVR reimplements it); the naked-NPC fix is off. SE 15873 is not in the VR library either. | 16113 |

## 5. Has an address, never confirmed

These work in play so far, but were only matched by neighbours and function sizes. Confirm them in the
disassembly when a crash points near one.

| SE id | VR address | What it is | AE id in the source |
| --- | --- | --- | --- |
| 11466 | `0x11e470` | ExtraDataList::SetWorn | 11612 |
| 11470 | `0x11ea00` | ExtraDataList::SetHealth | 11616 |
| 11473 | `0x11ede0` | ExtraDataList::SetCharge | 11619 |
| 11474 | `0x11ef40` | ExtraDataList::SetSoul | 11620 |
| 11676 | `0x12a640` | ExtraDataList::SetPoison | 11822 |
| 12274 | `0x1454a0` | Lock::SetLock | 12401 |
| 13620 | `0x17c4e0` | ModManager::GetCellFromCoordinates | 13718 |
| 14257 | `0x19c0c0` | TESNPC::SetLeveledNpc | 14375 |
| 14383 | `0x19f970` | TESContainer::GetItemCount | 14529 |
| 14775 | `0x1b0900` | TESTexture ctor | 14953 |
| ? | `0x1b1f00` | SE address from the symbol name | 15002 |
| ? | `0x1b1a30` | SE address from the symbol name | 15006 |
| 15902 | `0x1fce80` | InventoryChanges::Save | 16142 |
| 15903 | `0x1fcfb0` | InventoryChanges::Load | 16143 |
| 18133 | `0x25b7c0` | BGSWorldLocation distance | 18518 |
| 18178 | `0x25da30` | ImageSpaceModifierInstance stop | 18563 |
| 18606 | `0x27a4c0` | TESObjectCELL::GetCOCPlacementInfo | 19075 |
| 18949 | `0x28ccd0` | EventDispatcher::PushEvent | 19364 |
| 19263 | `0x29f110` | TESObjectREFR::RemoveItem | 19689 |
| 19276 | `0x29fac0` | TESObjectREFR::GetContainer | 19702 |
| 19282 | `0x29fdb0` | TESObjectREFR::AddObjectToContainer | 19708 |
| 19357 | `0x2a7ba0` | TESObjectREFR::GetWorldLocation | 19784 |
| 19418 | `0x2ad090` | TESObjectREFR::GetHandle | 19846 |
| 19798 | `0x2b8480` | GetLocationEncounterZone | 20203 |
| 19816 | `0x2b89e0` | TESObjectREFR::CreateLock | 20221 |
| 22754 | `0x332a90` | TESQuest::CompleteAllObjectives | 23231 |
| 24065 | `0x367980` | TESIdleForm property | 24568 |
| 24468 | `0x37f980` | TESQuest::SetStopped | 24987 |
| 24482 | `0x3803d0` | TESQuest::SetStage | 25004 |
| 25697 | `0x3c4970` | Sky::ReleaseWeatherOverride | 26244 |
| 26454 | `0x3eada0` | FaceGen CreateTints | 27040 |
| 26576 | `0x3f2ff0` | experience calculation | 27244 |
| 32525 | `0x510b20` | combat target selector sort predicate | 32525 |
| 32048 | `0x4fd1c0` | animation action helper | 32802 |
| 32049 | `0x4fd300` | animation action helper | 32803 |
| 32139 | `0x500890` | IAnimationGraphManagerHolder::RevertAnimationGraphManager | 32883 |
| 32488 | `0x50e480` | CombatController::SetTarget | 33235 |
| 32512 | `0x50ff00` | combat movement check | 33261 |
| 32528 | `0x510c90` | combat target array quick sort | 33285 |
| 33623 | `0x550540` | MagicCaster::CastSpell | 34401 |
| 33728 | `0x557070` | MagicTarget::DispelAllSpells | 34512 |
| 33741 | `0x557830` | MagicTarget::CheckAddEffectTargetData | 34525 |
| 33745 | `0x557f80` | MagicTarget::GetTargetAsActor | 34529 |
| 34193 | `0x569920` | SummonCreatureEffect start | 34989 |
| 34286 | `0x56e070` | ValueModifierEffect::ApplyActorEffect | 35086 |
| 34444 | `0x574bf0` | MenuTopicManager::PlayDialogueOption | 35269 |
| 35107 | `0x59f160` | BGSLoadFormBuffer::ReadFormId | 36000 |
| 35158 | `0x5a1070` | BGSSaveFormBuffer::WriteFormId | 36048 |
| 36525 | `0x5ef640` | Actor::AddObjectToContainer | 36525 |
| 36527 | `0x5efa10` | Actor::GetGoldAmount | 36527 |
| 36741 | `0x604f30` | Actor::GetDetectionState | 36741 |
| 36174 | `0x5d5120` | animation helper | 37147 |
| 36196 | `0x5d63a0` | Character dtor | 37175 |
| 36365 | `0x6226a0` | Actor::Process, the AI step skipped for remote actors (public database; 0x5e0e20 was the whole actor update and froze animations, 2026-09-20) | 37356 |
| 36511 | `0x5ee4f0` | TESObjectREFR::PayGoldToContainer | 37511 |
| 36521 | `0x5eeca0` | Actor::PickUpObject | 37521 |
| 36575 | `0x5f5280` | Actor::IsFleeing | 37577 |
| 36669 | `0x600220` | Actor::SetFactionRank | 37677 |
| 37943 | `0x640f30` | ActorEquipManager::UnequipAll | 38899 |
| 37980 | `0x643910` | EquipManager internal UnequipShout | 38935 |
| 37998 | `0x644070` | ActorMediator::PerformIdleAction | 38952 |
| 38005 | `0x644510` | AI package init | 38959 |
| 38046 | `0x645b10` | animation helper | 39002 |
| 38156 | `0x64c170` | AIProcess::CheckForNewPackage | 39114 |
| 38612 | `0x66dd50` | dialogue response processing | 39643 |
| 39382 | `0x6c00f0` | Actor::DropObject | 40454 |
| 39456 | `0x6c6e00` | PlayerCharacter::PickUpObject | 40533 |
| 39458 | `0x6c74d0` | PlayerCharacter::SetWaypoint | 40535 |
| 39459 | `0x6c7630` | PlayerCharacter::RemoveWaypoint | 40536 |
| 41626 | `0x743170` | Actor::UpdateDetectionState | 42704 |
| 45923 | `0x7e1ac0` | combat target validity check | 47196 |
| 46039 | `0x7e7e90` | Actor::HasEquippedRangedWeapon | 47303 |
| 46043 | `0x7e8020` | Actor world location check | 47307 |
| 51754 | `0x8f9ea0` | SubtitleManager::HideSubtitle | 52627 |
| 53115 | `0x95c670` | SkyrimVM::Update | 53926 |
| 53604 | `0x976690` | EventDispatcher::RegisterSink | 54425 |
| 54864 | `0x9ad8a0` | beast form change | 55497 |
| 56763 | `0xa052c0` | behavior symbol lookup | 57185 |
| 57804 | `0xa2e820` | hkbBehaviorGraph::HandleEvents | 58377 |
| 57805 | `0xa2ea40` | hkbBehaviorGraph event helper | 58378 |
| 58656 | `0xa4dca0` | behavior generator event | 59310 |
| 59405 | `0xa88950` | behavior graph update | 60079 |
| 62420 | `0xb1cfa0` | BSAnimationGraphManager send event | 63362 |
| 62430 | `0xb1d8b0` | BSAnimationGraphManager event helper | 63372 |
| 66964 | `0xc413f0` | CRC hash stub | 68221 |
| 67245 | `0xc4e600` | BSInputEnableManager::EnableOtherEvent | 68545 |
| 67823 | `0xc6dc90` | BSFixedString::Set | 69165 |
| 69269 | `0xcac090` | NiCamera::WorldPtToScreenPt3 | 70639 |
| 69335 | `0xcaef60` | FaceGen CreateTexture | 70717 |
| 74481 | `0xd8a900` | BSFaceGenNiNode::GetObjectByName | 76207 |
| 79937 | `0xf1a3b0` | UI::IsMenuOpen | 82074 |
| 79951 | `0xf1bf10` | UI close all menus | 82088 |
| ? | `0x1eb1d08` | combat detection time limit setting value | 382393 |
| ? | `0x1eb1d24` | combat recent LOS time limit setting value | 382400 |
| 514164 | `0x1f8319c` | GetInvalidRefHandle (from `database.csv`, confidence 2) | 400312 |
| ? | `0x2fc4880` | animation global | 401100 |
| 517060 | `0x2febcc0` | animation global (next to 403566/403567) | 403568 |
| ? | `0x2fff008` | animation global | 403988 |
| ? | `0x3010168` | combat setting value | 405282 |
| ? | `0x3422948` | script object handle policy | 414391 |
| 527752 | `0x3423e20` | NiMaskedShader NiRTTI | 414675 |

### From the SE/VR binary-diff table only

The least trusted: taken from a diff table that was wrong for every RTTI entry it was tested on.

| SE id | VR address | Used as | AE id in the source |
| --- | --- | --- | --- |
| ? | `0x95c430` | MapMenu hookLoc | 53112 |
