#include <Messages/VoiceDataRequest.h>

void VoiceDataRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Sequence);
    Serialization::WriteVarInt(aWriter, Data.size());
    aWriter.WriteBytes(Data.data(), Data.size());
}

void VoiceDataRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Sequence = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const uint64_t cSize = Serialization::ReadVarInt(aReader);
    Data.clear();
    if (cSize > kMaxBytes)
        return;
    Data.resize(static_cast<size_t>(cSize));
    if (!aReader.ReadBytes(Data.data(), Data.size()))
        Data.clear();
}
