#pragma once
// Small API substitute for executing the production record engine in tests.
// The real GUI headers are checked separately with run_tests.py --gui-source.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using int8 = std::int8_t;
using int32 = std::int32_t;
using int64 = long long;
using uint64 = std::uint64_t;

class String {
    std::string value;
public:
    String() = default;
    String(const char* s): value(s) {}
    String(std::string s): value(std::move(s)) {}
    std::string toStdString() const { return value; }
    bool startsWith(const char* prefix) const { return value.rfind(prefix, 0) == 0; }
    bool operator==(const char* other) const { return value == other; }
    bool operator!=(const char* other) const { return value != other; }
};

class File {
    std::filesystem::path path;
public:
    File(std::filesystem::path path_): path(std::move(path_)) {}
    File getParentDirectory() const { return File(path.parent_path()); }
    String getFullPathName() const { return path.string(); }
};

struct GenericProcessor { virtual ~GenericProcessor() = default; };
struct OtherBase { virtual ~OtherBase() = default; int unused = 1; };
// Deliberately offset the GenericProcessor base to exercise node identification.
struct RecordNode : OtherBase, GenericProcessor {};

class DataStream {
    String name;
public:
    DataStream(const char* name_, int id_ = 1): name(name_), id(id_) {}
    String getName() const { return name; }
    int id;
    int getStreamId() const { return id; }
    struct Field;
    std::vector<std::shared_ptr<Field>> metadata;
    int findMetadata(int type, unsigned int length, String identifier) const;
    const Field* getMetadataValue(int index) const { return metadata.at(index).get(); }
    template <class T> void add(int type, const char* name, const T* values, size_t count);
};
struct MetadataDescriptor { enum MetadataType { DOUBLE, INT32, CHAR }; };
struct DataStream::Field {
    int type;
    std::string name;
    unsigned int count;
    std::vector<unsigned char> bytes;
    template <class T> void getValue(T& value) const { std::memcpy(&value, bytes.data(), sizeof(T)); }
    template <class T> void getValue(T* values) const { std::memcpy(values, bytes.data(), bytes.size()); }
    void getValue(String& value) const {
        const auto end = std::find(bytes.begin(), bytes.begin() + count, 0);
        value = String(std::string(bytes.begin(), end));
    }
};
inline int DataStream::findMetadata(int type, unsigned int length, String identifier) const {
    for (size_t i = 0; i < metadata.size(); ++i)
        if (metadata[i]->type == type && metadata[i]->count == length && metadata[i]->name == identifier.toStdString()) return int(i);
    return -1;
}
template <class T> void DataStream::add(int type, const char* name, const T* values, size_t count) {
    auto field = std::make_shared<Field>();
    field->type = type; field->name = name; field->count = unsigned(count);
    // Match MetadataValue::getSize/getValue in the pinned GUI exactly: CHAR
    // has one additional byte, and the pointer overload copies that byte too.
    field->bytes.resize(sizeof(T) * count + (type == MetadataDescriptor::CHAR ? 1 : 0));
    std::memcpy(field->bytes.data(), values, sizeof(T) * count);
    metadata.push_back(field);
}
class ContinuousChannel {
    String stream;
    float rate;
public:
    ContinuousChannel(const char* stream_, float rate_): stream(stream_), rate(rate_) {}
    String getStreamName() const { return stream; }
    String getName() const { return "test-channel"; }
    float getSampleRate() const { return rate; }
    int getStreamId() const { return 1; }
};
struct SpikeChannel {
    int getNumChannels() const { return 4; }
    int getTotalSamples() const { return 40; }
    float getSampleRate() const { return 30000; }
};
struct Spike {
    double timestamp;
    std::vector<float> values = std::vector<float>(160, 42.0f);
    double getTimestampInSeconds() const { return timestamp; }
    const float* getDataPointer() const { return values.data(); }
};
struct EventChannel { enum Type { TTL, TEXT }; };
struct EventPacket { double timestamp; int line; bool state; };
class Event;
using EventPtr = std::unique_ptr<Event>;
class Event {
public:
    virtual ~Event() = default;
    virtual EventChannel::Type getEventType() const { return EventChannel::TTL; }
    static EventPtr deserialize(const EventPacket&, const EventChannel*);
};
class TTLEvent : public Event {
    EventPacket packet;
public:
    TTLEvent(EventPacket packet_): packet(packet_) {}
    double getTimestampInSeconds() const { return packet.timestamp; }
    int getLine() const { return packet.line; }
    bool getState() const { return packet.state; }
};
inline EventPtr Event::deserialize(const EventPacket& event, const EventChannel*) {
    return std::make_unique<TTLEvent>(event);
}

struct EngineParameter {};
struct RecordEngineManager;
class RecordEngine {
public:
    virtual ~RecordEngine() = default;
    virtual String getEngineId() const = 0;
    virtual void openFiles(File, int, int) = 0;
    virtual void closeFiles() = 0;
    virtual void writeContinuousData(int, int, const float*, const double*, int) = 0;
    virtual void writeEvent(int, const EventPacket&) = 0;
    virtual void writeSpike(int, const Spike*) = 0;
    virtual void writeTimestampSyncText(DataStream*, int64, double, String) = 0;
    virtual void setParameter(EngineParameter&) {}
    void registerRecordNode(RecordNode* node) { recordNode = node; }
    std::vector<ContinuousChannel> testChannels;
    std::vector<DataStream> testStreams;
    std::vector<int> testChannelMap;
    int testRecordedStreams = -1;
    int testSpikeChannels = 0;
    int testEventChannels = 0;
protected:
    RecordNode* recordNode = nullptr;
    int getNumRecordedSpikeChannels() const { return testSpikeChannels; }
    int getNumRecordedContinuousChannels() const { return int(testChannelMap.size()); }
    int getNumRecordedEventChannels() const { return testEventChannels; }
    int getNumRecordedDataStreams() const { return testRecordedStreams >= 0 ? testRecordedStreams : int(testStreams.size()); }
    int getGlobalIndex(int index) const { return testChannelMap.at(index); }
    const ContinuousChannel* getContinuousChannel(int index) const { return &testChannels.at(index); }
    std::vector<const DataStream*> getDataStreams() const {
        std::vector<const DataStream*> result;
        for (auto& stream : testStreams) result.push_back(&stream);
        return result;
    }
    const SpikeChannel* getSpikeChannel(int) const { static SpikeChannel channel; return &channel; }
    const EventChannel* getEventChannel(int) const { static EventChannel channel; return &channel; }
};
struct RecordEngineManager {
    RecordEngineManager(const char*, const char*, RecordEngine* (*)()) {}
};
template <class T> RecordEngine* engineFactory() { return new T; }

namespace CoreServices {
    inline std::map<int, RecordNode*> nodes;
    inline std::vector<std::string> messages;
    inline std::mutex messageMutex;
    inline String recordingName = "test";
    inline std::atomic<int> stopRequests{0};
    inline std::vector<int> getAvailableRecordNodeIds() {
        std::vector<int> result;
        for (auto entry : nodes) result.push_back(entry.first);
        return result;
    }
    inline GenericProcessor* getProcessorById(std::uint16_t id) { return nodes.at(id); }
    namespace RecordNode { inline String getRecordEngineId(int) { return "LTX"; } }
    inline String getRecordingDirectoryName() { return recordingName; }
    inline void sendStatusMessage(const char* message) {
        std::lock_guard<std::mutex> lock(messageMutex);
        messages.emplace_back(message);
    }
    inline void setRecordingStatus(bool enabled) { if (!enabled) ++stopRequests; }
    inline void setAcquisitionStatus(bool enabled) { if (!enabled) ++stopRequests; }
}
struct MessageManager {
    template <class Fn> static void callAsync(Fn fn) { fn(); }
};
#define LOGC(...) ((void)0)
#define LOGE(...) ((void)0)
