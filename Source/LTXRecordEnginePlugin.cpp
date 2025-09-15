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
    constexpr char* ignore_stream_named = "memory_usage";

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
        if (getNumRecordedSpikeChannels() > 0) {
            mode = RecordMode::SPIKES_AND_SET;
            LOGC("LTX RecordEngine using mode:SPIKES_AND_SET (", mode, ").");
        }
        else if (getNumRecordedContinuousChannels() == 0) {
            LOGE("No spikes and no continous channels, nothing to record.");
            mode = RecordMode::NONE;
            return;
        }
        else if (getContinuousChannel(getGlobalIndex(0))->getStreamName() == "bonsai") {
            mode = RecordMode::POS_ONLY; // first continuous channel is bonsai data, treat as pos
            LOGC("LTX RecordEngine using mode:POS_ONLY (", mode, ").");
            if (getNumRecordedDataStreams() > 1) { // WARNING: for some reason getTotalRecordedStreams() returns 0 when it should be 1, so this check it maybe totally broken
                LOGE("For LTX pos, require exactly 1 data stream, but found ", getNumRecordedDataStreams(), " streams.");
                CoreServices::setAcquisitionStatus(false);
                return;
            }
        }
        else {
            mode = RecordMode::EEG_ONLY;
            LOGC("LTX RecordEngine using mode:EEG_ONLY (", mode, ").");
            size_t n_streams = 0;
            for (auto stream : getDataStreams()) {
                if (stream->getName() == ignore_stream_named) {
                    continue;
                }
                n_streams++;
            }
            if (n_streams != 1) {
                LOGE("For LTX eeg, require exactly 1 data stream (excluding a '", ignore_stream_named, "', but found ", n_streams, " streams.");
                CoreServices::setAcquisitionStatus(false);
                return;
            }
        }

        std::string basePath = rootFolder.getParentDirectory().getFullPathName().toStdString()
            + (experimentNumber == 1 ? "" : " e" + std::to_string(experimentNumber))
            + (recordingNumber == 0 ? "" : " r" + std::to_string(recordingNumber + 1));

        std::chrono::system_clock::time_point start_tm = std::chrono::system_clock::now(); // used to calculate duration (not sure if OpenEphys offers an alternative)

        startingTimestamp = TIMESTAMP_UNINITIALIZED;

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
                    CoreServices::setAcquisitionStatus(false);
                    return;
                }
                if (channel->getTotalSamples() != oeSampsPerSpike) {
                    LOGE("Expected exactly 40 samples per spike, which will be padded out to 50, but found ", channel->getTotalSamples());
                    CoreServices::setAcquisitionStatus(false);
                    return;
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
            for (int i = 0; i < getNumRecordedContinuousChannels(); i++) {
                // important check before we get going...
                const ContinuousChannel* channel = getContinuousChannel(getGlobalIndex(i));
                if (channel->getStreamName() == ignore_stream_named) {
                    continue;
                }
                if (channel->getSampleRate() != eegInputSampRate) {
                    LOGE("Expected a sample rate of exactly ", eegInputSampRate, ", but found ", channel->getSampleRate());
                    CoreServices::setAcquisitionStatus(false);
                    return;
                }

                eegFiles.push_back(std::make_unique<LTXFile>(basePath, ".egf" + (i == 0 ? "" : std::to_string(i + 1)), start_tm));
                LTXFile* f = eegFiles.back().get();
                f->AddHeaderValue("num_chans", 1);
                f->AddHeaderValue("sample_rate", std::to_string(eegOutputSampRate) + " hz");
                f->AddHeaderValue("stream_name", channel->getStreamName().toStdString());
                f->AddHeaderValue("chan_name", channel->getName().toStdString());
                f->AddHeaderPlaceholder("num_EEG_samples");
                eegFullSampCount.push_back(0);
            }
        }
        else if (mode == RecordMode::POS_ONLY) {

            if (getNumRecordedContinuousChannels() != requiredPosChans) {
                LOGE("For LTX pos, require exactly ", requiredPosChans, " float channels from Bonsai. The first is a timestamp, and then x1,y1,x2,y2,numpix1,numpix2. ",
                    "However, received ", getNumRecordedContinuousChannels(), " channels.");
                CoreServices::setAcquisitionStatus(false);
                return;
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


    }

    void RecordEnginePlugin::closeFiles()
    {
        std::chrono::system_clock::time_point end_tm = std::chrono::system_clock::now();

        if (mode == SPIKES_AND_SET) {
            setFile->FinaliseFile(end_tm);

            for (int i = 0; i < tetFiles.size(); i++) {
                tetFiles[i]->FinaliseHeaderPlaceholder(tetSpikeCount[i]);
                tetFiles[i]->FinaliseFile(end_tm);
            }

            if(ttlFile != nullptr){
                ttlFile->FinaliseFile(end_tm);
            }
        }
        else if (mode == RecordMode::EEG_ONLY) {
            for (int i = 0; i < eegFiles.size(); i++) {
                eegFiles[i]->FinaliseHeaderPlaceholder(eegFullSampCount[i] / eegDownsampleBy);
                eegFiles[i]->FinaliseFile(end_tm);
            }
        }
        else if (mode == RecordMode::POS_ONLY) {
            posFile->FinaliseHeaderPlaceholder(posSampCount);
            posFile->FinaliseFile(end_tm);
        }

        LOGC("Completed writing files.")

    }

    void RecordEnginePlugin::writeContinuousData(int writeChannel,
        int realChannel,
        const float* dataBuffer,
        const double* ftsBuffer,
        int size)
    {

        if (mode == RecordMode::SPIKES_AND_SET) {
            return;
        }

        while (ftsBuffer[0] < startingTimestamp && size > 0) {
            size--;
            ftsBuffer++;
            dataBuffer++;
            if (size == 0) {
                return; // entire block of data is from before hitting record button
            }
        }

        if (mode == RecordMode::EEG_ONLY) {
            if (getContinuousChannel(realChannel)->getStreamName() == ignore_stream_named) {
                return;
            }
            // no timestamps written in the EEG file at all
            constexpr int outputBufferSize = 1024;
            if (size / eegDownsampleBy + 1 > outputBufferSize) {
                LOGE("size / downsampleBy +1 = ", size, " / ", eegDownsampleBy, " +1 is greater than expected (expected ", outputBufferSize, ")");
                CoreServices::setAcquisitionStatus(false);
                return;
            }

            uint64 remainder = eegFullSampCount[writeChannel] % eegDownsampleBy;
            uint64 offset = remainder == 0 ? 0 : eegDownsampleBy - remainder;
            int8_t eegBuffer[outputBufferSize]; // warning: not initialised
            int64 nSampsWritten = float32sToInt8sDownsampled<static_cast<size_t>(outputBufferSize), -250, 250, eegDownsampleBy >(
                &dataBuffer[offset], eegBuffer, size - static_cast<int>(offset));
            eegFiles[writeChannel]->WriteBinaryData(eegBuffer, nSampsWritten);
            eegFullSampCount[writeChannel] += size;

        } else if (mode == RecordMode::POS_ONLY) {

            if (posFirstTimestamp == TIMESTAMP_UNINITIALIZED && writeChannel == 0) {
                posFirstTimestamp = dataBuffer[0];
                if (posFirstTimestamp > 4 * 60 * 60 /* 4 hours in seconds = 14400 */) {
                    LOGE("POS recording started with 32bit floating point timestamp: ", posFirstTimestamp, " seconds. That's quite large; you won't get many decimal places of precision. "
                        "Stopping acquistion now. When you restart acquisition, the timestamp will begin at 0seconds, which will work much better.");
                    // note this is talking about the timestamp we recieve from the Bonsai source; the timestamps we record below will always start from zero, but they will inherit
                    // the precision provided by the Bonsai source, hence the check here. Note we are checking at the start of the recording, so we use an even more conservative threshold.
                    CoreServices::setAcquisitionStatus(false);
                    return;
                }
            }

            // Note we are assuming that channels came in order so that when we get the 0th one first
            // and when we get the last one we can assume we've seen the all and they all had the same size.
            // Hopefully a safe assumption, but i haven't actually checked the existing implementation in the core codebase.

            if (writeChannel == posTimestampChannel) {
                posSamplesBuffer.resize(size);
                std::memset(posSamplesBuffer.data(), 0, size * sizeof(PosSample));
                posSampCount += size;
                for (int i = 0; i < size; i++) {
                    posSamplesBuffer[i].timestamp = BSWAP32(
                        std::isnan(dataBuffer[i]) ? 0 : static_cast<int32_t>((dataBuffer[i] - posFirstTimestamp) * timestampTimebase));
                }
            } else {
                for (int i = 0; i < size; i++) {
                    posSamplesBuffer[i].xy_etc[writeChannel - 1] = BSWAP16(
                        std::isnan(dataBuffer[i]) ? posNaN : static_cast<uint16_t>(dataBuffer[i])
                    );
                }

                if (writeChannel == requiredPosChans - 1) {
                    posFile->WriteBinaryData(static_cast<void*>(posSamplesBuffer.data()), sizeof(PosSample) * size);
                }
            }
        }

    }

    void RecordEnginePlugin::writeEvent(int eventIndex, const EventPacket& event)
    {
        if(ttlFile == nullptr){
            return;
        }
        const EventChannel* info = getEventChannel(eventIndex);
        EventPtr eventStruct = Event::deserialize(event, info);

        if (startingTimestamp == TIMESTAMP_UNINITIALIZED) {
            return;
        } else if (eventStruct->getTimestampInSeconds() < startingTimestamp) {
            return;
        } else if (eventStruct->getEventType() != EventChannel::TTL) {
            return;
        }

        TTLEvent* ttl = static_cast<TTLEvent*>(eventStruct.get());

        std::ostringstream str_stream;
        str_stream << "ttl_" << (ttl->getLine()+1) << " "
                   << (eventStruct->getTimestampInSeconds() - startingTimestamp) << " "
                   << (ttl->getState() ? '1' : '0') << "\r\n";
        std::string str = str_stream.str();
        ttlFile->WriteBinaryData((void*)str.c_str(), str.size());
    }

    void RecordEnginePlugin::writeSpike(int electrodeIndex, const Spike * spike)
    {
        if (mode != RecordMode::SPIKES_AND_SET) {
            return;
        }
        if (startingTimestamp == TIMESTAMP_UNINITIALIZED) {
            return;
        } else if (spike->getTimestampInSeconds() < startingTimestamp) {
            return;
        }
        constexpr int totalBytes = spikesBytesPerChan * spikesNumChans;
        const SpikeChannel* channel = getSpikeChannel(electrodeIndex);

        int8 spikeBuffer[totalBytes] = {}; // initialise with zeros

        int32_t timestamp = BSWAP32(static_cast<int32_t>((spike->getTimestampInSeconds() - startingTimestamp) * timestampTimebase));

        const float* voltageData = spike->getDataPointer();
        for (int i = 0; i < spikesNumChans; i++)
        {
            std::memcpy(&spikeBuffer[i * spikesBytesPerChan], &timestamp, 4);
            float32sToInt8s<oeSampsPerSpike, -125, 125>(
                &voltageData[i * oeSampsPerSpike],
                &spikeBuffer[i * spikesBytesPerChan + 4 /* timestamp bytes */]);
        }
        tetFiles[spike->getChannelIndex()]->WriteBinaryData(spikeBuffer, totalBytes);
        tetSpikeCount[spike->getChannelIndex()]++;
    }

    void RecordEnginePlugin::writeTimestampSyncText(
        DataStream* stream,
        int64 sampleNumber,
        double timestamp,
        String text)
    {
        if (stream == nullptr || stream->getName() == ignore_stream_named) {
            return;
        }
        if (startingTimestamp != TIMESTAMP_UNINITIALIZED) {
            LOGE("MODE:", mode, ": writeTimestampSyncText() called more than once with a seemingly valid stream. Aborting recording.");
            CoreServices::setAcquisitionStatus(false);
        }
        startingTimestamp = timestamp;

    }

    void RecordEnginePlugin::setParameter(EngineParameter& parameter)
    {
    }

}
