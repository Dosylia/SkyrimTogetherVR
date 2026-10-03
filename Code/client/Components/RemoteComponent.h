#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>

struct RemoteComponent
{
    RemoteComponent(uint32_t aId, uint32_t aRefId, uint32_t aOwnershipEpoch) noexcept
        : Id(aId)
        , CachedRefId(aRefId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    uint32_t Id;
    uint32_t CachedRefId;
    uint32_t OwnershipEpoch;
    //! The base form of the copy made for this character, when this client made one (0 otherwise). The game reuses a
    //! deleted copy's form id, so CachedRefId alone can end up naming an actor of somebody else's; a base that no
    //! longer matches says so (see CharacterService::RunSpawnUpdates).
    uint32_t CachedBaseId{};
};
