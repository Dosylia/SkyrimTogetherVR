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

// The form id behind a LaunchData pointer, or 0 unless (on VR) the memory is readable and the form table maps that id
// back to this very pointer. On VR these pointers were once seen as garbage, so they were not read there at all: the
// shooter id stayed 0, CombatService dropped every launch, and the other player never saw an arrow leave this client
// (2026-09-18). TiltedEvolutionVR reads the same layout on VR. With this check a wrong layout costs an unsynced
// projectile, never a crash; HookLaunch logs the first launches so the next session settles which it is.
static uint32_t ValidFormId(const void* apForm) noexcept
{
    if (!apForm)
        return 0;

#ifdef SKYRIMVR
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apForm, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    if (static_cast<const uint8_t*>(apForm) + sizeof(TESForm) > static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize)
        return 0;
#endif

    const auto* pForm = static_cast<const TESForm*>(apForm);
    const uint32_t formId = pForm->formID;

#ifdef SKYRIMVR
    if (TESForm::GetById(formId) != pForm)
        return 0;
#endif

    return formId;
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
    Event.ProjectileBaseID = ValidFormId(arData.pProjectileBase);
    Event.ShooterID = ValidFormId(arData.pShooter);
    Event.WeaponID = ValidFormId(arData.pFromWeapon);
    Event.AmmoID = ValidFormId(arData.pFromAmmo);
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    Event.ParentCellID = ValidFormId(arData.pParentCell);
    Event.SpellID = ValidFormId(arData.pSpell);

    // TEMPORARY: says which of the six resolved on VR. The first launches of any kind, and every launch that carries
    // a spell (staff and spell projectiles are rare, and a staff of light was reported breaking things for the other
    // player on 2026-09-18 after the ten arrows had used up this budget). Remove once the layout is settled.
    static uint32_t s_launchesLogged = 0;
    if (s_launchesLogged < 10 || Event.SpellID != 0)
    {
        ++s_launchesLogged;
        spdlog::info("Projectile launch: shooter {:X}, base {:X}, weapon {:X}, ammo {:X}, cell {:X}, spell {:X}", Event.ShooterID, Event.ProjectileBaseID,
                     Event.WeaponID, Event.AmmoID, Event.ParentCellID, Event.SpellID);
    }
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
