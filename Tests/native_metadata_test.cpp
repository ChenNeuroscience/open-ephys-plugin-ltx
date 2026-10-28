// Links the exact GUI's Metadata.cpp and JUCE core, rather than the writer shim.
#include <Metadata.h>
#include <juce_graphics/colour/juce_Colour.cpp>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <iostream>

// Only metadata is needed to reproduce a read during pre-acquisition setup.
class DataStream : public MetadataObject {};
#include "LTXCameraMetadata.h"

int main() {
    MetadataDescriptor crop(MetadataDescriptor::CHAR, 128, "crop", "crop", "camera_tracker.crop");
    assert(crop.getDataSize() == 129);
    std::array<char, 129> input{};
    const char* description = "rectangle(cx,cy,w,h,deg) 330 250 420 300 17.19";
    std::strncpy(input.data(), description, 128);
    auto* value = new MetadataValue(crop, input.data());
    assert(value->getDataSize() == 129);

    // Safely demonstrate the old failure: byte 128 is overwritten even though
    // the declared metadata length is 128. A char[128] destination overflowed.
    std::array<char, 129> probe;
    probe.fill('!');
    value->getValue<char>(probe.data());
    assert(probe[128] == '\0');

    // The extra byte is not guaranteed to be NUL when the source used the raw
    // setter with a struct member. The bounded reader must ignore it either way.
    input[128] = '~';
    value->setValue<char>(input.data());
    probe.fill('!');
    value->getValue<char>(probe.data());
    assert(probe[128] == '~');

    DataStream stream;
    stream.addMetadata(new MetadataDescriptor(crop), value);
    auto read = LTX::readCameraMetadata(&stream);
    assert(read.present && read.hasCrop && !read.hasWindow);
    assert(read.cropText == description);
    assert(read.shape == LTX::CameraMetadata::Shape::Rectangle);

    // A full field with no terminator inside its declared length is bounded.
    input.fill('x');
    input[128] = '\0';
    value->setValue<char>(input.data());
    read = LTX::readCameraMetadata(&stream);
    assert(read.cropText == std::string(128, 'x'));
    assert(read.shape == LTX::CameraMetadata::Shape::Unknown);

    // Embedded control characters cannot break the saved file header.
    input.fill(0);
    std::strcpy(input.data(), "unknown()\r\nnew_header bad");
    value->setValue<char>(input.data());
    read = LTX::readCameraMetadata(&stream);
    assert(read.cropText == "unknown()  new_header bad");
    assert(!LTX::readCameraMetadata(nullptr).present);
    std::cout << "PASS: real GUI CHAR[128] copies 129 bytes; production reader uses bounded String API\n";
}
