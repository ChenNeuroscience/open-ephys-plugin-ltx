/*
	------------------------------------------------------------------

	This file is part of the Open Ephys GUI
	Copyright (C) 2022 Open Ephys

	------------------------------------------------------------------

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "LTXRecordEnginePlugin.h"
#include "util.h"
#include "LTXSharedState.h"

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace LTX {

    namespace SharedState {
        // see comment in LTXSharedState.h. These are the defaults. Any of the plugins can in principle change the values.
        std::atomic<int> window_max_x {700};
        std::atomic<int> window_min_x {0};
        std::atomic<int> window_min_y {0};
        std::atomic<int> window_max_y {700};
        std::atomic<float> pixels_per_metre {795.0};
    }

    constexpr int timestampTimebase = 96000;
    constexpr int eegInputSampRate = 30000;
    constexpr int eegOutputSampRate = 5000;
    constexpr int eegDownsampleBy = eegInputSampRate / eegOutputSampRate;
    constexpr int requiredPosChans = 7; // see assertion below for more details
    constexpr int spikesNumChans = 4;
    constexpr int spikesBytesPerChan = 4 /* 4 byte timestamp */ + 50 /* one-byte voltage for 50 samples */;
    constexpr int oeSampsPerSpike = 40; // seems to be hard-coded as 8+32 = 40
    constexpr int posTimestampChannel = 0; // provided by the bonsai source plugin
    constexpr int posNaN = 1023; // when writing pos data as uint16s, if we encounter a NaN in the source float data, we write this value.
    constexpr const char* ignore_stream_prefixed = "memory_usage";

    RecordEnginePlugin::RecordEnginePlugin() {}

    RecordEnginePlugin::~RecordEnginePlugin() {}

    RecordEngineManager* RecordEnginePlugin::getEngineManager()
    {
        RecordEngineManager* man = new RecordEngineManager("LTX", "LTX Format",
            &(engineFactory<RecordEnginePlugin>));
        return man;
    }


    void RecordEnginePlugin::openFiles(File rootFolder, int experimentNumber, int recordingNumber)
    {
        // Release any previous generation before joining this recording.
        timingSession.reset();
        setFile.reset();
        ttlFile.reset();
        posFile.reset();
        tetFiles.clear();
        eegFiles.clear();
        eegFullSampCount.clear();
        eegWrittenSamples.clear();
        eegPaddingSamples.clear();
        eegFirstDataDelay.clear();
        eegStarted.clear();
        eegWriteChannels.clear();
        recordingDelaySeconds = 0;
        timingSnapshot = Timing::Snapshot{};
        pendingWrites.clear();
        pendingBytes = 0;
        filesOpen = false;
        timingFailed = false;
        timingError.clear();
        startingTimestamp = TIMESTAMP_UNINITIALIZED;
        posFirstTimestamp = TIMESTAMP_UNINITIALIZED;
        posSampRate = 0;
        posSampCount = 0;
        posSamplesBuffer.clear();
        posStarted = false;
        posRealSampCount = posPaddingSamples = 0;
        posFirstDataDelay = 0;
        mode = RecordMode::NONE;
        basePath = rootFolder.getParentDirectory().getFullPathName().toStdString()
            + (experimentNumber == 1 ? "" : " e" + std::to_string(experimentNumber))
            + (recordingNumber == 0 ? "" : " r" + std::to_string(recordingNumber + 1));

        if (getNumRecordedSpikeChannels() > 0) {
            mode = RecordMode::SPIKES_AND_SET;
            LOGC("LTX RecordEngine using mode:SPIKES_AND_SET (", mode, ").");
        }
        else if (getNumRecordedContinuousChannels() == 0) {
            LOGE("No spikes and no continous channels, nothing to record.");
            mode = RecordMode::NONE;
        }
        else if (getContinuousChannel(getGlobalIndex(0))->getStreamName() == "bonsai") {
            mode = RecordMode::POS_ONLY; // first continuous channel is bonsai data, treat as pos
            LOGC("LTX RecordEngine using mode:POS_ONLY (", mode, ").");
            if (getNumRecordedDataStreams() > 1) { // WARNING: for some reason getTotalRecordedStreams() returns 0 when it should be 1, so this check it maybe totally broken
                LOGE("For LTX pos, require exactly 1 data stream, but found ", getNumRecordedDataStreams(), " streams.");
                failTiming("Position Record Node has more than one recorded data stream");
                return;
            }
        }
        else {
            mode = RecordMode::EEG_ONLY;
            LOGC("LTX RecordEngine using mode:EEG_ONLY (", mode, ").");
            size_t n_streams = 0;
            for (auto stream : getDataStreams()) {
                if (stream->getName().startsWith(ignore_stream_prefixed)) {
                    continue;
                }
                n_streams++;
            }
            if (n_streams != 1) {
                LOGE("For LTX eeg, require exactly 1 data stream (excluding a '", ignore_stream_prefixed, "', but found ", n_streams, " streams.");
                failTiming("EEG Record Node must have exactly one non-memory data stream");
                return;
            }
        }

        try {
            registerTiming(experimentNumber, recordingNumber);
            if (mode == RecordMode::NONE) timingSession->exclude(timingNodeId);
        } catch (const std::exception& error) {
            failTiming(error.what());
        }
    }

    void RecordEnginePlugin::openAlignedFiles()
    {
        const auto start_tm = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(*timingSnapshot.epochMs));

        if (mode == RecordMode::SPIKES_AND_SET) {
            setFile = std::make_unique<LTXFile>(basePath, ".set", start_tm);

            setFile->AddHeaderValue("lasttrialdatetime", std::chrono::duration_cast<std::chrono::seconds>(start_tm.time_since_epoch()).count());

            // need some numbers here. todo: allow configuration
            setFile->AddHeaderValue("lightBearing_1", 0);
            setFile->AddHeaderValue("lightBearing_2", 180);
            setFile->AddHeaderValue("lightBearing_3", 0);
            setFile->AddHeaderValue("lightBearing_4", 0);

            setFile->AddHeaderValue("colactive_1", 1);
            setFile->AddHeaderValue("colactive_2", 1);
            setFile->AddHeaderValue("colactive_3", 0);
            setFile->AddHeaderValue("colactive_4", 0);


            tetFiles.clear();
            tetSpikeCount.clear();
            for (int i = 0; i < getNumRecordedSpikeChannels(); i++) {
                // couple of important checks before we get going...
                const SpikeChannel* channel = getSpikeChannel(i);
                if (channel->getNumChannels() != spikesNumChans) {
                    LOGE("Expected exactly 4 channels for a spike, but found ", channel->getNumChannels());
                    throw std::runtime_error("LTX requires four channels per spike");
                }
                if (channel->getTotalSamples() != oeSampsPerSpike) {
                    LOGE("Expected exactly 40 samples per spike, which will be padded out to 50, but found ", channel->getTotalSamples());
                    throw std::runtime_error("LTX requires 40 samples per spike");
                }

                tetFiles.push_back(std::make_unique<LTXFile>(basePath, "." + std::to_string(i + 1), start_tm));
                LTXFile* f = tetFiles.back().get();
                f->AddHeaderValue("num_chans", 4);
                f->AddHeaderValue("bytes_per_timestamp", 4);
                f->AddHeaderValue("samples_per_spike", 50);
                f->AddHeaderValue("bytes_per_sample", 1);
                f->AddHeaderValue("spike_format", "t,ch1,t,ch2,t,ch3,t,ch4");
                f->AddHeaderValue("sample_rate", std::to_string(getSpikeChannel(i)->getSampleRate()) + " hz");
                f->AddHeaderValue("timebase", std::to_string(timestampTimebase) + " hz");
                f->AddHeaderPlaceholder("num_spikes");
                tetSpikeCount.push_back(0);
            }

            if (getNumRecordedEventChannels() > 0){
                ttlFile = std::make_unique<LTXFile>(basePath, ".ttl", start_tm);
            }
        }
        else if (mode == RecordMode::EEG_ONLY) {
            eegFiles.clear();
            eegFullSampCount.clear();
            eegWrittenSamples.clear();
            eegPaddingSamples.clear();
            eegFirstDataDelay.clear();
            eegStarted.clear();
            eegWriteChannels.clear();
            for (int i = 0; i < getNumRecordedContinuousChannels(); i++) {
                // important check before we get going...
                const ContinuousChannel* channel = getContinuousChannel(getGlobalIndex(i));
                if (channel->getStreamName().startsWith(ignore_stream_prefixed)) {
                    continue;
                }
                if (channel->getSampleRate() != eegInputSampRate) {
                    LOGE("Expected a sample rate of exactly ", eegInputSampRate, ", but found ", channel->getSampleRate());
                    throw std::runtime_error("LTX EEG input must be sampled at 30000 Hz");
                }

                const size_t outputChannel = eegFiles.size();
                eegWriteChannels.emplace(i, outputChannel);
                eegFiles.push_back(std::make_unique<LTXFile>(basePath, ".egf" + (outputChannel == 0 ? "" : std::to_string(outputChannel + 1)), start_tm));
                LTXFile* f = eegFiles.back().get();
                f->AddHeaderValue("num_chans", 1);
                f->AddHeaderValue("sample_rate", std::to_string(eegOutputSampRate) + " hz");
                f->AddHeaderValue("stream_name", channel->getStreamName().toStdString());
                f->AddHeaderValue("chan_name", channel->getName().toStdString());
                f->AddHeaderPlaceholder("num_EEG_samples");
                eegFullSampCount.push_back(0);
                eegWrittenSamples.push_back(0);
                eegPaddingSamples.push_back(0);
                eegFirstDataDelay.push_back(0);
                eegStarted.push_back(false);
            }
        }
        else if (mode == RecordMode::POS_ONLY) {

            if (getNumRecordedContinuousChannels() != requiredPosChans) {
                LOGE("For LTX pos, require exactly ", requiredPosChans, " float channels from Bonsai. The first is a timestamp, and then x1,y1,x2,y2,numpix1,numpix2. ",
                    "However, received ", getNumRecordedContinuousChannels(), " channels.");
                throw std::runtime_error("LTX position requires seven recorded channels");
            }


            posFile = std::make_unique<LTXFile>(basePath, ".pos", start_tm);

            posSampRate = getContinuousChannel(getGlobalIndex(0))->getSampleRate();
            posFile->AddHeaderValue("timestamp_timebase", std::to_string(timestampTimebase) + " hz");
            posFile->AddHeaderValue("timebase", std::to_string(posSampRate) + " hz"); // required by Waveform GUI at least, maybe other code
            posFile->AddHeaderValue("sample_rate", std::to_string(posSampRate) + " hz");

            posFile->AddHeaderValue("bytes_per_timestamp", 4);
            posFile->AddHeaderValue("bytes_per_coord", 2);

            posFile->AddHeaderValue("pos_format", "t,x1,y1,x2,y2,numpix1,numpix2");

            // no idea if this is needed for anything. dummy values...
            posFile->AddHeaderValue("num_colours", 4);
            posFile->AddHeaderValue("bearing_colour_1", 330);
            posFile->AddHeaderValue("bearing_colour_2", 150);
            posFile->AddHeaderValue("bearing_colour_3", 0);
            posFile->AddHeaderValue("bearing_colour_4", 0);

            // not actually sure what they are supposed to mean. Maybe some of them
            // might need to be computed based on the observed data and set at the end of the trial rather than configured by the user?
            posFile->AddHeaderValue("pixels_per_metre", static_cast<double>(LTX::SharedState::pixels_per_metre.load()));
            posFile->AddHeaderValue("window_min_x", LTX::SharedState::window_min_x.load());
            posFile->AddHeaderValue("window_max_x", LTX::SharedState::window_max_x.load());
            posFile->AddHeaderValue("window_min_y", LTX::SharedState::window_min_y.load());
            posFile->AddHeaderValue("window_max_y", LTX::SharedState::window_max_y.load());
            posFile->AddHeaderValue("min_x", LTX::SharedState::window_min_x.load());
            posFile->AddHeaderValue("max_x", LTX::SharedState::window_max_x.load());
            posFile->AddHeaderValue("min_y", LTX::SharedState::window_min_y.load());
            posFile->AddHeaderValue("max_y", LTX::SharedState::window_max_y.load());

            posFile->AddHeaderPlaceholder("num_pos_samples");
            posSampCount = 0;
            posFirstTimestamp = TIMESTAMP_UNINITIALIZED; // gets initialised using the first continuous data below
        }

        if (setFile) addTimingHeaders(*setFile);
        if (ttlFile) addTimingHeaders(*ttlFile);
        for (auto& file : tetFiles) addTimingHeaders(*file);
        for (auto& file : eegFiles) addTimingHeaders(*file);
        if (posFile) addTimingHeaders(*posFile);
        filesOpen = true;


    }

    void RecordEnginePlugin::registerTiming(int experimentNumber, int recordingNumber)
    {
        std::set<int> expected;
        timingNodeId = -1;
        for (int id : CoreServices::getAvailableRecordNodeIds()) {
            if (CoreServices::RecordNode::getRecordEngineId(id) != "LTX") continue;
            auto* processor = CoreServices::getProcessorById(static_cast<uint16_t>(id));
            if (!processor) throw std::runtime_error("LTX Record Node disappeared at recording start");
            expected.insert(id);
            // RecordEngine exposes a forward-declared RecordNode pointer. A
            // dynamic_cast<void*> gives the complete object's address without
            // relying on its inheritance layout or private GUI headers.
            if (dynamic_cast<const void*>(processor) == static_cast<const void*>(recordNode)) timingNodeId = id;
        }
        if (timingNodeId < 0) throw std::runtime_error("Cannot identify this LTX Record Node");
        const std::string key = CoreServices::getRecordingDirectoryName().toStdString()
            + "/" + std::to_string(experimentNumber) + "/" + std::to_string(recordingNumber);
        const char* name = mode == POS_ONLY ? "position" : mode == EEG_ONLY ? "eeg"
            : mode == SPIKES_AND_SET ? "spikes" : "inactive";
        timingSession = Timing::join(key, expected, timingNodeId, name);
    }

    void RecordEnginePlugin::failTiming(const std::string& message)
    {
        if (timingFailed) return;
        timingFailed = true;
        timingError = message;
        if (timingSession) timingSession->fail(message);
        pendingWrites.clear();
        pendingBytes = 0;
        const std::string warning = "LTX timing ERROR: " + message + ". Recording stopped; see timing metadata.";
        LOGE(warning);
        CoreServices::sendStatusMessage(warning.c_str());
        // Never wait for the GUI from the disk-writing thread.
        MessageManager::callAsync([] { CoreServices::setRecordingStatus(false); });
    }

    bool RecordEnginePlugin::reservePending(size_t bytes)
    {
        if (timingFailed) return false;
        if (bytes > maximumPendingBytes - pendingBytes) {
            failTiming("LTX start origins unavailable before the 256 MiB staging limit");
            return false;
        }
        pendingBytes += bytes;
        return true;
    }

    bool RecordEnginePlugin::flushPendingWrites()
    {
        if (timingFailed || mode == NONE || !timingSession) return false;
        try {
            if (timingSession->hasFailed()) throw std::runtime_error(timingSession->snapshot().error);
            if (!filesOpen) {
                timingSnapshot = timingSession->snapshot();
                if (!timingSnapshot.error.empty()) throw std::runtime_error(timingSnapshot.error);
                if (!timingSnapshot.epochMs || startingTimestamp == TIMESTAMP_UNINITIALIZED) return false;
                recordingDelaySeconds = timingSnapshot.delaySeconds(timingNodeId);
                // Validate before allocating/writing padding in any format.
                Timing::paddingSamples(recordingDelaySeconds, 1.0);
                openAlignedFiles();
                writeTimingMetadata(false);
            }
            while (!pendingWrites.empty()) {
                auto write = std::move(pendingWrites.front());
                pendingWrites.pop_front();
                write();
            }
            pendingBytes = 0;
            return true;
        } catch (const std::exception& error) {
            failTiming(error.what());
            return false;
        }
    }

    void RecordEnginePlugin::addTimingHeaders(LTXFile& file)
    {
        file.AddHeaderValue("timing_method", "oe_first_recording_block_wall_clock");
        file.AddHeaderValue("recording_epoch_unix_ms", static_cast<long long>(*timingSnapshot.epochMs));
        file.AddHeaderValue("record_node_id", timingNodeId);
        file.AddHeaderValue("record_node_start_offset_ms", recordingDelaySeconds * 1000.0);
        file.AddHeaderValue("startup_alignment_estimated", 1);
        if (mode == SPIKES_AND_SET) {
            file.AddHeaderValue("missing_start_seconds", recordingDelaySeconds);
            file.AddHeaderValue("timestamp_start_offset_seconds", recordingDelaySeconds);
        }
    }

    void RecordEnginePlugin::addPaddingHeaders(LTXFile& file, uint64 count, double firstDataDelay, double rate)
    {
        file.AddHeaderValue("startup_padding_samples", static_cast<long long>(count));
        file.AddHeaderValue("startup_first_data_offset_ms", firstDataDelay * 1000.0);
        file.AddHeaderValue("startup_padding_residual_ms", (firstDataDelay - double(count) / rate) * 1000.0);
        file.AddHeaderValue("startup_padding_value", mode == POS_ONLY ? "coords_1023_pixels_0" : "0");
    }

    static std::string jsonString(const std::string& value)
    {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << char(c);
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
            else out << char(c);
        }
        out << '"';
        return out.str();
    }

    void RecordEnginePlugin::writeTimingMetadata(bool complete)
    {
        const auto snapshot = timingSession ? timingSession->snapshot() : Timing::Snapshot{};
        const std::string path = basePath + ".timing-" + std::to_string(timingNodeId) + ".json";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write LTX timing metadata: " + path);
        out << std::setprecision(17)
            << "{\n  \"schema_version\": 1,\n  \"node_id\": " << timingNodeId
            << ",\n  \"complete\": " << (complete ? "true" : "false")
            << ",\n  \"method\": \"oe_first_recording_block_wall_clock\",\n"
            << "  \"alignment_estimated\": true,\n  \"error\": " << jsonString(timingError.empty() ? snapshot.error : timingError)
            << ",\n  \"common_epoch_unix_ms\": ";
        if (snapshot.epochMs) out << *snapshot.epochMs; else out << "null";
        out << ",\n  \"record_node_offset_seconds\": " << recordingDelaySeconds
            << ",\n  \"warning_threshold_ms\": " << Timing::warningThresholdMs
            << ",\n  \"position_padding_samples\": " << posPaddingSamples
            << ",\n  \"position_first_data_offset_seconds\": " << posFirstDataDelay
            << ",\n  \"position_sample_rate\": " << posSampRate
            << ",\n  \"position_applied_offset_seconds\": " << (posSampRate ? double(posPaddingSamples) / posSampRate : 0)
            << ",\n  \"eeg_channels\": [";
        for (size_t i = 0; i < eegPaddingSamples.size(); ++i) {
            if (i) out << ',';
            out << "{\"channel\":" << i + 1 << ",\"padding_samples\":" << eegPaddingSamples[i]
                << ",\"first_data_offset_seconds\":" << eegFirstDataDelay[i]
                << ",\"applied_offset_seconds\":" << double(eegPaddingSamples[i]) / eegOutputSampRate
                << ",\"sample_rate\":" << eegOutputSampRate
                << ",\"written_samples\":" << eegWrittenSamples[i] << '}';
        }
        out << "],\n  \"participants\": [";
        bool first = true;
        for (const auto& entry : snapshot.starts) {
            if (!first) out << ',';
            first = false;
            out << "{\"node_id\":" << entry.first << ",\"first_block_unix_ms\":";
            if (entry.second) out << *entry.second; else out << "null";
            const auto modeName = snapshot.modes.find(entry.first);
            out << ",\"mode\":" << jsonString(modeName == snapshot.modes.end() ? "unregistered" : modeName->second)
                << ",\"inactive\":" << (snapshot.inactive.count(entry.first) ? "true" : "false") << '}';
        }
        out << "]\n}\n";
        out.close();
        if (!out) throw std::runtime_error("Failed writing LTX timing metadata: " + path);
    }

    void RecordEnginePlugin::closeFiles()
    {
        if (timingSession && mode != NONE) {
            auto snapshot = timingSession->snapshot();
            if (!snapshot.starts.at(timingNodeId)) {
                if (pendingWrites.empty()) {
                    timingSession->exclude(timingNodeId);
                    mode = NONE; // No recording callback or data ever arrived.
                }
                else failTiming("Recording ended without an Open Ephys software-time anchor");
            }
            // Only close may wait: each other engine has its own disk thread.
            // Data callbacks must return to allow the sync messages to arrive.
            if (mode != NONE && !timingFailed && !timingSession->waitForOrigins(std::chrono::milliseconds(250)))
                failTiming("Recording ended before all LTX start origins arrived");
            flushPendingWrites();
            if (mode != NONE && !filesOpen && !timingFailed)
                failTiming("Recording ended without a valid LTX stream origin");
        }
        const auto end_tm = std::chrono::system_clock::now();
        try {
            if (filesOpen && mode == SPIKES_AND_SET) {
                setFile->FinaliseFile(end_tm);
                for (size_t i = 0; i < tetFiles.size(); i++) {
                    tetFiles[i]->FinaliseHeaderPlaceholder(static_cast<uint64_t>(tetSpikeCount[i]));
                    tetFiles[i]->FinaliseFile(end_tm);
                }
                if (ttlFile) ttlFile->FinaliseFile(end_tm);
            } else if (filesOpen && mode == EEG_ONLY) {
                for (size_t i = 0; i < eegFiles.size(); i++) {
                    if (!eegStarted[i]) addPaddingHeaders(*eegFiles[i], 0, 0, eegOutputSampRate);
                    eegFiles[i]->FinaliseHeaderPlaceholder(static_cast<uint64_t>(eegWrittenSamples[i]));
                    eegFiles[i]->FinaliseFile(end_tm);
                }
            } else if (filesOpen && mode == POS_ONLY) {
                if (!posStarted) addPaddingHeaders(*posFile, 0, 0, posSampRate);
                posFile->FinaliseHeaderPlaceholder(static_cast<uint64_t>(posSampCount));
                posFile->FinaliseFile(end_tm);
            }
            writeTimingMetadata(!timingFailed);
        } catch (const std::exception& error) {
            failTiming(error.what());
        }
        pendingWrites.clear();
        pendingBytes = 0;
        filesOpen = false;
        LOGC("Completed writing LTX files.");
    }

    void RecordEnginePlugin::writeContinuousData(int writeChannel, int realChannel,
                                                const float* data, const double* timestamps, int size)
    {
        if (size <= 0 || mode == NONE) return;
        if (mode == SPIKES_AND_SET) { flushPendingWrites(); return; }
        if (getContinuousChannel(realChannel)->getStreamName().startsWith(ignore_stream_prefixed)) return;
        const size_t bytes = size_t(size) * (sizeof(float) + sizeof(double));
        if (!reservePending(bytes)) return;
        pendingWrites.emplace_back([this, writeChannel, realChannel,
                                    values = std::vector<float>(data, data + size),
                                    times = std::vector<double>(timestamps, timestamps + size)] {
            writeAlignedContinuous(writeChannel, realChannel, values.data(), times.data(), int(values.size()));
        });
        flushPendingWrites();
    }

    void RecordEnginePlugin::beginPosition(double firstDataDelay)
    {
        posFirstDataDelay = firstDataDelay;
        posPaddingSamples = Timing::paddingSamples(firstDataDelay, posSampRate);
        addPaddingHeaders(*posFile, posPaddingSamples, firstDataDelay, posSampRate);
        std::array<PosSample, 512> padding{};
        for (auto& sample : padding)
            for (int coordinate = 0; coordinate < 4; ++coordinate) sample.xy_etc[coordinate] = BSWAP16(posNaN);
        for (uint64 at = 0; at < posPaddingSamples;) {
            const size_t count = size_t(std::min<uint64>(padding.size(), posPaddingSamples - at));
            for (size_t i = 0; i < count; ++i)
                padding[i].timestamp = BSWAP32(static_cast<int32_t>(std::llround(double(at + i) / posSampRate * timestampTimebase)));
            posFile->WriteBinaryData(padding.data(), count * sizeof(PosSample));
            at += count;
        }
        posSampCount = posPaddingSamples;
        posStarted = true;
    }

    void RecordEnginePlugin::writeAlignedContinuous(int writeChannel, int realChannel,
                                                    const float* data, const double* timestamps, int size)
    {
        // A complete block can precede its sync text in the GUI callback order.
        // Staging ensures the origin is known before filtering any samples.
        while (size > 0 && timestamps[0] < startingTimestamp) {
            --size;
            ++timestamps;
            ++data;
        }
        if (size <= 0) return;
        if (!std::isfinite(timestamps[0])) throw std::runtime_error("Invalid LTX continuous-data timestamp");
        if (mode == EEG_ONLY) {
            const size_t channel = eegWriteChannels.at(writeChannel);
            if (!eegStarted[channel]) {
                const double delay = recordingDelaySeconds + std::max(0.0, timestamps[0] - startingTimestamp);
                const uint64 count = Timing::paddingSamples(delay, eegOutputSampRate);
                eegPaddingSamples[channel] = count;
                eegFirstDataDelay[channel] = delay;
                addPaddingHeaders(*eegFiles[channel], count, delay, eegOutputSampRate);
                std::array<int8_t, 1024> zeros{};
                for (uint64 at = 0; at < count;) {
                    const size_t block = size_t(std::min<uint64>(zeros.size(), count - at));
                    eegFiles[channel]->WriteBinaryData(zeros.data(), block);
                    at += block;
                }
                eegWrittenSamples[channel] = count;
                eegStarted[channel] = true;
            }
            // Padding is at the output rate and must not change the input
            // downsampling phase. Work in bounded chunks for large input blocks.
            constexpr int capacity = 1024;
            int8_t output[capacity];
            const uint64 remainder = eegFullSampCount[channel] % eegDownsampleBy;
            int offset = remainder == 0 ? 0 : eegDownsampleBy - int(remainder);
            while (offset < size) {
                const int inputCount = std::min(size - offset, capacity * eegDownsampleBy);
                const int64 count = float32sToInt8sDownsampled<capacity, -250, 250, eegDownsampleBy>(
                    data + offset, output, inputCount);
                eegFiles[channel]->WriteBinaryData(output, size_t(count));
                eegWrittenSamples[channel] += count;
                offset += capacity * eegDownsampleBy;
            }
            eegFullSampCount[channel] += size;
        } else if (mode == POS_ONLY) {
            if (writeChannel == posTimestampChannel) {
                if (!posStarted)
                    beginPosition(recordingDelaySeconds + std::max(0.0, timestamps[0] - startingTimestamp));
                if (posFirstTimestamp == TIMESTAMP_UNINITIALIZED) {
                    for (int i = 0; i < size; ++i) {
                        if (std::isfinite(data[i])) {
                            if (data[i] > 4 * 60 * 60)
                                throw std::runtime_error("Bonsai float timestamps exceed four hours; restart acquisition");
                            // Missing samples at the head need valid grid timestamps
                            // even if the first camera timestamp is NaN.
                            posFirstTimestamp = data[i] - double(posRealSampCount + i) / posSampRate;
                            break;
                        }
                    }
                }
                posSamplesBuffer.assign(size, PosSample{});
                for (int i = 0; i < size; ++i) {
                    const double localTime = std::isfinite(data[i])
                        ? data[i] - posFirstTimestamp : double(posRealSampCount + i) / posSampRate;
                    const double time = localTime + double(posPaddingSamples) / posSampRate;
                    if (!std::isfinite(time) || time < 0 || time * timestampTimebase > std::numeric_limits<int32_t>::max())
                        throw std::runtime_error("Position timestamp outside the LTX int32 timebase");
                    posSamplesBuffer[i].timestamp = BSWAP32(static_cast<int32_t>(std::llround(time * timestampTimebase)));
                }
                posSampCount += size;
                posRealSampCount += size;
            } else {
                if (writeChannel < 1 || writeChannel >= requiredPosChans || posSamplesBuffer.size() != size_t(size))
                    throw std::runtime_error("Inconsistent LTX position channel order or block sizes");
                for (int i = 0; i < size; ++i)
                    posSamplesBuffer[i].xy_etc[writeChannel - 1] = BSWAP16(
                        std::isnan(data[i]) ? (writeChannel <= 4 ? posNaN : 0) : static_cast<uint16_t>(data[i]));
                if (writeChannel == requiredPosChans - 1)
                    posFile->WriteBinaryData(posSamplesBuffer.data(), sizeof(PosSample) * size);
            }
        }
    }

    void RecordEnginePlugin::writeEvent(int eventIndex, const EventPacket& event)
    {
        if (mode != SPIKES_AND_SET || timingFailed) return;
        const EventChannel* info = getEventChannel(eventIndex);
        EventPtr decoded = Event::deserialize(event, info);
        if (decoded->getEventType() != EventChannel::TTL) return;
        auto* ttl = static_cast<TTLEvent*>(decoded.get());
        const double timestamp = ttl->getTimestampInSeconds();
        const int line = ttl->getLine();
        const bool state = ttl->getState();
        if (!reservePending(sizeof(double) + sizeof(int) + sizeof(bool))) return;
        pendingWrites.emplace_back([this, timestamp, line, state] { writeAlignedEvent(timestamp, line, state); });
        flushPendingWrites();
    }

    void RecordEnginePlugin::writeAlignedEvent(double timestamp, int line, bool state)
    {
        if (!ttlFile || timestamp < startingTimestamp) return;
        if (!std::isfinite(timestamp)) throw std::runtime_error("Invalid LTX TTL timestamp");
        std::ostringstream text;
        text << std::setprecision(17) << "ttl_" << line + 1 << " "
             << timestamp - startingTimestamp + recordingDelaySeconds << " " << (state ? '1' : '0') << "\r\n";
        auto bytes = text.str();
        ttlFile->WriteBinaryData(bytes.data(), bytes.size());
    }

    void RecordEnginePlugin::writeSpike(int electrodeIndex, const Spike* spike)
    {
        if (mode != SPIKES_AND_SET || timingFailed) return;
        std::array<int8, spikesBytesPerChan * spikesNumChans> bytes{};
        const float* voltages = spike->getDataPointer();
        for (int channel = 0; channel < spikesNumChans; ++channel)
            float32sToInt8s<oeSampsPerSpike, -125, 125>(voltages + channel * oeSampsPerSpike,
                bytes.data() + channel * spikesBytesPerChan + 4);
        const double timestamp = spike->getTimestampInSeconds();
        if (!reservePending(bytes.size() + sizeof(double))) return;
        pendingWrites.emplace_back([this, electrodeIndex, timestamp, bytes] { writeAlignedSpike(electrodeIndex, timestamp, bytes); });
        flushPendingWrites();
    }

    void RecordEnginePlugin::writeAlignedSpike(int electrodeIndex, double timestamp,
                                               std::array<int8, 216> bytes)
    {
        if (timestamp < startingTimestamp) return;
        const double seconds = timestamp - startingTimestamp + recordingDelaySeconds;
        if (!std::isfinite(seconds) || seconds * timestampTimebase > std::numeric_limits<int32_t>::max())
            throw std::runtime_error("Spike timestamp outside the LTX int32 timebase");
        const int32_t ticks = BSWAP32(static_cast<int32_t>(std::llround(seconds * timestampTimebase)));
        for (int channel = 0; channel < spikesNumChans; ++channel)
            std::memcpy(bytes.data() + channel * spikesBytesPerChan, &ticks, 4);
        tetFiles.at(electrodeIndex)->WriteBinaryData(bytes.data(), bytes.size());
        ++tetSpikeCount.at(electrodeIndex);
    }

    void RecordEnginePlugin::writeTimestampSyncText(DataStream* stream, int64 sampleNumber,
                                                   double timestamp, String text)
    {
        if (mode == NONE || timingFailed || !timingSession) return;
        if (stream == nullptr) {
            if (text.startsWith("Software Time (")) {
                // The sampleNumber parameter carries Unix milliseconds for this
                // message. The GUI's timestamp parameter is zero for null streams.
                timingSession->publish(timingNodeId, sampleNumber);
                for (const auto& warning : timingSession->takeWarnings()) {
                    std::ostringstream message;
                    message << "LTX timing: Record Node " << warning.first << " started " << warning.second
                            << " ms after the earliest recorder. Applying startup padding/timestamp correction"
                            << " (host-clock estimate); see .timing-*.json.";
                    LOGC(message.str());
                    CoreServices::sendStatusMessage(message.str().c_str());
                }
            }
        } else if (!stream->getName().startsWith(ignore_stream_prefixed)) {
            if (startingTimestamp != TIMESTAMP_UNINITIALIZED || !std::isfinite(timestamp) || timestamp < 0) {
                failTiming("Invalid or repeated LTX stream recording origin");
                return;
            }
            startingTimestamp = timestamp;
        }
        flushPendingWrites();
    }

    void RecordEnginePlugin::setParameter(EngineParameter& parameter)
    {
    }

}
