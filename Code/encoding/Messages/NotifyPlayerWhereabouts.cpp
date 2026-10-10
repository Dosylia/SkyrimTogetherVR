#include <Messages/NotifyPlayerWhereabouts.h>

namespace
{
// More than any server holds; a longer list is cut when read.
constexpr uint64_t kMaxPlayers = 256;
} // namespace

void NotifyPlayerWhereabouts::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Players.size());
    for (const Entry& entry : Players)
    {
        Serialization::WriteVarInt(aWriter, entry.PlayerId);
        entry.WorldSpaceId.Serialize(aWriter);
        entry.CellId.Serialize(aWriter);
        entry.Position.Serialize(aWriter);
        Serialization::WriteString(aWriter, entry.Place);
    }
}

void NotifyPlayerWhereabouts::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    Players.clear();
    for (uint64_t i = 0; i < cCount && i < kMaxPlayers; ++i)
    {
        Entry entry{};
        entry.PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        entry.WorldSpaceId.Deserialize(aReader);
        entry.CellId.Deserialize(aReader);
        entry.Position.Deserialize(aReader);
        entry.Place = Serialization::ReadString(aReader);
        Players.push_back(std::move(entry));
    }
}
