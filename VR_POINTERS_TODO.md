# VR pointers still to find

142 addresses that Skyrim Together still has no trustworthy Skyrim VR (1.4.15) address for.
Generated 2026-09-13. Everything **not** in this file is either already resolved or verified,
so nothing here duplicates earlier work.

## Read this first: the ids below are AE ids, not SE ids

Every id in this list (and in the Skyrim Together source) is an **Anniversary Edition (1.6.x)**
Address Library id. Looking one up in the **SE 1.5.97** table gives the address of a
*different* function, because the two games number their functions differently.

Example, id 32883 (`IAnimationGraphManagerHolder::InternalRevertAnimationGraphManager`):

| | Address | What it actually is |
| --- | --- | --- |
| SE id 32883 (wrong lookup) | SE `0x14051EF20` → VR `0x14052F1E0` | some other function |
| AE id 32883 → **SE id 32139** (right) | SE `0x1404F0620` → **VR `0x140500890`** | InternalRevertAnimationGraphManager |

The earlier `SetGraphVariableFloat` result (VR `0x14052F2D0`) had the same problem. The real one
is SE id 32143 → VR `0x140500990`, which the official VR Address Library confirms.

**32883 is now done** (VR `0x140500890`), so it is not in the list below.

## How to find and confirm one

1. Take the **AE address** column (AE 1.6.318). If you have an AE binary, that is the function.
   Otherwise, use the name/purpose column and the search window.
2. **Search window:** the two nearest neighbours with known answers are given as
   `AE id = SE id = VR address`. Address Library ids are roughly in address order, so the target
   usually sits **between those two VR addresses** in the same order as in SE/AE.
3. **Confirm with function sizes.** The neighbouring functions in VR should have the same sizes
   and order as in SE (e.g. for 32883, the three functions before it are `0x80`, `0xd0`, `0x90`
   bytes in both AE and SE, and VR repeats the same pattern). This check was right 99.5% of the
   time on 1958 known answers.
4. "Candidate" rows already have a guess from that method, but with too few matching sizes to
   trust. A quick look in Ghidra is enough to confirm or reject them.

When you confirm one, send **AE id → SE id → VR address**.

Kinds: **HOOK** = Skyrim Together detours this function (currently switched off on VR, so that
feature doesn't sync). **patch/direct** = bytes are patched at, or read from, this address.
**call** = Skyrim Together calls it directly.


## Priority 1: hooks (40), needed for syncing

| AE id | Used as (file) | Kind | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 14375 | `s_SetLeveledNpc` (Games/Skyrim/Forms/TESNPC.cpp:18) | HOOK | 0x140196810 | - | AE 14299 = SE 14190 = VR 0x199cf0 | AE 14383 = SE 14261 = VR 0x19c3b0 |
| 19512 | `s_lockChange` (Games/Skyrim/TESObjectREFR.cpp:1109) | HOOK | 0x140297850 | SE 19110 → VR 0x297310 (fp 2/5) | AE 19509 = SE 19107 = VR 0x297060 | AE 19517 = SE 19115 = VR 0x297860 |
| 19708 | `s_addInventoryItem` (Games/Skyrim/TESObjectREFR.cpp:1114) | HOOK | 0x1402A0930 | - | AE 19703 = SE 19277 = VR 0x29faf0 | AE 19728 = SE 19301 = VR 0x2a1c60 |
| 21622 | `hasPerk` (Games/Skyrim/Magic/MagicTarget.cpp:234) | HOOK | 0x1402F1260 | - | - | AE 21910 = SE 21425 = VR 0x2f9900 |
| 27244 | `s_calculateExperience` (Games/Skyrim/PlayerCharacter.cpp:251) | HOOK | 0x1403FC280 | - | AE 27203 = SE 26570 = VR 0x3f0e00 | AE 27263 = SE 26592 = VR 0x3f4420 |
| 33236 | `s_updateTarget` (Games/Skyrim/Combat/CombatController.cpp:81) | HOOK | 0x1405177F0 | - | AE 33159 = SE 32426 = VR 0x50c160 | AE 33271 = SE 32519 = VR 0x510520 |
| 34053 | `adjustForPerks` (Games/Skyrim/Magic/MagicTarget.cpp:233) | HOOK | 0x140557F30 | - | AE 33917 = SE 33138 = VR 0x9257f0 | AE 34057 = SE 33282 = VR 0x540ea0 |
| 34140 | `s_interruptCast` (Games/Skyrim/Magic/ActorMagicCaster.cpp:63) | HOOK | 0x14055C700 | - | AE 34130 = SE 33348 = VR 0x5494f0 | AE 34144 = SE 33363 = VR 0x546260 |
| 34370 | `s_finish` (Games/Skyrim/Effects/InvisibilityEffect.cpp:27) | HOOK | 0x1405661D0 | - | AE 34343 = SE 33561 = VR 0x54d8b0 | AE 34400 = SE 33622 = VR 0x5503e0 |
| 34989 | `s_start` (Games/Skyrim/Effects/SummonCreatureEffect.h:30) | HOOK | 0x140580040 | SE 34193 → VR 0x569920 (fp 2/5) | AE 34988 = SE 34192 = VR 0x5698d0 | AE 34990 = SE 34194 = VR 0x5699e0 |
| 35086 | `s_applyActorEffect` (Games/Skyrim/Actor.cpp:1280) | HOOK | 0x1405845F0 | - | AE 35084 = SE 34284 = VR 0x56de70 | AE 35095 = SE 34292 = VR 0x49fde0 |
| 35269 | `s_playDialogueOption` (Games/Misc/MenuTopicManager.cpp:40) | HOOK | 0x14058B5D0 | - | AE 35220 = SE 34413 = VR 0x572fd0 | AE 35287 = SE 34460 = VR 0x5766f0 |
| 36291 | `s_SimulateTime` (Games/TimeManager.cpp:25) | HOOK | 0x1405C7220 | - | AE 36215 = SE 35320 = VR 0x5aa320 | AE 36311 = SE 35413 = VR 0x5add00 |
| 36564 | `cMainLoop` (SkyrimVM64.cpp:48) | HOOK | 0x1405D9F50 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 37313 | `s_ForceState` (Games/Skyrim/Actor.cpp:1277) | HOOK | 0x1405F8860 | - | AE 37275 = SE 36286 = VR 0x5daae0 | AE 37334 = SE 36344 = VR 0x5de910 |
| 37356 | `s_actorProcess` (Games/Skyrim/Actor.cpp:1270) | HOOK | 0x1405FD7E0 | - | AE 37335 = SE 36345 = VR 0x5de930 | AE 37383 = SE 36392 = VR 0x5e2ec0 |
| 37521 | `s_pickUpObject` (Games/Skyrim/Actor.cpp:1283) | HOOK | 0x14060C510 | - | AE 37491 = SE 36492 = VR 0x5eba50 | AE 37532 = SE 36532 = VR 0x5efc20 |
| 37542 | `s_speakSoundFunction` (Games/Skyrim/Actor.cpp:1289) | HOOK | 0x14060E8E0 | - | AE 37537 = SE 36537 = VR 0x5f0560 | AE 37560 = SE 36559 = VR 0x5f44a0 |
| 37577 | `isFleeing` (Games/Skyrim/Actor.cpp:1291) | HOOK | 0x140612EF0 | - | AE 37560 = SE 36559 = VR 0x5f44a0 | AE 37610 = SE 36602 = VR 0x5f6460 |
| 37905 | `s_initiateMountPackage` (Games/Skyrim/Actor.cpp:1287) | HOOK | 0x14062D1D0 | SE 36881 → VR 0x60e300 (fp 2/5) | AE 37904 = SE 36880 = VR 0x60e130 | AE 37907 = SE 36883 = VR 0x60ee10 |
| 38928 | `s_equipSpellFunc` (Games/Skyrim/EquipManager.cpp:313) | HOOK | 0x14065F9C0 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38929 | `s_equipFunc` (Games/Skyrim/EquipManager.cpp:311) | HOOK | 0x14065FD20 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38930 | `s_equipShoutFunc` (Games/Skyrim/EquipManager.cpp:315) | HOOK | 0x140660080 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38933 | `s_unequipSpellFunc` (Games/Skyrim/EquipManager.cpp:314) | HOOK | 0x140660410 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38934 | `s_unequipFunc` (Games/Skyrim/EquipManager.cpp:312) | HOOK | 0x140660700 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38935 | `s_unequipShoutFunc` (Games/Skyrim/EquipManager.cpp:316) | HOOK | 0x140660950 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38949 | `performAction` (Games/Animation.cpp:186) | HOOK | 0x140660FC0 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38959 | `s_initFromPackage` (Games/References.cpp:137) | HOOK | 0x140661820 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 39643 | `s_processResponse` (Games/Skyrim/Actor.cpp:1286) | HOOK | 0x14068BEE0 | SE 38612 → VR 0x66dd50 (fp 1/5) | AE 39642 = SE 38611 = VR 0x66d9a0 | AE 39647 = SE 38616 = VR 0x670830 |
| 40245 | `s_characterCtor` (Games/Skyrim/Actor.cpp:1273) | HOOK | 0x1406BA510 | SE 39171 → VR 0x69bec0 (fp 2/5) | AE 40244 = SE 39170 = VR 0x69be20 | AE 40249 = SE 39175 = VR 0x69c2d0 |
| 40246 | `s_characterCtor2` (Games/Skyrim/Actor.cpp:1274) | HOOK | 0x1406BA610 | SE 39172 → VR 0x69bfc0 (fp 2/5) | AE 40244 = SE 39170 = VR 0x69be20 | AE 40249 = SE 39175 = VR 0x69c2d0 |
| 40412 | `cVMDestructor` (SkyrimVM64.cpp:50) | HOOK | 0x1406C19F0 | SE 39341 → VR 0x6a3a30 (fp 2/5) | AE 40411 = SE 39340 = VR 0x6a26a0 | AE 40414 = SE 39343 = VR 0x6bae40 |
| 40533 | `s_pickUpObject` (Games/Skyrim/PlayerCharacter.cpp:248) | HOOK | 0x1406CD770 | - | AE 40501 = SE 39425 = VR 0x6c38a0 | AE 40548 = SE 39471 = VR 0x6cbce0 |
| 40535 | `s_setWaypoint` (Games/Skyrim/PlayerCharacter.cpp:252) | HOOK | 0x1406CDE40 | - | AE 40501 = SE 39425 = VR 0x6c38a0 | AE 40548 = SE 39471 = VR 0x6cbce0 |
| 40536 | `s_removeWaypoint` (Games/Skyrim/PlayerCharacter.cpp:253) | HOOK | 0x1406CDFA0 | - | AE 40501 = SE 39425 = VR 0x6c38a0 | AE 40548 = SE 39471 = VR 0x6cbce0 |
| 52626 | `s_showSubtitle` (Games/Misc/SubtitleManager.cpp:69) | HOOK | 0x1408FBD50 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 55497 | `s_setBeastForm` (Games/Skyrim/PlayerCharacter.cpp:249) | HOOK | 0x14099C360 | - | AE 55495 = SE 54862 = VR 0x76b780 | AE 55506 = SE 54939 = VR 0x9b3800 |
| 104359 | `s_signaturesMatch` (Games/Misc/BSScript.cpp:100) | HOOK | 0x141366590 | - | AE 104321 = SE 97536 = VR 0x12708c0 | AE 104434 = SE 97692 = VR 0x12a0c50 |
| 104788 | `s_registerPapyrusFunction` (Games/Misc/BSScript.cpp:98) | HOOK | 0x141381B20 | SE 98065 → VR 0x1278410 (fp 2/5) | AE 104781 = SE 98058 = VR 0x1277cb0 | AE 104793 = SE 98070 = VR 0x1279010 |
| 105220 | `s_compareVariables` (Games/Misc/BSScript.cpp:102) | HOOK | 0x1413A5A20 | - | AE 105185 = SE 98529 = VR 0x12b2ab0 | AE 105267 = SE 98613 = VR 0x12c7fb0 |

## Priority 2: patches / engine plumbing (17)

| AE id | Used as (file) | Kind | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 14774 | `getGenerator` (Games/Skyrim/BSRandom/BSRandom.cpp:29) | patch/direct | 0x1401A4080 | SE 14599 → VR 0x1a8cd0 (fp 2/5) | AE 14763 = SE 14588 = VR 0x1a8670 | AE 14775 = SE 14600 = VR 0x1a8d80 |
| 34452 | `hookLoc` (Games/Skyrim/Projectiles/Projectile.cpp:159) | patch/direct | 0x14056B8F0 | SE 33672 → VR 0x554980 (fp 3/5) | AE 34451 = SE 33671 = VR 0x5547a0 | AE 34456 = SE 33676 = VR 0x555020 |
| 36548 | `MainInit` (Games/Skyrim/Interface/UI.cpp:146) | patch/direct | 0x1405D52E0 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 51538 | `FavoritesCanProcess` (Games/Skyrim/Interface/UI.cpp:153) | patch/direct | 0x1408A6B40 | SE 50644 → VR 0x8a3ee0 (fp 3/5) | AE 51537 = SE 50643 = VR 0x8a3e60 | AE 51539 = SE 50645 = VR 0x8a3f30 |
| 52510 | `ProcessMessage` (Games/Skyrim/Interface/Menus/SkillsMenu.cpp:18) | patch/direct | 0x1408EE960 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 52518 | `controlPatch` (Games/Skyrim/Interface/Menus/SkillsMenu.cpp:26) | patch/direct | 0x1408F0280 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 53112 | `hookLoc` (Games/Skyrim/Interface/Menus/MapMenu.cpp:13) | patch/direct | 0x140916110 | - | AE 52940 = SE 52055 = VR 0x90b240 | AE 53113 = SE 52226 = VR 0x919520 |
| 57704 | `createHavokThread` (Games/Skyrim/BSSystem/BSThread.cpp:100) | patch/direct | 0x1409FBEF0 | - | - | - |
| 68261 | `threadInit` (Games/Skyrim/BSSystem/BSThread.cpp:79) | patch/direct | 0x140C2CD40 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |
| 68276 | `unsignedInt` (Games/Skyrim/BSRandom/BSRandom.cpp:23) | patch/direct | 0x140C2D180 | - | AE 68245 = SE 66988 = VR 0xc42730 | AE 68449 = SE 67151 = VR 0xc485e0 |
| 68617 | `pollInputDevices` (Games/Skyrim/BSInput/BSInputDeviceManager.cpp:25) | patch/direct | 0x140C3B360 | - | AE 68553 = SE 67253 = VR 0xc4e900 | AE 68618 = SE 67316 = VR 0xc51ac0 |
| 68781 | `windowLoc` (Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp:101) | patch/direct | 0x140C402C0 | - | AE 68762 = SE 67457 = VR 0xc5e2a0 | AE 69161 = SE 67819 = VR 0xc6db20 |
| 69066 | `setThreadName` (Games/Skyrim/BSSystem/BSThread.cpp:88) | patch/direct | 0x140C4BC50 | - | AE 68762 = SE 67457 = VR 0xc5e2a0 | AE 69161 = SE 67819 = VR 0xc6db20 |
| 69554 | `getTaskletManagerInstance` (Games/Skyrim/BSSystem/BSTaskletManager.cpp:33) | patch/direct | 0x140C60EE0 | - | AE 69200 = SE 67855 = VR 0xc6f400 | AE 69558 = SE 68207 = VR 0xc7eb70 |
| 77226 | `initWindowLoc` (Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp:99) | patch/direct | 0x140DA3850 | - | AE 76844 = SE 75076 = VR 0xda4be0 | AE 77228 = SE 75447 = VR 0xdbabc0 |
| 77246 | `timerLoc` (Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp:105) | patch/direct | 0x140DA5BE0 | SE 75461 → VR 0xdbbdd0 (fp 1/5) | AE 77245 = SE 75460 = VR 0xdbbb80 | AE 77247 = SE 75462 = VR 0xdbbe60 |
| 82082 | `ProcessHook` (Games/Skyrim/Interface/UI.cpp:134) | patch/direct | 0x140F04C90 | - | AE 82081 = SE 79944 = VR 0xf1a680 | AE 82167 = SE 80059 = VR 0xf1fbc0 |

Several of these currently work through old values (WinMain 36544, BSThread 68261/69066/57704). Confirm them, but they are not blocking.

## Priority 3: direct calls (79)

| AE id | Used as (file) | Kind | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 11612 | `setWornData` (Games/Skyrim/ExtraData/ExtraDataList.cpp:112) | call | 0x14011A5D0 | - | AE 11600 = SE 11483 = VR 0x11fba0 | AE 11617 = SE 11471 = VR 0x11eb50 |
| 11616 | `setHealth` (Games/Skyrim/ExtraData/ExtraDataList.cpp:126) | call | 0x14011AB30 | - | AE 11600 = SE 11483 = VR 0x11fba0 | AE 11617 = SE 11471 = VR 0x11eb50 |
| 11619 | `setChargeData` (Games/Skyrim/ExtraData/ExtraDataList.cpp:104) | call | 0x14011AE60 | SE 11473 → VR 0x11ede0 (fp 0/5) | AE 11617 = SE 11471 = VR 0x11eb50 | AE 11637 = SE 11491 = VR 0x1205e0 |
| 11620 | `setSoulData` (Games/Skyrim/ExtraData/ExtraDataList.cpp:97) | call | 0x14011AF60 | SE 11474 → VR 0x11ef40 (fp 0/5) | AE 11617 = SE 11471 = VR 0x11eb50 | AE 11637 = SE 11491 = VR 0x1205e0 |
| 11822 | `setPoison` (Games/Skyrim/ExtraData/ExtraDataList.cpp:119) | call | 0x140124160 | - | AE 11812 = SE 11666 = VR 0x129e70 | AE 11953 = SE 11813 = VR 0x131900 |
| 12060 | `setEnchantmentData` (Games/Skyrim/ExtraData/ExtraDataList.cpp:133) | call | 0x14012D670 | SE 11921 → VR 0x1372b0 (fp 0/5) | AE 12056 = SE 11917 = VR 0x136f60 | AE 12065 = SE 11926 = VR 0x1375f0 |
| 12401 | `realSetLock` (Games/Misc/Lock.cpp:6) | call | 0x14013B180 | - | AE 12399 = SE 12272 = VR 0x145380 | AE 12432 = SE 12298 = VR 0x1459b0 |
| 13718 | `getCell` (Games/ModManager.cpp:68) | call | 0x140175940 | SE 13620 → VR 0x17c4e0 (fp 1/5) | AE 13716 = SE 13618 = VR 0x17c000 | AE 13723 = SE 13625 = VR 0x17cc30 |
| 14529 | `s_getItemCount` (Games/Skyrim/Components/TESContainer.cpp:7) | call | 0x14019A3C0 | - | AE 14384 = SE 14262 = VR 0x19c480 | AE 14543 = SE 14390 = VR 0x19fda0 |
| 14953 | `s_constructor` (Games/Skyrim/Components/TESTexture.cpp:9) | call | 0x1401AC180 | - | AE 14875 = SE 14704 = VR 0x1ad250 | AE 14988 = SE 14809 = VR 0x1b1470 |
| 15002 | `sub_1401A2220` (Games/Skyrim/AnimationExperiments.cpp:378) | call | 0x1401AD5A0 | - | AE 14988 = SE 14809 = VR 0x1b1470 | AE 15203 = SE 15029 = VR 0x1bd040 |
| 15006 | `sub_1401A1D50` (Games/Skyrim/AnimationExperiments.cpp:412) | call | 0x1401AD850 | - | AE 14988 = SE 14809 = VR 0x1b1470 | AE 15203 = SE 15029 = VR 0x1bd040 |
| 16113 | `s_getArmor` (Games/Skyrim/ExtraData/ExtraContainerChanges.cpp:36) | call | 0x1401F2E40 | SE 15873 → VR none (fp 4/5) | AE 16106 = SE 15866 = VR 0x1f7250 | AE 16123 = SE 15883 = VR 0x1f9ca0 |
| 18563 | `s_clearImageSpaceModifier` (Games/Skyrim/NetImmerse/ImageSpaceModifierInstance.h:24) | call | 0x14025C1E0 | - | AE 18503 = SE 18109 = VR 0x25b040 | AE 18568 = SE 18183 = VR 0x25dcf0 |
| 19075 | `s_getCOCPlacementInfo` (Games/Skyrim/Forms/TESObjectCELL.cpp:30) | call | 0x14027BEE0 | SE 18606 → VR 0x27a4c0 (fp 2/5) | AE 19068 = SE 18599 = VR 0x279a30 | AE 19078 = SE 18609 = VR 0x27a7f0 |
| 19364 | `s_pushEvent` (Games/Skyrim/Events/EventDispacther.cpp:32) | call | 0x14028D170 | - | AE 19298 = SE 18862 = VR 0x28af80 | AE 19434 = SE 19021 = VR 0x291680 |
| 19846 | `s_getHandle` (Games/Skyrim/TESObjectREFR.cpp:130) | call | 0x1402AE020 | - | AE 19827 = SE 19400 = VR 0x2aba40 | AE 19872 = SE 19446 = VR 0x2ae290 |
| 20460 | `s_loadCell` (Games/Skyrim/Forms/TESWorldSpace.cpp:6) | call | 0x1402C4F30 | - | AE 20456 = SE 20022 = VR 0x2c32d0 | AE 20543 = SE 20095 = VR 0x2c7b20 |
| 21600 | `isFirstPerson` (Games/Skyrim/Camera/PlayerCamera.cpp:15) | call | 0x1402F0740 | - | - | AE 21910 = SE 21425 = VR 0x2f9900 |
| 24987 | `SetStopped` (Games/Skyrim/Forms/TESQuest.cpp:69) | call | 0x1403876C0 | - | AE 24959 = SE 24709 = VR 0x35bf10 | AE 24988 = SE 24469 = VR 0x37f9c0 |
| 25004 | `SetStage` (Games/Skyrim/Forms/TESQuest.cpp:96) | call | 0x1403880F0 | - | AE 25003 = SE 24481 = VR 0x3802b0 | AE 25014 = SE 24486 = VR 0x380830 |
| 26503 | `s_getDifficultyMultiplier` (Games/References.cpp:56) | call | 0x1403D8C80 | - | AE 26424 = SE 25858 = VR 0x3ce200 | AE 26586 = SE 25977 = VR 0x3d3160 |
| 27040 | `CreateTints` (Systems/FaceGenSystem.cpp:64) | call | 0x1403F3750 | - | AE 27010 = SE 26431 = VR 0x3e9680 | AE 27110 = SE 26509 = VR 0x3ee610 |
| 32802 | `sub_1404ECF50` (Games/Skyrim/AnimationExperiments.cpp:338) | call | 0x140505AD0 | - | AE 32486 = SE 31718 = VR 0x4e8240 | AE 32884 = SE 32192 = VR 0x502890 |
| 32803 | `sub_1404ED090` (Games/Skyrim/AnimationExperiments.cpp:377) | call | 0x140505C10 | - | AE 32486 = SE 31718 = VR 0x4e8240 | AE 32884 = SE 32192 = VR 0x502890 |
| 33235 | `setTarget` (Games/Skyrim/Combat/CombatController.cpp:73) | call | 0x1405174F0 | - | AE 33159 = SE 32426 = VR 0x50c160 | AE 33271 = SE 32519 = VR 0x510520 |
| 33285 | `arrayQuickSort` (Games/Skyrim/Combat/CombatController.cpp:8) | call | 0x140519EA0 | SE 32528 → VR 0x510c90 (fp 2/5) | AE 33284 = SE 32527 = VR 0x510bb0 | AE 33286 = SE 32529 = VR 0x510d20 |
| 34401 | `s_castSpell` (Games/Skyrim/Magic/MagicCaster.cpp:7) | call | 0x1405672B0 | SE 33623 → VR 0x550540 (fp 1/5) | AE 34400 = SE 33622 = VR 0x5503e0 | AE 34408 = SE 33630 = VR 0x550d40 |
| 35503 | `internalGetChangeFlags` (Games/Forms.cpp:102) | call | 0x140598A90 | - | AE 35417 = SE 34561 = VR 0x57cf80 | AE 35505 = SE 34584 = VR 0x57f780 |
| 35993 | `ctor` (Games/SaveLoad.cpp:49) | call | 0x1405B8AC0 | - | AE 35991 = SE 35099 = VR 0x59ed50 | AE 35996 = SE 35103 = VR 0x59f000 |
| 36544 | `winMain` (TiltedOnlineApp.cpp:49) | call | 0x1405D29F0 | - | AE 36459 = SE 35492 = VR 0x5b1710 | AE 36604 = SE 51246 = VR 0x8d0500 |
| 37147 | `sub_1405CCB20` (Games/Skyrim/AnimationExperiments.cpp:337) | call | 0x1405F0C20 | - | AE 36985 = SE 36010 = VR 0x5cef50 | AE 37198 = SE 36218 = VR 0x5d80a0 |
| 37175 | `s_characterDtor` (Games/Skyrim/Actor.cpp:1275) | call | 0x1405F2330 | - | AE 36985 = SE 36010 = VR 0x5cef50 | AE 37198 = SE 36218 = VR 0x5d80a0 |
| 37511 | `s_payGoldToContainer` (Games/Skyrim/TESObjectREFR.cpp:376) | call | 0x14060BD60 | - | AE 37491 = SE 36492 = VR 0x5eba50 | AE 37532 = SE 36532 = VR 0x5efc20 |
| 37717 | `setPlayerTeammate` (Games/Skyrim/Actor.cpp:776) | call | 0x1406215A0 | - | AE 37698 = SE 36690 = VR 0x6025a0 | AE 37735 = SE 36723 = VR 0x603e30 |
| 38533 | `s_setNoBleedoutRecovery` (Games/Skyrim/Actor.cpp:308) | call | 0x1406490C0 | - | AE 38522 = SE 37573 = VR 0x62c610 | AE 38538 = SE 37588 = VR 0x62ccc0 |
| 38899 | `s_unequipAll` (Games/Skyrim/EquipManager.cpp:137) | call | 0x14065DCA0 | SE 37943 → VR 0x640f30 (fp 2/5) | AE 38897 = SE 37941 = VR 0x640cb0 | AE 38900 = SE 37944 = VR 0x641010 |
| 38952 | `PerformIdleAction` (Games/Skyrim/AnimationExperiments.cpp:286) | call | 0x140661300 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38953 | `PerformComplexAction` (Games/Animation.cpp:111) | call | 0x140661390 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 38979 | `setWeaponState` (Games/Skyrim/Misc/ActorState.cpp:6) | call | 0x1406627C0 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 39002 | `sub_14063CAA0` (Games/Skyrim/AnimationExperiments.cpp:335) | call | 0x1406630A0 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 39004 | `ApplyAnimationVariables` (Games/Animation.cpp:110) | call | 0x140663650 | - | AE 38924 = SE 37968 = VR 0x642910 | AE 39033 = SE 38079 = VR 0x6488a0 |
| 52627 | `s_hideSubtitle` (Games/Misc/SubtitleManager.cpp:39) | call | 0x1408FBF50 | - | AE 52490 = SE 51618 = VR 0x8ead70 | AE 52628 = SE 51755 = VR 0x8fa0a0 |
| 52847 | `fadeOutGame` (Games/References.cpp:77) | call | 0x140905B30 | SE 51909 → VR 0x903080 (fp 1/5) | AE 52845 = SE 51907 = VR 0x902f20 | AE 52849 = SE 51911 = VR 0x903370 |
| 54425 | `s_registerSink` (Games/Skyrim/Events/EventDispacther.cpp:12) | call | 0x140968B90 | - | AE 54351 = SE 53494 = VR 0x973f30 | AE 54483 = SE 53659 = VR 0x97a610 |
| 57185 | `sub_1409CA9D0` (Games/Skyrim/AnimationExperiments.cpp:82) | call | 0x1409EEF70 | - | AE 57036 = SE 56631 = VR 0xa01c90 | AE 57231 = SE 56801 = VR 0xa063b0 |
| 58377 | `HandleEvents` (Games/Skyrim/AnimationExperiments.cpp:44) | call | 0x140A184E0 | - | - | - |
| 58378 | `sub_1409F3EF0` (Games/Skyrim/AnimationExperiments.cpp:119) | call | 0x140A18700 | - | - | - |
| 59310 | `sub_140A13150` (Games/Skyrim/AnimationExperiments.cpp:96) | call | 0x140A37960 | - | - | - |
| 60079 | `sub_140A4DFA0` (Games/Skyrim/AnimationExperiments.cpp:128) | call | 0x140A727B0 | - | - | AE 60338 = SE 59653 = VR 0xa96820 |
| 63362 | `BSSendEvent` (Games/Skyrim/AnimationExperiments.cpp:240) | call | 0x140B06E80 | - | - | AE 63607 = SE 62710 = VR 0xb320f0 |
| 63372 | `sub_140AE2DB0` (Games/Skyrim/AnimationExperiments.cpp:227) | call | 0x140B077F0 | - | - | AE 63607 = SE 62710 = VR 0xb320f0 |
| 63591 | `InternalSendEvent` (Games/Skyrim/AnimationExperiments.cpp:207) | call | 0x140B15870 | - | - | AE 63607 = SE 62710 = VR 0xb320f0 |
| 68221 | `hash_stub` (Games/Skyrim/AnimationExperiments.cpp:165) | call | 0x140C2B290 | - | AE 68146 = SE 66885 = VR 0xc3e940 | AE 68233 = SE 66976 = VR 0xc42150 |
| 70639 | `s_w2s` (Games/Skyrim/NetImmerse/NiCamera.cpp:10) | call | 0x140C8E580 | - | AE 70630 = SE 69263 = VR 0xcab2c0 | AE 70640 = SE 69270 = VR 0xcac0e0 |
| 70717 | `CreateTexture` (Systems/FaceGenSystem.cpp:62) | call | 0x140C91110 | - | AE 70640 = SE 69270 = VR 0xcac0e0 | AE 70810 = SE 69433 = VR 0xcb2ee0 |
| 76207 | `GetObjectByName` (Systems/FaceGenSystem.cpp:25) | call | 0x140D79A80 | - | AE 76122 = SE 74398 = VR 0xd86d70 | AE 76436 = SE 74702 = VR 0xd956c0 |
| 82074 | `s_isMenuOpen` (Games/Skyrim/Interface/UI.cpp:23) | call | 0x140F044A0 | - | - | AE 82081 = SE 79944 = VR 0xf1a680 |
| 82088 | `s_CloseAll` (Games/Skyrim/Interface/UI.cpp:31) | call | 0x140F06320 | - | AE 82081 = SE 79944 = VR 0xf1a680 | AE 82167 = SE 80059 = VR 0xf1fbc0 |
| 104296 | `s_reset` (Games/Skyrim/Misc/BSScript.cpp:24) | call | 0x1413613E0 | - | AE 104294 = SE 97506 = VR 0x126e940 | AE 104299 = SE 97514 = VR 0x126f6f0 |
| 104653 | `ctor` (Games/Skyrim/Misc/BSScript.h:281) | call | 0x141379C20 | - | AE 104651 = SE 97923 = VR 0x129dd90 | AE 104665 = SE 97935 = VR 0x12abde0 |
| 104655 | `dtor` (Games/Skyrim/Misc/BSScript.h:274) | call | 0x141379E70 | - | AE 104651 = SE 97923 = VR 0x129dd90 | AE 104665 = SE 97935 = VR 0x12abde0 |
| 370892 | `s_greetDistance` (Games/References.cpp:44) | call | 0x141E6DA68 | - | - | - |
| 380768 | `bAlwaysActive` (TiltedOnlineApp.cpp:92) | call | 0x141E83088 | - | - | - |
| 381472 | `s_difficulty` (Games/References.cpp:38) | call | 0x141E84A28 | - | - | - |
| 400188 | `s_gameHeap` (Games/Memory.cpp:15) | call | 0x141F57A80 | - | - | AE 400267 = SE 514139 = VR 0x1f82ac8 |
| 400312 | `s_nullHandle` (Games/Skyrim/TESObjectREFR.cpp:140) | call | 0x141F592BC | - | AE 400269 = SE 514141 = VR 0x1f82ad8 | AE 400315 = SE 514167 = VR 0x1f831b0 |
| 400441 | `tes` (Games/TES.cpp:7) | call | 0x141F5B250 | - | AE 400331 = SE 514182 = VR 0x1f83220 | AE 400443 = SE 514283 = VR 0x1f850e8 |
| 401069 | `s_character` (Games/Skyrim/PlayerCharacter.cpp:45) | call | 0x142F99F90 | - | AE 401037 = SE 514893 = VR 0x2fc4658 | AE 401099 = SE 514959 = VR 0x2fc4878 |
| 401100 | `qword_142EFF990` (Games/Skyrim/AnimationExperiments.cpp:341) | call | 0x142F9A0A0 | - | AE 401099 = SE 514959 = VR 0x2fc4878 | AE 401203 = SE 515064 = VR 0x2fc4fe0 |
| 403566 | `qword_142F271B8` (Games/Animation.cpp:112) | call | 0x142FC1C88 | - | AE 403560 = SE 517052 = VR 0x2fea720 | AE 403759 = SE 517228 = VR 0x2ffd778 |
| 403567 | `s_actorMediator` (Games/Animation.cpp:69) | call | 0x142FC1C90 | - | AE 403560 = SE 517052 = VR 0x2fea720 | AE 403759 = SE 517228 = VR 0x2ffd778 |
| 403568 | `qword_142F271C8` (Games/Skyrim/AnimationExperiments.cpp:340) | call | 0x142FC1C98 | - | AE 403560 = SE 517052 = VR 0x2fea720 | AE 403759 = SE 517228 = VR 0x2ffd778 |
| 403988 | `qword_142F3A1E8` (Games/Skyrim/AnimationExperiments.cpp:381) | call | 0x142FD4EB8 | - | AE 403902 = SE 517372 = VR 0x2ffe470 | AE 404238 = SE 517711 = VR 0x2fffdea |
| 404125 | `s_value` (Games/Skyrim/AI/AITimer.h:7) | call | 0x142FD530C | - | AE 403902 = SE 517372 = VR 0x2ffe470 | AE 404238 = SE 517711 = VR 0x2fffdea |
| 406126 | `s_matrix` (Games/Skyrim/Interface/Menus/HUDMenuUtils.cpp:19) | call | 0x142FE75F0 | - | AE 406113 = SE 519572 = VR 0x3011cf0 | AE 406167 = SE 519620 = VR 0x3013408 |
| 406160 | `s_port` (Games/Skyrim/Interface/Menus/HUDMenuUtils.cpp:20) | call | 0x142FE8B98 | - | AE 406113 = SE 519572 = VR 0x3011cf0 | AE 406167 = SE 519620 = VR 0x3013408 |
| 414391 | `s_policy` (Games/Skyrim/Misc/BSScript.cpp:108) | call | 0x14326A930 | - | - | AE 414660 = SE 527731 = VR 0x34234c0 |
| 414675 | `NiMaskedShaderRTTI` (Systems/FaceGenSystem.cpp:61) | call | 0x14326BBB0 | - | AE 414660 = SE 527731 = VR 0x34234c0 | AE 414916 = SE 527970 = VR 0x3485630 |

## Priority 4: debug views only (6)

| AE id | Used as (file) | Kind | AE 1.6.318 address | Candidate | Known neighbour below | Known neighbour above |
| --- | --- | --- | --- | --- | --- | --- |
| 18518 | `getModifiedDistance` (Services/Debug/Views/CombatView.cpp:84) | call | 0x140259D50 | - | AE 18503 = SE 18109 = VR 0x25b040 | AE 18568 = SE 18183 = VR 0x25dcf0 |
| 33261 | `checkMovement` (Services/Debug/Views/CombatView.cpp:97) | call | 0x140518F90 | - | AE 33159 = SE 32426 = VR 0x50c160 | AE 33271 = SE 32519 = VR 0x510520 |
| 47196 | `isValidTarget` (Services/Debug/Views/CombatView.cpp:44) | call | 0x1407E1700 | - | AE 47011 = SE 45702 = VR 0x7d7d40 | AE 47286 = SE 46022 = VR 0x7e6220 |
| 382393 | `s_value` (Services/Debug/Views/CombatView.cpp:57) | call | 0x141E87400 | - | - | AE 382701 = SE 509927 = VR 0x1eb2720 |
| 382400 | `s_value` (Services/Debug/Views/CombatView.cpp:63) | call | 0x141E8741C | - | - | AE 382701 = SE 509927 = VR 0x1eb2720 |
| 405282 | `s_value` (Services/Debug/Views/CombatView.cpp:90) | call | 0x142FE5B78 | - | AE 405246 = SE 518706 = VR 0x3010038 | - |
