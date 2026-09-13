#include "Projectile.h"
#include <Games/Skyrim/Forms/TESObjectWEAP.h>
#include <Games/Skyrim/Forms/MagicItem.h>
#include <Games/Skyrim/Forms/TESAmmo.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <World.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Forms/SpellItem.h>

TP_THIS_FUNCTION(TLaunch, BSPointerHandle<Projectile>*, BSPointerHandle<Projectile>, Projectile::LaunchData& arData);
static TLaunch* RealLaunch = nullptr;

// LaunchData's layout past Origin isn't verified on VR (see the "seemingly
// always null" / "idk if it matters" notes in Projectile.h) - a struct offset
// mismatch means these pointers read back non-null but garbage, consistently
// the same garbage every run (0x0000003400000033), which points at a real
// layout shift on VR rather than random uninitialized memory. A __try/__except
// guard here reliably stopped the access violation itself, but consistently
// left the *following* function epilogue's /GS stack cookie corrupted,
// causing an unrecoverable STATUS_STACK_BUFFER_OVERRUN right after - SEH
// recovery interacts badly with this heavily-inlined, custom-loaded hook
// context. Until the real VR LaunchData layout is reverse-engineered, just
// don't read these fields on VR at all: no exception to recover from means
// no follow-up corruption. Event fields are left at their zero defaults.
static void ReadLaunchDataFormIds(const Projectile::LaunchData& arData, ProjectileLaunchedEvent& aEvent)
{
#ifndef SKYRIMVR
    if (arData.pProjectileBase)
        aEvent.ProjectileBaseID = arData.pProjectileBase->formID;
    if (arData.pShooter)
        aEvent.ShooterID = arData.pShooter->formID;
    if (arData.pFromWeapon)
        aEvent.WeaponID = arData.pFromWeapon->formID;
    if (arData.pFromAmmo)
        aEvent.AmmoID = arData.pFromAmmo->formID;
    if (arData.pParentCell)
        aEvent.ParentCellID = arData.pParentCell->formID;
    if (arData.pSpell)
        aEvent.SpellID = arData.pSpell->formID;
#else
    (void)arData;
    (void)aEvent;
#endif
}

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, LaunchData& apLaunchData) noexcept
{
    BSPointerHandle<Projectile>* result = TiltedPhoques::ThisCall(RealLaunch, apResult, apLaunchData);

    TP_ASSERT(result, "No projectile handle returned.");
    if (!result)
    {
        spdlog::error("No projectile handle returned.");
        return nullptr;
    }

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");
    if (!pProjectile)
    {
        spdlog::error("No projectile found.");
        return nullptr;
    }

    pProjectile->fPower = apLaunchData.fPower;

    return result;
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, Projectile::LaunchData& arData)
{
    // sync concentration spells through spell cast sync, the rest through projectile sync
    if (arData.pSpell)
    {
        if (auto* pSpell = Cast<SpellItem>(arData.pSpell))
        {
            if (pSpell->eCastingType == MagicSystem::CastingType::CONCENTRATION)
            {
                return TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
            }
        }
    }

    if (arData.pShooter)
    {
        Actor* pActor = Cast<Actor>(arData.pShooter);
        if (pActor)
        {
            ActorExtension* pExtendedActor = pActor->GetExtension();
            // GetExtension() returns null for actors that are neither a plain
            // Actor nor the PlayerCharacter (most creatures) - guard before use.
            if (pExtendedActor && pExtendedActor->IsRemote())
            {
                apThis->handle.iBits = 0;
                return apThis;
            }
        }
    }

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
    ReadLaunchDataFormIds(arData, Event);
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    Event.CastingSource = arData.eCastingSource;
    Event.UnkBool1 = arData.bUnkBool1;
    Event.Area = arData.iArea;
    Event.Power = arData.fPower;
    Event.Scale = arData.fScale;
    Event.AlwaysHit = arData.bAlwaysHit;
    Event.NoDamageOutsideCombat = arData.bNoDamageOutsideCombat;
    Event.AutoAim = arData.bAutoAim;
    Event.UnkBool2 = arData.bUnkBool2;
    Event.DeferInitialization = arData.bDeferInitialization;
    Event.ForceConeOfFire = arData.bForceConeOfFire;

    auto result = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);

    TP_ASSERT(result, "No projectile handle returned.");

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");

    Event.Power = pProjectile->fPower;

    // A projectile can launch (background/menu scene, early scripted event)
    // before TiltedOnlineApp::BeginMain() has run World::Create() - World::Get()
    // is UB on an empty entt::locator, and every member access after it lands
    // on some offset into a null object (this crashed inside RunnerService's
    // TaskQueue mutex, at exactly its byte offset within World). Skip the sync
    // for this one launch rather than dereference through a null World.
    if (entt::locator<World>::has_value())
        World::Get().GetRunner().Trigger(Event);

    return result;
}

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        POINTER_SKYRIMSE(TLaunch, s_launch, 44108, 42928);

        RealLaunch = s_launch.Get();

        TP_HOOK(&RealLaunch, HookLaunch);

        // id 34452 is unverified on VR - it resolves through the crosswalk
        // table to an unrelated address, and the Jump below then writes a
        // 5-byte jmp into the middle of whatever real function lives there
        // (confirmed in a crash dump). Neutralised until verified.
        #ifndef SKYRIMVR
        VersionDbPtr<uint8_t> hookLoc(34452);
        #else
        // Actually neutralised now: both branches used 34452 before, so the Jump
        // below was still patching an unrelated function +0x374 on VR.
        VersionDbPtr<uint8_t> hookLoc(0);
        #endif
        if (!hookLoc.Get())
            return;

        struct C : TiltedPhoques::CodeGenerator
        {
            C(uint8_t* apLoc)
            {
                // replicate
                mov(rbx, ptr[rsp + 0x50]);

                // nullptr check
                cmp(rbx, 0);
                jz("exit");
                // jump back
                jmp_S(apLoc + 0x379);

                L("exit");
                // return false; scratch space from the registers
                mov(al, 0);
                add(rsp, 0x138);
                pop(r15);
                pop(r14);
                pop(r13);
                pop(r12);
                pop(rdi);
                pop(rsi);
                pop(rbx);
                pop(rbp);
                ret();
            }
        } gen(hookLoc.Get());
        TiltedPhoques::Jump(hookLoc.Get() + 0x374, gen.getCode());
    });

