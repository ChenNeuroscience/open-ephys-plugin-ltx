// Links the exact GUI's Metadata.cpp and JUCE core, rather than the writer shim.
#include <Metadata.h>
#include <DataQueue.h>
#include <juce_graphics/colour/juce_Colour.cpp>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <iostream>
#include <fstream>

// Only metadata is needed to reproduce a read during pre-acquisition setup.
class DataStream : public MetadataObject {};
#include "LTXCameraMetadata.h"
#include "LTXPositionAssembler.h"

extern "C" PLUGIN_API OELogger& getOELogger() { return OELogger::instance(); }

static void recordingQueueWraps(std::ostream* trace) {
    constexpr int blockSize = 64, blocks = 300, capacity = blockSize * blocks;
    constexpr int batch = 7, total = 3 * capacity + 13;
    DataQueue queue(blockSize, blocks);
    queue.setChannelCount(7);
    queue.setTimestampStreamCount(1);
    AudioBuffer<float> input(7, batch);
    std::vector<CircularBufferIndexes> channelIndexes(7), timestampIndexes(1);
    Array<int64> sampleNumbers;
    LTX::PositionAssembler assembler;
    int written = 0, splits = 0;
    const auto sampleValue = [](int channel, int sample) {
        return channel == 0 ? float(12 + sample / 50.0) : float(channel * 100 + sample % 100);
    };
    if (trace) {
        const std::uint64_t rows = total;
        trace->write(reinterpret_cast<const char*>(&rows), sizeof(rows));
    }
    const auto drain = [&] {
        assert(queue.startRead(channelIndexes, timestampIndexes, sampleNumbers, 4096));
        if (channelIndexes[0].size2) ++splits;
        for (int channel = 0; channel < 7; ++channel) {
            const auto deliver = [&](int offset, int size) {
                if (!size) return;
                const float* values = queue.getContinuousDataBufferReference().getReadPointer(channel, offset);
                const double* times = queue.getTimestampBufferReference().getReadPointer(0, offset);
                if (trace) {
                    const std::int32_t c = channel, n = size;
                    trace->write(reinterpret_cast<const char*>(&c), sizeof(c));
                    trace->write(reinterpret_cast<const char*>(&n), sizeof(n));
                    trace->write(reinterpret_cast<const char*>(values), size * sizeof(float));
                    trace->write(reinterpret_cast<const char*>(times), size * sizeof(double));
                }
                assembler.append(channel, values, times, size);
                LTX::PositionAssembler::Frame frame;
                while (assembler.pop(frame)) {
                    for (int c = 0; c < 7; ++c) assert(frame[c] == sampleValue(c, written));
                    ++written;
                }
            };
            const auto& parts = channelIndexes[channel];
            deliver(parts.index1, parts.size1);
            if (parts.size2) deliver(parts.index2, parts.size2);
        }
        queue.stopRead();
    };
    for (int at = 0; at < total; at += batch) {
        const int count = std::min(batch, total - at);
        for (int channel = 0; channel < 7; ++channel)
            for (int i = 0; i < count; ++i) input.setSample(channel, i, sampleValue(channel, at + i));
        queue.writeSynchronizedTimestamps(at / 50.0, 1 / 50.0, 0, count);
        // Exercise a reader running halfway through the producer's channel
        // loop, as well as the ordinary case where all channels are ready.
        const bool interleave = (at / batch) % 2 == 0;
        for (int channel = 0; channel < 7; ++channel) {
            queue.writeChannel(input, channel, channel, count, at);
            if (interleave && channel == 2) drain();
        }
        drain();
    }
    assembler.finish();
    assert(written == total && splits == 3);
    std::cout << "PASS: real GUI DataQueue, three wraps, " << written
              << " complete rows; 64 * 300 / 50 = 384 seconds per wrap\n";
}

int main(int argc, char** argv) {
    assert(argc == 1 || argc == 2);
    std::ofstream trace;
    if (argc == 2) {
        trace.open(argv[1], std::ios::binary);
        assert(trace);
    }
    recordingQueueWraps(argc == 2 ? &trace : nullptr);
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
