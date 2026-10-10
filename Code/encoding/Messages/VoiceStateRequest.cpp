#include <Messages/VoiceStateRequest.h>

void VoiceStateRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Enabled);
}

void VoiceStateRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Enabled = Serialization::ReadBool(aReader);
}
