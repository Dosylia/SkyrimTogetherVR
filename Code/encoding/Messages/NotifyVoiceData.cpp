#include <Messages/NotifyVoiceData.h>
#include <Messages/VoiceDataRequest.h>

void NotifyVoiceData::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    Serialization::WriteVarInt(aWriter, Sequence);
    Serialization::WriteVarInt(aWriter, Data.size());
    aWriter.WriteBytes(Data.data(), Data.size());
}

void NotifyVoiceData::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Sequence = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const uint64_t cSize = Serialization::ReadVarInt(aReader);
    Data.clear();
    if (cSize > VoiceDataRequest::kMaxBytes)
        return;
    Data.resize(static_cast<size_t>(cSize));
    if (!aReader.ReadBytes(Data.data(), Data.size()))
        Data.clear();
}
