#pragma once

#include <Structs/Movement.h>
#include <Structs/ActionEvent.h>
#include <Structs/VRPose.h>

using TiltedPhoques::Buffer;
using TiltedPhoques::Vector;

struct ReferenceUpdate
{
    ReferenceUpdate() = default;
    ~ReferenceUpdate() = default;

    bool operator==(const ReferenceUpdate& acRhs) const noexcept;
    bool operator!=(const ReferenceUpdate& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader);

    Movement UpdatedMovement{};
    Vector<ActionEvent> ActionEvents{};
    VRPose UpdatedVRPose{};
};
