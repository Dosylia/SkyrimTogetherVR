#include <TiltedOnlinePCH.h>

#include <Games/Animation/IAnimationGraphManagerHolder.h>

#include <BSAnimationGraphManager.h>

bool IAnimationGraphManagerHolder::SetVariableFloat(BSFixedString* apVariable, float aValue)
{
    TP_THIS_FUNCTION(TSetFloatVariable, bool, IAnimationGraphManagerHolder, BSFixedString*, float);
    // Defensive: ThisCall through an unresolved id would call a null pointer.
    POINTER_SKYRIMSE(TSetFloatVariable, InternalSetFloatVariable, 32887, 32143);
    if (!InternalSetFloatVariable.Get())
        return false;

    return TiltedPhoques::ThisCall(InternalSetFloatVariable, this, apVariable, aValue);
}

bool IAnimationGraphManagerHolder::SetVariableInt(BSFixedString* apVariable, int32_t aValue)
{
    TP_THIS_FUNCTION(TSetIntVariable, bool, IAnimationGraphManagerHolder, BSFixedString*, int32_t);
    POINTER_SKYRIMSE(TSetIntVariable, InternalSetIntVariable, 32886, 32142);

    return TiltedPhoques::ThisCall(InternalSetIntVariable, this, apVariable, aValue);
}

bool IAnimationGraphManagerHolder::SetVariableBool(BSFixedString* apVariable, bool aValue)
{
    TP_THIS_FUNCTION(TSetBoolVariable, bool, IAnimationGraphManagerHolder, BSFixedString*, bool);
    POINTER_SKYRIMSE(TSetBoolVariable, InternalSetBoolVariable, 32885, 32141);

    return TiltedPhoques::ThisCall(InternalSetBoolVariable, this, apVariable, aValue);
}

bool IAnimationGraphManagerHolder::RevertAnimationGraphManager()
{
    TP_THIS_FUNCTION(TRevertAnimationGraphManager, bool, IAnimationGraphManagerHolder);
    // id 32883 has no known VR address (parked) - Get() is null; ThisCall
    // would call through a null function pointer and crash.
    POINTER_SKYRIMSE(TRevertAnimationGraphManager, InternalRevertAnimationGraphManager, 32883, 32883);
    if (!InternalRevertAnimationGraphManager.Get())
        return false;

    return TiltedPhoques::ThisCall(InternalRevertAnimationGraphManager, this);
}

bool IAnimationGraphManagerHolder::IsReady()
{
    BSAnimationGraphManager* pAnimationGraph = nullptr;
    const auto result = GetBSAnimationGraph(&pAnimationGraph);

    if (pAnimationGraph)
        pAnimationGraph->Release();

    return result;
}

