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
        eegWriteChannels.clear();
        pendingWrites.clear();
        pendingBytes = 0;
        filesOpen = false;
        receivedRecordingData = false;
        recordingFailed = false;
        timingError.clear();
        startingTimestamp = TIMESTAMP_UNINITIALIZED;
        posFirstTimestamp = TIMESTAMP_UNINITIALIZED;
        posSampRate = 0;
        posSampCount = 0;
        positionAssembler = PositionAssembler{};
        cameraMetadata = CameraMetadata{};
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
                failRecording("Position Record Node has more than one recorded data stream");
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
                failRecording("EEG Record Node must have exactly one non-memory data stream");
                return;
            }
        }

        try {
            registerTiming(experimentNumber, recordingNumber);
            if (mode == RecordMode::NONE) timingSession->exclude(timingNodeId);
            announceTimingWarnings();
        } catch (const std::exception& error) {
            timingError = error.what(); // Diagnostics must never prevent recording.
        }
        try {
            openOutputFiles();
        } catch (const std::exception& error) {
            failRecording(error.what());
        }
    }

    void RecordEnginePlugin::openOutputFiles()
    {
        const auto start_tm = std::chrono::system_clock::now();

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
                f->ReserveHeader("num_spikes", "0");
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
                f->ReserveHeader("num_EEG_samples", "0");
                eegFullSampCount.push_back(0);
                eegWrittenSamples.push_back(0);
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

            // Capture the matching stream directly, independently of the viewer.
            const auto streamId = getContinuousChannel(getGlobalIndex(0))->getStreamId();
            for (const DataStream* stream : getDataStreams())
                if (stream->getStreamId() == streamId) cameraMetadata = readCameraMetadata(stream);
            positionEncoding = PositionEncoding(cameraMetadata);
            const bool metadataWindow = cameraMetadata.validWindow();
            const int left = metadataWindow ? 0 : SharedState::window_min_x.load();
            const int top = metadataWindow ? 0 : SharedState::window_min_y.load();
            const int right = metadataWindow ? int(std::llround((double(cameraMetadata.window[2]) - cameraMetadata.window[0]) * positionEncoding.scale))
                                             : SharedState::window_max_x.load();
            const int bottom = metadataWindow ? int(std::llround((double(cameraMetadata.window[3]) - cameraMetadata.window[1]) * positionEncoding.scale))
                                              : SharedState::window_max_y.load();
            // An explicitly uncalibrated camera must not inherit stale viewer PPM.
            const double ppm = cameraMetadata.present
                ? (cameraMetadata.calibrated() ? cameraMetadata.ppm * positionEncoding.scale : std::numeric_limits<double>::quiet_NaN())
                : double(SharedState::pixels_per_metre.load());
            posFile->AddHeaderValue("position_metadata_source", cameraMetadata.present ? "camera_tracker" : "legacy_viewer");
            if (cameraMetadata.present)
                posFile->AddHeaderValue("data_source", "open-ephys-plugin-camera v0.1");
            posFile->AddHeaderValue("pixels_per_metre", ppm);
            posFile->AddHeaderValue("window_min_x", left);
            posFile->AddHeaderValue("window_max_x", right);
            posFile->AddHeaderValue("window_min_y", top);
            posFile->AddHeaderValue("window_max_y", bottom);
            posFile->AddHeaderValue("min_x", left);
            posFile->AddHeaderValue("max_x", right);
            posFile->AddHeaderValue("min_y", top);
            posFile->AddHeaderValue("max_y", bottom);
            if (cameraMetadata.present) {
                if (cameraMetadata.hasPpm) posFile->AddHeaderValue("camera_tracker.pixels_per_metre", cameraMetadata.ppm);
                if (cameraMetadata.hasWindow) {
                    std::ostringstream values;
                    for (auto value : cameraMetadata.window) values << value << ' ';
                    posFile->AddHeaderValue("camera_tracker.window", values.str());
                }
                if (cameraMetadata.hasCrop) posFile->AddHeaderValue("camera_tracker.crop", cameraMetadata.cropText);
                if (cameraMetadata.hasFrame) {
                    posFile->AddHeaderValue("camera_tracker.frame_size", std::to_string(cameraMetadata.frame[0]) + " " + std::to_string(cameraMetadata.frame[1]));
                }
                posFile->AddHeaderValue("position_coordinate_transform", positionEncoding.transformed ? "crop_relative_uniform_scale" : "identity");
                posFile->AddHeaderValue("position_coordinate_origin_x", positionEncoding.originX);
                posFile->AddHeaderValue("position_coordinate_origin_y", positionEncoding.originY);
                posFile->AddHeaderValue("position_coordinate_scale", positionEncoding.scale);
                posFile->AddHeaderValue("position_missing_coordinate", 1023);
                posFile->AddHeaderValue("position_area_units", "camera_pixels_saturated_uint16");
                if (!cameraMetadata.validWindow())
                    CoreServices::sendStatusMessage("LTX camera warning: stream crop bounds unavailable; using legacy coordinate mapping and viewer bounds.");
            }

            posFile->ReserveHeader("num_pos_samples", "0");
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

    void RecordEnginePlugin::failRecording(const std::string& message)
    {
        if (recordingFailed) return;
        recordingFailed = true;
        timingError = message;
        pendingWrites.clear();
        pendingBytes = 0;
        const std::string warning = "LTX recording ERROR: " + message + ". Recording stopped; see timing metadata.";
        LOGE(warning);
        CoreServices::sendStatusMessage(warning.c_str());
        // Never wait for the GUI from the disk-writing thread.
        MessageManager::callAsync([] { CoreServices::setRecordingStatus(false); });
    }

    bool RecordEnginePlugin::reservePending(size_t bytes)
    {
        if (recordingFailed) return false;
        if (bytes > maximumPendingBytes - pendingBytes) {
            failRecording("LTX native stream origin unavailable before the 256 MiB staging limit");
            return false;
        }
        pendingBytes += bytes;
        return true;
    }

    bool RecordEnginePlugin::flushPendingWrites()
    {
        if (recordingFailed || mode == NONE || !filesOpen) return false;
        try {
            if (startingTimestamp == TIMESTAMP_UNINITIALIZED) return false;
            while (!pendingWrites.empty()) {
                auto write = std::move(pendingWrites.front());
                pendingWrites.pop_front();
                write();
            }
            pendingBytes = 0;
            return true;
        } catch (const std::exception& error) {
            failRecording(error.what());
            return false;
        }
    }

    void RecordEnginePlugin::addTimingHeaders(LTXFile& file)
    {
        file.AddHeaderValue("timing_method", "oe_first_recording_block_wall_clock");
        file.AddHeaderValue("record_node_id", timingNodeId);
        file.AddHeaderValue("startup_timing_estimated", 1);
        file.AddHeaderValue("startup_timing_correction_applied", 0);
        for (const char* key : {"recording_epoch_unix_ms", "record_node_start_unix_ms",
                                "record_node_start_offset_ms", "startup_timing_status"})
            file.ReserveHeader(key);
    }

    void RecordEnginePlugin::announceTimingWarnings()
    {
        if (!timingSession) return;
        for (const auto& warning : timingSession->takeWarnings()) {
            std::ostringstream message;
            message << "LTX timing: Record Node " << warning.first << " started " << warning.second
                    << " ms after the earliest recorder (host-clock estimate). No correction applied;"
                    << " discrepancy recorded in file headers and .timing-*.json.";
            LOGC(message.str());
            CoreServices::sendStatusMessage(message.str().c_str());
        }
    }

    void RecordEnginePlugin::updateTimingHeaders()
    {
        const auto timingSnapshot = timingSession ? timingSession->snapshot() : Timing::Snapshot{};
        const auto own = timingSnapshot.starts.find(timingNodeId);
        const bool available = timingSnapshot.epochMs && timingSnapshot.error.empty()
            && own != timingSnapshot.starts.end() && own->second;
        const auto update = [&](LTXFile& file) {
            file.UpdateHeader("recording_epoch_unix_ms", available ? std::to_string(*timingSnapshot.epochMs) : "unavailable");
            file.UpdateHeader("record_node_start_unix_ms", own != timingSnapshot.starts.end() && own->second
                ? std::to_string(*own->second) : "unavailable");
            file.UpdateHeader("record_node_start_offset_ms", available
                ? std::to_string(*own->second - *timingSnapshot.epochMs) : "unavailable");
            file.UpdateHeader("startup_timing_status", available ? "measured_uncorrected" : "unavailable");
        };
        if (setFile) update(*setFile);
        if (ttlFile) update(*ttlFile);
        for (auto& file : tetFiles) update(*file);
        for (auto& file : eegFiles) update(*file);
        if (posFile) update(*posFile);
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
            << "  \"estimated\": true,\n  \"correction_applied\": false,\n  \"error\": " << jsonString(timingError.empty() ? snapshot.error : timingError)
            << ",\n  \"common_epoch_unix_ms\": ";
        if (snapshot.epochMs && snapshot.error.empty()) out << *snapshot.epochMs; else out << "null";
        const auto own = snapshot.starts.find(timingNodeId);
        const bool available = snapshot.epochMs && snapshot.error.empty()
            && own != snapshot.starts.end() && own->second;
        out << ",\n  \"timing_available\": " << (available ? "true" : "false");
        out << ",\n  \"record_node_offset_seconds\": ";
        if (snapshot.epochMs && snapshot.error.empty() && own != snapshot.starts.end() && own->second)
            out << snapshot.delaySeconds(timingNodeId);
        else out << "null";
        out << ",\n  \"warning_threshold_ms\": " << Timing::warningThresholdMs
            << ",\n  \"participants\": [";
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
            const auto snapshot = timingSession->snapshot();
            if (!snapshot.starts.at(timingNodeId) && !receivedRecordingData)
                timingSession->exclude(timingNodeId);
        }
        flushPendingWrites();
        if (!pendingWrites.empty() && !recordingFailed)
            failRecording("Recording ended without a valid LTX stream origin");
        if (receivedRecordingData && (!timingSession || !timingSession->snapshot().epochMs
                            || !timingSession->snapshot().error.empty())) {
            CoreServices::sendStatusMessage("LTX timing warning: startup discrepancy unavailable; recording data was not adjusted.");
        }
        if (mode == POS_ONLY && !recordingFailed) {
            try { positionAssembler.finish(); }
            catch (const std::exception& error) { failRecording(error.what()); }
        }
        announceTimingWarnings();
        const auto end_tm = std::chrono::system_clock::now();
        try {
            if (filesOpen) updateTimingHeaders();
            if (filesOpen && mode == SPIKES_AND_SET) {
                setFile->FinaliseFile(end_tm);
                for (size_t i = 0; i < tetFiles.size(); i++) {
                    tetFiles[i]->UpdateHeader("num_spikes", tetSpikeCount[i]);
                    tetFiles[i]->FinaliseFile(end_tm);
                }
                if (ttlFile) ttlFile->FinaliseFile(end_tm);
            } else if (filesOpen && mode == EEG_ONLY) {
                for (size_t i = 0; i < eegFiles.size(); i++) {
                    eegFiles[i]->UpdateHeader("num_EEG_samples", eegWrittenSamples[i]);
                    eegFiles[i]->FinaliseFile(end_tm);
                }
            } else if (filesOpen && mode == POS_ONLY) {
                posFile->UpdateHeader("num_pos_samples", posSampCount);
                posFile->FinaliseFile(end_tm);
            }
        } catch (const std::exception& error) {
            failRecording(error.what());
        }
        try {
            writeTimingMetadata(!recordingFailed);
        } catch (const std::exception& error) {
            LOGE(error.what());
            CoreServices::sendStatusMessage("LTX timing warning: could not save timing JSON; see data-file headers.");
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
        if (mode == SPIKES_AND_SET) return;
        if (getContinuousChannel(realChannel)->getStreamName().startsWith(ignore_stream_prefixed)) return;
        receivedRecordingData = true;
        if (writeImmediately([&] { writeLocalContinuous(writeChannel, realChannel, data, timestamps, size); })) return;
        const size_t bytes = size_t(size) * (sizeof(float) + sizeof(double));
        if (!reservePending(bytes)) return;
        pendingWrites.emplace_back([this, writeChannel, realChannel,
                                    values = std::vector<float>(data, data + size),
                                    times = std::vector<double>(timestamps, timestamps + size)] {
            writeLocalContinuous(writeChannel, realChannel, values.data(), times.data(), int(values.size()));
        });
    }

    void RecordEnginePlugin::writeLocalContinuous(int writeChannel, int realChannel,
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
            // Preserve input downsampling phase across blocks.
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
            writePositionChannel(writeChannel, data, timestamps, size);
        }
    }

    void RecordEnginePlugin::writePositionChannel(int channel, const float* data, const double* timestamps, int size)
    {
        positionAssembler.append(channel, data, timestamps, size);
        std::array<PosSample, 512> output{};
        std::size_t used = 0;
        PositionAssembler::Frame frame;
        while (positionAssembler.pop(frame)) {
            const double cameraTime = frame[posTimestampChannel];
            if (posFirstTimestamp == TIMESTAMP_UNINITIALIZED && std::isfinite(cameraTime)) {
                if (cameraTime > 4 * 60 * 60)
                    throw std::runtime_error("Bonsai float timestamps exceed four hours; restart acquisition");
                posFirstTimestamp = cameraTime - double(posSampCount) / posSampRate;
            }
            const double time = std::isfinite(cameraTime)
                ? cameraTime - posFirstTimestamp : double(posSampCount) / posSampRate;
            if (!std::isfinite(time) || time < 0 || time * timestampTimebase > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("Position timestamp outside the LTX int32 timebase");
            auto& sample = output[used++];
            sample.timestamp = BSWAP32(static_cast<int32_t>(std::llround(time * timestampTimebase)));
            for (int value = 1; value < requiredPosChans; ++value) {
                const uint16_t encoded = value <= 4
                    ? positionEncoding.coordinate(frame[value], value == 1 || value == 3)
                    : positionEncoding.area(frame[value]);
                sample.xy_etc[value - 1] = BSWAP16(encoded);
            }
            ++posSampCount;
            if (used == output.size()) {
                posFile->WriteBinaryData(output.data(), used * sizeof(PosSample));
                used = 0;
            }
        }
        if (used) posFile->WriteBinaryData(output.data(), used * sizeof(PosSample));
    }

    void RecordEnginePlugin::writeEvent(int eventIndex, const EventPacket& event)
    {
        if (mode != SPIKES_AND_SET || recordingFailed) return;
        receivedRecordingData = true;
        const EventChannel* info = getEventChannel(eventIndex);
        EventPtr decoded = Event::deserialize(event, info);
        if (decoded->getEventType() != EventChannel::TTL) return;
        auto* ttl = static_cast<TTLEvent*>(decoded.get());
        const double timestamp = ttl->getTimestampInSeconds();
        const int line = ttl->getLine();
        const bool state = ttl->getState();
        if (writeImmediately([&] { writeLocalEvent(timestamp, line, state); })) return;
        if (!reservePending(sizeof(double) + sizeof(int) + sizeof(bool))) return;
        pendingWrites.emplace_back([this, timestamp, line, state] { writeLocalEvent(timestamp, line, state); });
    }

    void RecordEnginePlugin::writeLocalEvent(double timestamp, int line, bool state)
    {
        if (!ttlFile || timestamp < startingTimestamp) return;
        if (!std::isfinite(timestamp)) throw std::runtime_error("Invalid LTX TTL timestamp");
        std::ostringstream text;
        text << std::setprecision(17) << "ttl_" << line + 1 << " "
             << timestamp - startingTimestamp << " " << (state ? '1' : '0') << "\r\n";
        auto bytes = text.str();
        ttlFile->WriteBinaryData(bytes.data(), bytes.size());
    }

    void RecordEnginePlugin::writeSpike(int electrodeIndex, const Spike* spike)
    {
        if (mode != SPIKES_AND_SET || recordingFailed) return;
        receivedRecordingData = true;
        std::array<int8, spikesBytesPerChan * spikesNumChans> bytes{};
        const float* voltages = spike->getDataPointer();
        for (int channel = 0; channel < spikesNumChans; ++channel)
            float32sToInt8s<oeSampsPerSpike, -125, 125>(voltages + channel * oeSampsPerSpike,
                bytes.data() + channel * spikesBytesPerChan + 4);
        const double timestamp = spike->getTimestampInSeconds();
        if (writeImmediately([&] { writeLocalSpike(electrodeIndex, timestamp, bytes); })) return;
        if (!reservePending(bytes.size() + sizeof(double))) return;
        pendingWrites.emplace_back([this, electrodeIndex, timestamp, bytes] { writeLocalSpike(electrodeIndex, timestamp, bytes); });
    }

    void RecordEnginePlugin::writeLocalSpike(int electrodeIndex, double timestamp,
                                               std::array<int8, 216> bytes)
    {
        if (timestamp < startingTimestamp) return;
        const double seconds = timestamp - startingTimestamp;
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
        if (mode == NONE || recordingFailed) return;
        if (stream == nullptr) {
            if (timingSession && text.startsWith("Software Time (")) {
                // The sampleNumber parameter carries Unix milliseconds for this
                // message. The GUI's timestamp parameter is zero for null streams.
                timingSession->publish(timingNodeId, sampleNumber);
                announceTimingWarnings();
            }
        } else if (!stream->getName().startsWith(ignore_stream_prefixed)) {
            if (startingTimestamp != TIMESTAMP_UNINITIALIZED || !std::isfinite(timestamp) || timestamp < 0) {
                failRecording("Invalid or repeated LTX stream recording origin");
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
