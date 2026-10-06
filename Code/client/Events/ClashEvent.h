#pragma once

/**
 * @brief Dispatched when this player's hand, or what it holds, met another player's weapon here (a clash,
 * CharacterService from VRBodySync). The defender's rule (ActorValueService) weighs that player's hits against it.
 */
struct ClashEvent
{
    //! The other player's copy here.
    uint32_t FormId;
    //! When, on the server's clock.
    uint64_t Tick;
};
