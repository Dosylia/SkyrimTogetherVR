#include <TiltedOnlinePCH.h>

#include <Events/EventDispatcher.h>

namespace details
{
void InternalRegisterSink(void* apEventDispatcher, void* apSink) noexcept
{
    TP_THIS_FUNCTION(TRegisterSink, void, void, void* apSink);

    // SkyrimVM ctor RegisterSinks
    POINTER_SKYRIMSE(TRegisterSink, s_registerSink, 54425, 54425);

    TiltedPhoques::ThisCall(s_registerSink, apEventDispatcher, apSink);
}

void InternalUnRegisterSink(void* apEventDispatcher, void* apSink) noexcept
{
    TP_THIS_FUNCTION(TUnRegisterSink, void, void, void* apSink);

    // SkyrimVM dtor UnRegisterSinks
    POINTER_SKYRIMSE(TUnRegisterSink, s_unregisterSink, 54522, 54522);

    TiltedPhoques::ThisCall(s_unregisterSink, apEventDispatcher, apSink);
}

void InternalPushEvent(void* apEventDispatcher, void* apEvent) noexcept
{
    TP_THIS_FUNCTION(TPushEvent, void, void, void* apSink);

    // "Failed to setup moving reference because it has no parent cell or no 3D" after interlocked
    POINTER_SKYRIMSE(TPushEvent, s_pushEvent, 19364, 19364);

    TiltedPhoques::ThisCall(s_pushEvent, apEventDispatcher, apEvent);
}
} // namespace details

EventDispatcherManager* EventDispatcherManager::Get() noexcept
{
    using TGetEventDispatcherManager = EventDispatcherManager*();

    // 14298 is this symbol's AE id, not its SE/VR one (confirmed against
    // CommonLibVR-NG's ScriptEventSourceHolder::GetSingleton, which uses
    // RELOCATION_ID(14108, 14298) - SE id first, AE id second). VR shares
    // old-gen SE's numbering, not AE's, so 14298 doesn't exist in the VR
    // Address Library at all and fell through to an unverified crosswalk
    // guess that crashed inside the real game code it pointed at. 14108 is
    // confirmed present in the official VR Address Library CSV.
    POINTER_SKYRIMSE(TGetEventDispatcherManager, s_getEventDispatcherManager, 14298, 14108);

    return s_getEventDispatcherManager.Get()();
}

