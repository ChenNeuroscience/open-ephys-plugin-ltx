#ifdef NDEBUG
#undef NDEBUG
#endif
#include "LTXRecordEnginePlugin.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <thread>

static constexpr int64 epochMs = 1700000000000LL;

static void configure(LTX::RecordEnginePlugin& engine, RecordNode& node, int mode) {
    engine.registerRecordNode(&node);
    if (mode == 0) {
        engine.testStreams.emplace_back("bonsai");
        for (int i = 0; i < 7; ++i) {
            engine.testChannels.emplace_back("bonsai", 50);
            engine.testChannelMap.push_back(i);
        }
    } else if (mode == 1) {
        engine.testStreams.emplace_back("memory_usage-extra");
        engine.testStreams.emplace_back("neural");
        engine.testChannels.emplace_back("memory_usage-extra", 1);
        engine.testChannels.emplace_back("neural", 30000);
        engine.testChannels.emplace_back("neural", 30000);
        engine.testChannelMap = {0, 1, 2};
    } else if (mode == 2) {
        engine.testStreams.emplace_back("neural");
        engine.testSpikeChannels = 1;
        engine.testEventChannels = 1;
    }
}

static double nativeOrigin(int mode) { return 100.0 + 100.0 * mode; }

static void positionData(LTX::RecordEnginePlugin& engine, bool nanHead = false) {
    double times[] = {100.0, 100.02, 100.04};
    for (int channel = 0; channel < 7; ++channel) {
        float values[] = {100.0f + channel, 101.0f + channel, 102.0f + channel};
        if (channel == 0) {
            values[0] = nanHead ? std::nanf("") : 12.0f;
            values[1] = 12.02f;
            values[2] = 12.04f;
        } else if (nanHead) values[0] = std::nanf("");
        engine.writeContinuousData(channel, channel, values, times, 3);
    }
}

static void eegData(LTX::RecordEnginePlugin& engine) {
    float values[13];
    double times[13];
    for (int i = 0; i < 13; ++i) {
        values[i] = float(100 + i);
        times[i] = 200.0 + double(i) / 30000;
    }
    for (int channel = 0; channel < 3; ++channel) {
        engine.writeContinuousData(channel, channel, values, times, 7);
        engine.writeContinuousData(channel, channel, values + 7, times + 7, 5);
        engine.writeContinuousData(channel, channel, values + 12, times + 12, 1);
    }
}

static void spikeData(LTX::RecordEnginePlugin& engine) {
    Spike before{299.9}, spike{300.02};
    engine.writeSpike(0, &before);
    engine.writeSpike(0, &spike);
    engine.writeEvent(0, EventPacket{300.01, 1, true});
}

static void scenario(const std::filesystem::path& root, const std::string& name,
                     const std::array<int, 3>& delays, bool nanHead = false,
                     bool repeat = false, bool nativeFirst = false) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = name;
    CoreServices::messages.clear();
    CoreServices::stopRequests = 0;
    RecordNode nodes[3];
    LTX::RecordEnginePlugin engines[3];
    CoreServices::nodes = {{10, &nodes[0]}, {20, &nodes[1]}, {30, &nodes[2]}};
    for (int mode = 0; mode < 3; ++mode) configure(engines[mode], nodes[mode], mode);
    for (int recording = 0; recording < (repeat ? 2 : 1); ++recording) {
        // Background thread order differs from the captured recording order.
        for (int mode : {2, 0, 1}) engines[mode].openFiles(File(folder / ("node" + std::to_string(mode))), 1, recording);
        // The real GUI writes continuous data before draining the sync-text queue.
        positionData(engines[0], nanHead);
        eegData(engines[1]);
        spikeData(engines[2]);
        DataStream streams[] = {DataStream("bonsai"), DataStream("neural"), DataStream("neural")};
        if (nativeFirst)
            for (int mode : {1, 2, 0}) engines[mode].writeTimestampSyncText(&streams[mode], 0, nativeOrigin(mode), "Start Time");
        for (int mode : {2, 1, 0}) {
            const int delay = recording == 0 ? delays[mode] : (mode == 0 ? 400 : 0);
            engines[mode].writeTimestampSyncText(nullptr, epochMs + recording * 10000 + delay, 0,
                "Software Time (milliseconds since midnight Jan 1st 1970 UTC)");
            if (!nativeFirst) engines[mode].writeTimestampSyncText(&streams[mode], 0, nativeOrigin(mode), "Start Time");
        }
        std::thread a([&] { engines[0].closeFiles(); });
        std::thread b([&] { engines[1].closeFiles(); });
        std::thread c([&] { engines[2].closeFiles(); });
        a.join(); b.join(); c.join();
        assert(CoreServices::stopRequests == 0);
    }
    std::ofstream messages(folder / "messages.txt");
    for (auto& message : CoreServices::messages) messages << message << '\n';
}

static void coordinatorTests() {
    using namespace LTX::Timing;
    Session session({1, 2, 3});
    session.claim(1, "position"); session.claim(2, "eeg"); session.claim(3, "spikes");
    session.publish(3, epochMs + 400);
    assert(!session.snapshot().epochMs);
    session.publish(2, epochMs + 200);
    assert(!session.snapshot().epochMs);
    session.publish(1, epochMs);
    assert(*session.snapshot().epochMs == epochMs);
    assert(session.snapshot().delaySeconds(3) == 0.4);
    assert(session.takeWarnings().size() == 2);
    assert(session.takeWarnings().empty());
    Session concurrent({1, 2});
    concurrent.claim(1, "eeg"); concurrent.claim(2, "position");
    std::thread a([&] { concurrent.publish(2, epochMs); });
    std::thread b([&] { concurrent.publish(1, epochMs + 400); });
    a.join(); b.join();
    assert(*concurrent.snapshot().epochMs == epochMs);
}

static void missingAnchor(const std::filesystem::path& root) {
    const auto folder = root / "missing-anchor";
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = "missing-anchor";
    CoreServices::messages.clear();
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    engine.openFiles(File(folder / "node"), 1, 0);
    positionData(engine);
    DataStream stream("bonsai");
    engine.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
    engine.closeFiles();
    assert(CoreServices::stopRequests == 0);
    assert(!CoreServices::messages.empty());
}

static void edgeCases(const std::filesystem::path& root) {
    for (const std::string name : {"empty", "missing-participant", "missing-native", "capacity", "duplicate"}) {
        const auto folder = root / name;
        std::filesystem::create_directories(folder);
        CoreServices::recordingName = name;
        CoreServices::stopRequests = 0;
        RecordNode node, other;
        CoreServices::nodes = {{10, &node}};
        if (name == "missing-participant") CoreServices::nodes.emplace(20, &other);
        LTX::RecordEnginePlugin engine;
        configure(engine, node, 0);
        engine.openFiles(File(folder / "node"), 1, 0);
        if (name == "capacity") {
            const float value = 0;
            const double time = 0;
            engine.writeContinuousData(0, 0, &value, &time, 40000000);
        } else if (name != "empty") {
            engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
            if (name == "duplicate") engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
            positionData(engine);
            if (name != "missing-native") {
                DataStream stream("bonsai");
                engine.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
            }
        }
        engine.closeFiles();
        assert((CoreServices::stopRequests > 0) == (name == "capacity" || name == "missing-native"));
    }
    {
        const auto folder = root / "inactive";
        std::filesystem::create_directories(folder);
        CoreServices::recordingName = "inactive";
        CoreServices::stopRequests = 0;
        RecordNode nodes[2];
        CoreServices::nodes = {{10, &nodes[0]}, {20, &nodes[1]}};
        LTX::RecordEnginePlugin position, inactive;
        configure(position, nodes[0], 0);
        configure(inactive, nodes[1], 3);
        inactive.openFiles(File(folder / "inactive"), 1, 0);
        position.openFiles(File(folder / "position"), 1, 0);
        positionData(position);
        DataStream stream("bonsai");
        position.writeTimestampSyncText(nullptr, epochMs + 400, 0, "Software Time (");
        position.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
        position.closeFiles();
        inactive.closeFiles();
        assert(CoreServices::stopRequests == 0);
    }
    // Diagnostics report any measured discrepancy; no padding limit applies.
    LTX::Timing::Session excessive({1, 2});
    excessive.claim(1, "position"); excessive.claim(2, "eeg");
    excessive.publish(1, epochMs);
    excessive.publish(2, epochMs + 600001);
    assert(!excessive.hasFailed() && excessive.snapshot().epochMs);
}

static void addCameraMetadata(DataStream& stream, const char* crop, bool uncalibrated = false, bool unknown = false) {
    const double ppm = uncalibrated ? std::nan("") : 800.0;
    const int32 window[] = {100, 50, 1540, 1130};
    const int32 zeroWindow[] = {0, 0, 0, 0};
    const int32 frame[] = {1920, 1200};
    char description[128]{};
    std::strncpy(description, crop, 127);
    stream.add(MetadataDescriptor::DOUBLE, "camera_tracker.pixels_per_metre", &ppm, 1);
    stream.add(MetadataDescriptor::INT32, "camera_tracker.window", unknown ? zeroWindow : window, 4);
    stream.add(MetadataDescriptor::CHAR, "camera_tracker.crop", description, 128);
    stream.add(MetadataDescriptor::INT32, "camera_tracker.frame_size", frame, 2);
}

static void cameraScenario(const std::filesystem::path& root, const std::string& name, const char* crop,
                           bool uncalibrated = false, bool unknown = false) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = name;
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    addCameraMetadata(engine.testStreams[0], crop, uncalibrated, unknown);
    // Ensure lookup uses stream identity, rather than taking the first stream.
    DataStream unrelated("other", 99);
    const int32 badWindow[] = {0, 0, 10, 10};
    unrelated.add(MetadataDescriptor::INT32, "camera_tracker.window", badWindow, 4);
    engine.testStreams.insert(engine.testStreams.begin(), unrelated);
    // Keep the position validation at one recorded stream; getDataStreams can
    // include an unrecorded stream in the actual GUI too.
    engine.testRecordedStreams = 1;
    engine.openFiles(File(folder / "node"), 1, 0);
    double times[8];
    for (int i = 0; i < 8; ++i) times[i] = 100 + i / 50.0;
    for (int channel = 0; channel < 7; ++channel) {
        float values[] = {100, 1023, 1539, std::nanf(""), INFINITY, -10, 65536, 0};
        if (channel == 0) for (int i = 0; i < 8; ++i) values[i] = 12 + i / 50.0f;
        if (channel == 2 || channel == 4) {
            values[0] = 50; values[1] = 1023; values[2] = 1129;
        }
        if (channel >= 5) {
            values[0] = 8; values[1] = 70000; values[2] = 65535;
            values[7] = 0.2f;
        }
        engine.writeContinuousData(channel, channel, values, times, 8);
    }
    engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
    DataStream stream("bonsai");
    engine.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
    engine.closeFiles();
    assert(CoreServices::stopRequests == 0);
}

static void metadataTests() {
    DataStream legacy("bonsai");
    assert(!LTX::readCameraMetadata(&legacy).present);
    DataStream camera("bonsai");
    addCameraMetadata(camera, "rectangle(cx,cy,w,h,deg) 820 590 1440 1080 17.19");
    auto metadata = LTX::readCameraMetadata(&camera);
    // The real GUI includes an extra byte for CHAR metadata. Keep the shim
    // contract explicit; omitting it used to hide a pre-acquisition overflow.
    assert(camera.metadata[2]->bytes.size() == 129);
    assert(metadata.present && metadata.validWindow() && metadata.calibrated());
    assert(metadata.window[0] == 100 && metadata.frame[0] == 1920 && metadata.ppm == 800);
    assert(metadata.shape == LTX::CameraMetadata::Shape::Rectangle);
    assert(metadata.contains(820, 590) && !metadata.contains(100, 50));
    LTX::PositionEncoding encoding(metadata);
    assert(encoding.transformed && encoding.coordinate(1023, true) != 1023);
    assert(encoding.coordinate(INFINITY, true) == 1023);
    assert(encoding.area(INFINITY) == 0 && encoding.area(70000) == 65535);
    for (const char* shape : {"circle(cx,cy,r) 820 590 540", "square(cx,cy,w,deg) 820 590 600 45",
                              "rectangle(cx,cy,w,h,deg) 820 590 600 400 -30", "none()"}) {
        metadata.cropText = shape;
        metadata.parseCrop();
        assert(metadata.shape != LTX::CameraMetadata::Shape::Unknown);
        assert(metadata.contains(820, 590));
    }
    for (const char* invalid : {"", "circle(cx,cy,r) 1 2 -1", "square(cx,cy,w,deg) 1 2 3", "none() junk",
                              "rectangle(cx,cy,w,h,deg) 1 2 3 4 5 junk", "future_shape()"}) {
        metadata.cropText = invalid;
        metadata.parseCrop();
        assert(metadata.shape == LTX::CameraMetadata::Shape::Unknown);
    }
    // A crop already fitting the legacy range is translated without scaling.
    metadata.window = {1000, 1000, 1600, 1400};
    LTX::PositionEncoding small(metadata);
    assert(small.scale == 1 && small.coordinate(1023, true) == 23);
    assert(small.coordinate(1000, false) == 0 && small.coordinate(1599, true) == 599);
    // New streams can be uncalibrated and not yet have camera bounds.
    DataStream unknown("bonsai");
    addCameraMetadata(unknown, "none()", true, true);
    metadata = LTX::readCameraMetadata(&unknown);
    assert(metadata.present && !metadata.validWindow() && !metadata.calibrated());
}

static void fractionalRate(const std::filesystem::path& root) {
    const auto folder = root / "fractional-rate";
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = "fractional-rate";
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    engine.testChannels.clear();
    for (int i = 0; i < 7; ++i) engine.testChannels.emplace_back("bonsai", 29.97f);
    addCameraMetadata(engine.testStreams[0], "none()");
    engine.openFiles(File(folder / "node"), 1, 0);
    DataStream stream("bonsai");
    engine.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
    // A full block of missing timestamps, followed by real camera times. No
    // software-time anchor: diagnostics cannot gate either block's data.
    const float rate = 29.97f;
    double times[] = {100, 100 + 1.0 / rate};
    double later[] = {100 + 2.0 / rate, 100 + 3.0 / rate};
    for (int block = 0; block < 2; ++block) {
        for (int channel = 0; channel < 7; ++channel) {
            float values[] = {200, 200};
            if (channel == 0) {
                values[0] = block ? 12 + 2 / rate : std::nanf("");
                values[1] = block ? 12 + 3 / rate : std::nanf("");
            }
            engine.writeContinuousData(channel, channel, values, block ? later : times, 2);
        }
    }
    engine.closeFiles();
    assert(CoreServices::stopRequests == 0);
}

static void positionChunks(const std::filesystem::path& root, const std::string& name, int firstChunk, bool ragged = false, bool incomplete = false, bool badTime = false) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = name;
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    engine.openFiles(File(folder / "node"), 1, 0);
    engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
    DataStream stream("bonsai");
    engine.writeTimestampSyncText(&stream, 0, 100, "Start Time");
    double times[8];
    for (int i = 0; i < 8; ++i) times[i] = 100 + i / 50.0;
    for (int channel = 0; channel < 7; ++channel) {
        float values[8];
        for (int i = 0; i < 8; ++i) values[i] = channel == 0 ? 12 + i / 50.0f : float(channel * 100 + i);
        // Exact RecordThread order at a circular-buffer boundary: both parts
        // of this channel are delivered before moving to the next channel.
        const int split = ragged ? 1 + channel % 6 : firstChunk;
        double channelTimes[8];
        std::copy(std::begin(times), std::end(times), channelTimes);
        if (badTime && channel == 3) channelTimes[0] += 0.02;
        engine.writeContinuousData(channel, channel, values, channelTimes, split);
        engine.writeContinuousData(channel, channel, values + split, channelTimes + split,
                                   8 - split - (incomplete && channel == 6 ? 1 : 0));
    }
    engine.closeFiles();
    assert((CoreServices::stopRequests > 0) == (incomplete || badTime));
}

static void longPositionRecording(const std::filesystem::path& root) {
    const auto folder = root / "pos-long";
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = "pos-long";
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    engine.openFiles(File(folder / "node"), 1, 0);
    engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
    DataStream stream("bonsai");
    engine.writeTimestampSyncText(&stream, 0, 100, "Start Time");
    constexpr int total = 60007, batch = 7, capacity = 64 * 300;
    for (int at = 0; at < total; at += batch) {
        const int count = std::min(batch, total - at);
        const int first = std::min(count, capacity - at % capacity);
        double times[batch];
        for (int i = 0; i < count; ++i) times[i] = 100 + (at + i) / 50.0;
        for (int channel = 0; channel < 7; ++channel) {
            float values[batch];
            for (int i = 0; i < count; ++i) values[i] = channel == 0 ? float(12 + (at + i) / 50.0)
                : float(channel * 100 + (at + i) % 100);
            engine.writeContinuousData(channel, channel, values, times, first);
            engine.writeContinuousData(channel, channel, values + first, times + first, count - first);
        }
    }
    // Enough binary data has been written to flush the header to disk. The
    // timing fields must still be provisional; data delivery performs no timing
    // rewrites. Final values are checked by the file parser after close.
    std::ifstream live(folder.string() + ".pos", std::ios::binary);
    std::string prefix(4096, '\0');
    live.read(prefix.data(), std::streamsize(prefix.size()));
    assert(prefix.find("record_node_start_offset_ms unavailable") != std::string::npos);
    engine.closeFiles();
    assert(CoreServices::stopRequests == 0);
}

static void headerUpdates(const std::filesystem::path& root) {
    const auto start = std::chrono::system_clock::now();
    LTX::LTXFile file((root / "headers").string(), ".test", start);
    file.AddHeaderValue("literal", "100% literal");
    file.ReserveHeader("count", "0");
    file.ReserveHeader("note", "a longer initial value");
    file.UpdateHeader("count", std::numeric_limits<std::uint64_t>::max());
    const unsigned char first[] = {0, 1, 37, 10, 13, 255};
    file.WriteBinaryData(first, sizeof(first));
    file.UpdateHeader("note", "x"); // clears the remaining reserved space
    bool rejected = false;
    try { file.UpdateHeader("note", std::string(33, 'x')); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { file.UpdateHeader("unknown", 1); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { file.ReserveHeader("late"); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    const unsigned char second[] = {3, 2, 1};
    file.WriteBinaryData(second, sizeof(second)); // position restored after header updates
    file.UpdateHeader("note", "ok");
    file.FinaliseFile(start + std::chrono::seconds(7));
    file.FinaliseFile(start + std::chrono::seconds(9)); // does not append another footer
    rejected = false;
    try { file.UpdateHeader("count", 1); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    LTX::LTXFile empty((root / "headers-empty").string(), ".test", start);
    empty.ReserveHeader("count", "0");
    empty.UpdateHeader("count", 42);
    empty.FinaliseFile(start);
}

static void replayNativeQueue(const std::filesystem::path& root, const char* tracePath, bool trimAtWrap) {
    const std::string name = trimAtWrap ? "queue-trim" : "queue-replay";
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    CoreServices::recordingName = name;
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    // Replay twice through the same engine to test a subsequent recording while
    // acquisition is conceptually still running, including all assembler resets.
    for (int recording = 0; recording < (trimAtWrap ? 1 : 2); ++recording) {
        std::ifstream trace(tracePath, std::ios::binary);
        std::uint64_t total;
        trace.read(reinterpret_cast<char*>(&total), sizeof(total));
        assert(trace && total == 57613);
        engine.openFiles(File(folder / "node"), 1, recording);
        const double clockOffset = recording * 2000.0;
        const double origin = clockOffset + (trimAtWrap ? 19199 / 50.0 : 0);
        bool published = false;
        std::int32_t channel, count;
        while (trace.read(reinterpret_cast<char*>(&channel), sizeof(channel))) {
            trace.read(reinterpret_cast<char*>(&count), sizeof(count));
            assert(trace && channel >= 0 && channel < 7 && count > 0 && count <= 4096);
            std::vector<float> values(count);
            std::vector<double> times(count);
            trace.read(reinterpret_cast<char*>(values.data()), count * sizeof(float));
            trace.read(reinterpret_cast<char*>(times.data()), count * sizeof(double));
            assert(trace);
            if (trimAtWrap && times.front() < 19194 / 50.0) continue;
            for (double& time : times) time += clockOffset;
            if (channel == 0) for (float& value : values) value += float(clockOffset);
            engine.writeContinuousData(channel, channel, values.data(), times.data(), count);
            // RecordThread drains sync messages after delivering continuous data.
            // In the trimmed case that initial delivery itself straddles a wrap.
            if (!published && channel == 6 && times.back() >= origin + 1 / 50.0) {
                DataStream stream("bonsai");
                engine.writeTimestampSyncText(nullptr, epochMs + recording * 2000000, 0, "Software Time (");
                engine.writeTimestampSyncText(&stream, 0, origin, "Start Time");
                published = true;
            }
        }
        assert(trace.eof() && published);
        engine.closeFiles();
        assert(CoreServices::stopRequests == 0);
    }
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const std::filesystem::path root(argv[1]);
    if (argc == 3) {
        replayNativeQueue(root, argv[2], false);
        replayNativeQueue(root, argv[2], true);
    }
    coordinatorTests();
    metadataTests();
    fractionalRate(root);
    positionChunks(root, "pos-unsplit", 8);
    positionChunks(root, "pos-wrapped", 3);
    positionChunks(root, "pos-equal-wrap", 4);
    positionChunks(root, "pos-ragged", 3, true);
    positionChunks(root, "pos-incomplete", 3, false, true);
    positionChunks(root, "pos-badtime", 3, false, false, true);
    longPositionRecording(root);
    headerUpdates(root);
    std::array<int, 3> order{0, 1, 2};
    int count = 0;
    do {
        std::array<int, 3> delays{};
        for (int rank = 0; rank < 3; ++rank) delays[order[rank]] = rank * 200;
        scenario(root, "order-" + std::to_string(count++), delays);
    } while (std::next_permutation(order.begin(), order.end()));
    scenario(root, "tied", {0, 0, 0});
    scenario(root, "fractional", {397, 201, 0}, false, false, true);
    scenario(root, "nan-head", {400, 0, 200}, true);
    scenario(root, "repeat", {0, 400, 200}, false, true);
    scenario(root, "threshold", {50, 51, 0});
    scenario(root, "large-discrepancy", {600001, 0, 200});
    cameraScenario(root, "camera", "rectangle(cx,cy,w,h,deg) 820 590 1440 1080 17.19");
    cameraScenario(root, "camera-uncalibrated", "circle(cx,cy,r) 820 590 540", true);
    cameraScenario(root, "camera-unknown-bounds", "none()", true, true);
    missingAnchor(root);
    edgeCases(root);
    std::cout << "Production record-engine scenarios completed\n";
}
