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
    assert(paddingSamples(0.397, 50) == 20);
    assert(paddingSamples(0.4, 5000) == 2000);
    bool rejected = false;
    try { paddingSamples(std::nan(""), 50); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
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
    CoreServices::stopRequests = 0;
    RecordNode node;
    CoreServices::nodes = {{10, &node}};
    LTX::RecordEnginePlugin engine;
    configure(engine, node, 0);
    engine.openFiles(File(folder / "node"), 1, 0);
    positionData(engine);
    engine.closeFiles();
    assert(CoreServices::stopRequests > 0);
    assert(!std::filesystem::exists(folder.string() + ".pos"));
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
            // This must be rejected before either input pointer is copied.
            engine.writeContinuousData(0, 0, &value, &time, 40000000);
        } else if (name != "empty") {
            engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
            if (name == "duplicate") engine.writeTimestampSyncText(nullptr, epochMs, 0, "Software Time (");
            if (name == "missing-participant") {
                DataStream stream("bonsai");
                engine.writeTimestampSyncText(&stream, 0, 100.0, "Start Time");
                positionData(engine);
            }
        }
        engine.closeFiles();
        assert((CoreServices::stopRequests == 0) == (name == "empty"));
        assert(!std::filesystem::exists(folder.string() + ".pos"));
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
    LTX::Timing::Session excessive({1, 2});
    excessive.claim(1, "position"); excessive.claim(2, "eeg");
    excessive.publish(1, epochMs);
    excessive.publish(2, epochMs + 600001);
    assert(excessive.hasFailed() && !excessive.snapshot().epochMs);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    coordinatorTests();
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
    missingAnchor(root);
    edgeCases(root);
    std::cout << "Production record-engine scenarios completed\n";
}
