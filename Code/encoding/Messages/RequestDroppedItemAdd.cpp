#include <Messages/RequestDroppedItemAdd.h>

void RequestDroppedItemAdd::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Item.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Position.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, Rotation.x);
    Serialization::WriteFloat(aWriter, Rotation.y);
    Serialization::WriteFloat(aWriter, Rotation.z);
    Serialization::WriteBool(aWriter, Announcement);
}

void RequestDroppedItemAdd::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Item.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    Position.Deserialize(aReader);
    Rotation.x = Serialization::ReadFloat(aReader);
    Rotation.y = Serialization::ReadFloat(aReader);
    Rotation.z = Serialization::ReadFloat(aReader);
    Announcement = Serialization::ReadBool(aReader);
}
