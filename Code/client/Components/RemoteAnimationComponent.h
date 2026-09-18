#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct RemoteAnimationComponent
{
    List<ActionEvent> TimePoints;
    ActionEvent LastRanAction;
    ActionEvent LastProcessedAction;
    uint32_t ReplayCount;
    bool ResetAnimationGraphForReplay{false};
    // The owner has another kind of creature at this reference (see MarkForeignGraph in CharacterService): its
    // actions belong to a graph this actor does not have, so none are replayed.
    bool ForeignGraph{false};
};
