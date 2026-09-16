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
            // Null for most creatures.
            if (pExtendedActor && pExtendedActor->IsRemote())
            {
#ifdef SKYRIMVR
                // The owner syncs this projectile, so the local copy must go. VR's LaunchSpell reads the
                // returned projectile without a null check, so launch it and delete it on the main thread.
                auto* pResult = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
                if (pResult && pResult->handle.iBits && entt::locator<World>::has_value())
                {
                    World::Get().GetRunner().Queue(
                        [handle = pResult->handle.iBits]()
                        {
                            if (TESObjectREFR* pObject = TESObjectREFR::GetByHandle(handle))
                            {
                                pObject->Disable();
                                pObject->Delete();
                            }
                        });
                }
                return pResult;
#else
                apThis->handle.iBits = 0;
                return apThis;
#endif
            }
        }
    }

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
#ifndef SKYRIMVR
    // The LaunchData layout past Origin is wrong on VR: these pointers read back as garbage there.
    if (arData.pProjectileBase)
        Event.ProjectileBaseID = arData.pProjectileBase->formID;
    if (arData.pShooter)
        Event.ShooterID = arData.pShooter->formID;
    if (arData.pFromWeapon)
        Event.WeaponID = arData.pFromWeapon->formID;
    if (arData.pFromAmmo)
        Event.AmmoID = arData.pFromAmmo->formID;
#endif
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
#ifndef SKYRIMVR
    if (arData.pParentCell)
        Event.ParentCellID = arData.pParentCell->formID;
    if (arData.pSpell)
        Event.SpellID = arData.pSpell->formID;
#endif
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

    // Projectiles can launch before the World exists (main menu scene).
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

        // The projectile handle is read from the stack and dereferenced two instructions later; a null one crashed
        // the game. The stub below re-reads it and returns false instead.
        //
        // Same function on both builds, with a different frame: SE 33672 / VR 0x554980 reads the handle at +0x397
        // (`mov rbx, [rsp+0x58]`) and has a 0x158 frame, checked in the VR code, where AE 34452 reads it at +0x374
        // (`mov rbx, [rsp+0x50]`) with a 0x138 frame. The read is exactly the five bytes a jump needs, and nothing
        // branches into them.
#ifdef SKYRIMVR
        VersionDbPtr<uint8_t> hookLoc(33672);
        constexpr uint32_t cHandleRead = 0x397;
        constexpr uint32_t cHandleSlot = 0x58;
        constexpr uint32_t cFrameSize = 0x158;
#else
        VersionDbPtr<uint8_t> hookLoc(34452);
        constexpr uint32_t cHandleRead = 0x374;
        constexpr uint32_t cHandleSlot = 0x50;
        constexpr uint32_t cFrameSize = 0x138;
#endif

        struct C : TiltedPhoques::CodeGenerator
        {
            C(uint8_t* apLoc, uint32_t aHandleRead, uint32_t aHandleSlot, uint32_t aFrameSize)
            {
                // replicate
                mov(rbx, ptr[rsp + aHandleSlot]);

                // nullptr check
                cmp(rbx, 0);
                jz("exit");
                // jump back
                jmp_S(apLoc + aHandleRead + 5);

                L("exit");
                // return false; scratch space from the registers
                mov(al, 0);
                add(rsp, aFrameSize);
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
        } gen(hookLoc.Get(), cHandleRead, cHandleSlot, cFrameSize);
        TiltedPhoques::Jump(hookLoc.Get() + cHandleRead, gen.getCode());
    });
