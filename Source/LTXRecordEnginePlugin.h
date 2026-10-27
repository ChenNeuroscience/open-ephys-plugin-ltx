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

#ifndef RECORDENGINEPLUGIN_H_DEFINED
#define RECORDENGINEPLUGIN_H_DEFINED

#include <RecordingLib.h>
#include "LTXFile.h"
#include "LTXRecordingTiming.h"


#include <stdio.h>
#include <map>
#include <chrono>
#include <vector>
#include <array>
#include <deque>
#include <functional>


namespace LTX {

    class RecordEnginePlugin : public RecordEngine
    {
    public:

        /** Constructor */
        RecordEnginePlugin();

        /** Destructor */
        ~RecordEnginePlugin();

        /** Launches the manager for this Record Engine, and instantiates any parameters */
        static RecordEngineManager* getEngineManager();

        /** Returns a string that can be used to identify this record engine*/
        String getEngineId() const override { return "LTX"; }

        /** Called when recording starts to open all needed files */
        void openFiles(File rootFolder, int experimentNumber, int recordingNumber) override;

        /** Called when recording stops to close all files and do all the necessary cleanup */
        void closeFiles() override;

        /** Write continuous data for a channel, including synchronized float timestamps for each sample */
        void writeContinuousData(int writeChannel,
                                   int realChannel,
                                   const float* dataBuffer,
                                   const double* ftsBuffer,
                                   int size) override;

        /** Write a single event to disk (TTL or TEXT) */
        void writeEvent(int eventChannel,
                        const EventPacket& event) override;

        /** Write a spike to disk */
        void writeSpike(int electrodeIndex, const Spike* spike) override;

        /** Write the timestamp sync text messages to disk*/
        void writeTimestampSyncText(DataStream* stream, int64 sampleNumber, double timestamp, String text) override;


        
    private:
        enum RecordMode
        {
            NONE=-1,
            SPIKES_AND_SET,
            EEG_ONLY,
            POS_ONLY
        };
        const int TIMESTAMP_UNINITIALIZED = -1;

        RecordMode mode = RecordMode::NONE;

        double startingTimestamp = TIMESTAMP_UNINITIALIZED;

        std::unique_ptr<LTXFile> setFile;

        std::vector<std::unique_ptr<LTXFile>> tetFiles;
        std::vector<uint64> tetSpikeCount;

        std::unique_ptr<LTXFile> ttlFile;

        std::vector<std::unique_ptr<LTXFile>> eegFiles;
        std::vector<uint64> eegFullSampCount;
        std::vector<uint64> eegWrittenSamples;
        std::vector<uint64> eegPaddingSamples;
        std::vector<double> eegFirstDataDelay;
        std::vector<bool> eegStarted;
        std::map<int, size_t> eegWriteChannels;

        std::unique_ptr<LTXFile> posFile;
        uint64 posSampCount = 0;
        size_t posSampRate = 0;

        // this relates to the hacky timestamp encoded in one of the voltage streams not the proper timestamp data
        double posFirstTimestamp = TIMESTAMP_UNINITIALIZED; 
        uint64 posRealSampCount = 0;
        uint64 posPaddingSamples = 0;
        double posFirstDataDelay = 0;
        bool posStarted = false;

        // We assume that the channels come in order so that we know when we've got all the data for a given batch of samples.
        struct PosSample {
            int32_t timestamp = 0;
            uint16_t xy_etc[8] = {}; // we only use the first 6 elements, with the last two just padding
        };
        static_assert(sizeof(PosSample) == 4+8*2, "PosSample should be laid out in memory as 4+8*2 bytes.");
        std::vector<PosSample> posSamplesBuffer;

        std::string basePath;
        int timingNodeId = -1;
        std::shared_ptr<Timing::Session> timingSession;
        Timing::Snapshot timingSnapshot;
        double recordingDelaySeconds = 0;
        bool filesOpen = false;
        bool timingFailed = false;
        std::string timingError;
        std::deque<std::function<void()>> pendingWrites;
        size_t pendingBytes = 0;
        static constexpr size_t maximumPendingBytes = 256 * 1024 * 1024;

        void registerTiming(int experimentNumber, int recordingNumber);
        bool flushPendingWrites();
        bool reservePending(size_t bytes);
        void failTiming(const std::string& message);
        void openAlignedFiles();
        void writeTimingMetadata(bool complete);
        void addTimingHeaders(LTXFile& file);
        void addPaddingHeaders(LTXFile& file, uint64 count, double firstDataDelay, double rate);
        void writeAlignedContinuous(int writeChannel, int realChannel,
                                    const float* data, const double* timestamps, int size);
        void writeAlignedSpike(int electrodeIndex, double timestamp,
                               std::array<int8, 216> bytes);
        void writeAlignedEvent(double timestamp, int line, bool state);
        void beginPosition(double firstDataDelay);
        
        /** Sets an engine parameter */
	    void setParameter (EngineParameter& parameter) override;
    };

}
#endif
