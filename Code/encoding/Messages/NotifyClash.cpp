#include <Messages/NotifyClash.h>

void NotifyClash::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, FromId);
    Serialization::WriteVarInt(aWriter, OtherId);
    Serialization::WriteVarInt(aWriter, OtherSide);
    Serialization::WriteVarInt(aWriter, OwnSide);
    Point.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, Speed);
    Serialization::WriteVarInt(aWriter, Tick);
}

void NotifyClash::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    FromId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OtherId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OtherSide = static_cast<uint8_t>(Serialization::ReadVarInt(aReader) & 0xFF);
    OwnSide = static_cast<uint8_t>(Serialization::ReadVarInt(aReader) & 0xFF);
    Point.Deserialize(aReader);
    Speed = Serialization::ReadFloat(aReader);
    Tick = Serialization::ReadVarInt(aReader);
}
