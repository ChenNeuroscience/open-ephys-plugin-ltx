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

#include "LTXPosVisualizerPlugin.h"
#include "LTXPosVisualizerPluginEditor.h"
#include "LTXSharedState.h"
#include "util.h"

namespace LTX {

PosVisualizerPlugin::PosVisualizerPlugin() : GenericProcessor("Pos Viewer")
{
   
}


PosVisualizerPlugin::~PosVisualizerPlugin()
{
    stopTimer();
}

void PosVisualizerPlugin::registerParameters()
{
    addIntParameter(Parameter::PROCESSOR_SCOPE, "width", "Width", "INACTIVE / DEPRECATED", 1000, 200, 5000, false);   // OE does not handle removing parameters very well, so we leave these here for now
    addIntParameter(Parameter::PROCESSOR_SCOPE, "height", "Height", "INACTIVE / DEPRECATED", 1000, 200, 5000, false);

    addIntParameter(Parameter::PROCESSOR_SCOPE, "left", "Left", "Window left in the 'pixel' units sent by Bonsai.", 0, 0, 5000, false);
    addIntParameter(Parameter::PROCESSOR_SCOPE, "right", "Right", "Window right in the 'pixel' units sent by Bonsai.", 1000, 200, 5000, false);
    addIntParameter(Parameter::PROCESSOR_SCOPE, "top", "Top", "Window top in the 'pixel' units sent by Bonsai.", 0, 0, 5000, false);    
    addIntParameter(Parameter::PROCESSOR_SCOPE, "bottom", "Bottom", "Window bottom in the 'pixel' units sent by Bonsai.", 1000, 200, 5000, false);

    addFloatParameter(Parameter::PROCESSOR_SCOPE, "ppm", "PPM", "Pixels Per Meter", "pixels", 400.0f, 100.0f, 1000.0f, 0.1, false);

    paramLeft = dynamic_cast<IntParameter*>(getParameter("left"));
    paramRight = dynamic_cast<IntParameter*>(getParameter("right"));
    paramTop = dynamic_cast<IntParameter*>(getParameter("top"));
    paramBottom = dynamic_cast<IntParameter*>(getParameter("bottom"));

}

AudioProcessorEditor* PosVisualizerPlugin::createEditor()
{
    editor = std::make_unique<PosVisualizerPluginEditor>(this);
    return editor.get();
}


void PosVisualizerPlugin::updateSettings()
{
    hasPositionStream = false;
    positionChannels.fill(-1);
    for (const DataStream* stream : getDataStreams()) {
        if (!stream) continue;
        if (stream->getName() != "bonsai") continue;
        int count = 0;
        for (int i = 0; i < continuousChannels.size(); ++i) {
            if (continuousChannels[i] && continuousChannels[i]->getStreamId() == stream->getStreamId()) {
                if (count < 7) positionChannels[count] = i;
                ++count;
            }
        }
        if (count == 7) {
            positionStreamId = stream->getStreamId();
            hasPositionStream = true;
            break;
        }
    }
    refreshCameraMetadata();
    startTimer(250); // The camera rewrites shared metadata on the message thread.
}

void PosVisualizerPlugin::refreshCameraMetadata() {
    CameraMetadata next;
    if (hasPositionStream) {
        for (const DataStream* stream : getDataStreams())
            if (stream && stream->getStreamId() == positionStreamId) next = readCameraMetadata(stream);
    }
    const auto previous = getCameraMetadata();
    if (previous->present != next.present || previous->window != next.window || previous->cropText != next.cropText)
        recordingBuffer.clear(); // Do not mix paths from different arenas.
    std::atomic_store(&cameraMetadata, std::make_shared<const CameraMetadata>(next));
    for (const char* name : {"left", "right", "top", "bottom", "ppm"}) {
        if (auto* parameter = getParameter(name))
            if (parameter->isEnabled() == next.present) parameter->setEnabled(!next.present);
    }
    if (previous->present && !next.present) parameterValueChanged(nullptr);
    if (auto* posEditor = dynamic_cast<PosVisualizerPluginEditor*>(editor.get()))
        posEditor->refreshMetadataControls();
}

void PosVisualizerPlugin::timerCallback() { refreshCameraMetadata(); }

void PosVisualizerPlugin::startRecording() {
    refreshCameraMetadata();
    isRecording = true;
    recordingBuffer.clear();
}

void PosVisualizerPlugin::stopRecording() {
    isRecording = false;
}


void PosVisualizerPlugin::clearRecording() {
    recordingBuffer.clear();
}

void PosVisualizerPlugin::process(AudioBuffer<float>& buffer)
{
    if (hasPositionStream)
    {
        const uint32 numSamples = getNumSamplesInBlock(positionStreamId);
        if (numSamples == 0) {
            return;
        }
        // Reject incomplete mappings and transiently smaller buffers during
        // signal-chain reconfiguration before taking any channel pointers.
        if (numSamples > static_cast<uint32>(buffer.getNumSamples())) return;
        for (int channel : positionChannels)
            if (channel < 0 || channel >= buffer.getNumChannels()) return;

        if (isRecording) {
            // Keep camera coordinates; the canvas applies the current crop.
            auto x1_buffer = buffer.getReadPointer(positionChannels[ChannelMapping::x1]);
            auto y1_buffer = buffer.getReadPointer(positionChannels[ChannelMapping::y1]);
            
            for (int i = 0; i < numSamples; i++) {
                PosPoint point {x1_buffer[i], y1_buffer[i]};
                recordingBuffer.write(point, std::isfinite(point.x) && std::isfinite(point.y));
            }

        }

        // Keep the newest sample in camera coordinates.
        latestPosSamp.timestamp = buffer.getReadPointer(positionChannels[ChannelMapping::Timestamp])[numSamples-1];
        latestPosSamp.x1 = buffer.getReadPointer(positionChannels[ChannelMapping::x1])[numSamples-1];
        latestPosSamp.y1 = buffer.getReadPointer(positionChannels[ChannelMapping::y1])[numSamples-1];
        latestPosSamp.x2 = buffer.getReadPointer(positionChannels[ChannelMapping::x2])[numSamples-1];
        latestPosSamp.y2 = buffer.getReadPointer(positionChannels[ChannelMapping::y2])[numSamples-1];
        latestPosSamp.numpix1 = buffer.getReadPointer(positionChannels[ChannelMapping::numpix1])[numSamples-1];
        latestPosSamp.numpix2 = buffer.getReadPointer(positionChannels[ChannelMapping::numpix2])[numSamples-1];
        return; // should only be one data stream
    }

}


void PosVisualizerPlugin::handleTTLEvent(TTLEventPtr event)
{

}


void PosVisualizerPlugin::handleSpike(SpikePtr spike)
{

}


void PosVisualizerPlugin::handleBroadcastMessage (const String& msg, const int64 messageTimeMilliseconds)
{

}

void PosVisualizerPlugin::saveCustomParametersToXml(XmlElement* parentElement)
{

}


void PosVisualizerPlugin::loadCustomParametersFromXml(XmlElement* parentElement)
{

}


void PosVisualizerPlugin::parameterValueChanged(Parameter* param) {
    if (getCameraMetadata()->present) return;
    auto* ppm = getParameter("ppm");
    if (!paramLeft || !paramRight || !paramTop || !paramBottom || !ppm) return;
    LTX::SharedState::window_min_x = paramLeft->getValue();
    LTX::SharedState::window_max_x = paramRight->getValue();
    LTX::SharedState::window_min_y = paramTop->getValue();
    LTX::SharedState::window_max_y = paramBottom->getValue();
    LTX::SharedState::pixels_per_metre = ppm->getValue();
};

}
