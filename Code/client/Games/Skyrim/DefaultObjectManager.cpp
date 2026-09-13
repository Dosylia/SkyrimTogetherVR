#include <TiltedOnlinePCH.h>

#include <DefaultObjectManager.h>

DefaultObjectManager& DefaultObjectManager::Get()
{
    using TGetDefaultObjectManager = DefaultObjectManager&();

    POINTER_SKYRIMSE(TGetDefaultObjectManager, GetDefaultObjectManager, 13894, 10878); // VR: AE 13894 -> SE 10878 BGSDefaultObjectManager::GetSingleton (vr_address_tools se_ae + database.csv); 13894 is a different SE function

    return GetDefaultObjectManager();
}

