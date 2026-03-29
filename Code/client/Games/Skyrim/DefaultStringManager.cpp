#include <TiltedOnlinePCH.h>

#include <DefaultStringManager.h>

DefaultStringManager& DefaultStringManager::Get()
{
    using TGetDefaultStringManager = DefaultStringManager&();

    POINTER_SKYRIMSE(TGetDefaultStringManager, GetDefaultStringManager, 11437, 0);

    return GetDefaultStringManager();
}

