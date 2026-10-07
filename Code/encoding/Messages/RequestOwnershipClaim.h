#pragma once

#include "Message.h"

struct RequestOwnershipClaim final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestOwnershipClaim;

    RequestOwnershipClaim()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestOwnershipClaim() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestOwnershipClaim& achRhs) const noexcept
    {
        return ServerId == achRhs.ServerId && ExpectedOwnershipEpoch == achRhs.ExpectedOwnershipEpoch && Follower == achRhs.Follower && GetOpcode() == achRhs.GetOpcode();
    }

    uint32_t ServerId{};
    uint32_t ExpectedOwnershipEpoch{};
    //! The actor is the claimant's own follower: it belongs to the claimant's game, party leader or not (Emma,
    //! 2026-10-07), and nobody else's claim takes it while the claimant is connected.
    bool Follower{};
};
