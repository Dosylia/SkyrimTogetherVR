#include <Messages/PlayerPlaceRequest.h>

void PlayerPlaceRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Place);
}

void PlayerPlaceRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Place = Serialization::ReadString(aReader);
}
