/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
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

#include "LTXPosVisualizerPluginCanvas.h"
#include "LTXPosVisualizerPlugin.h"
#include "util.h"
#include <chrono>
#include <atomic>

namespace LTX{

	const int margin = 80;

	PosPlot::PosPlot(PosVisualizerPlugin* processor_)
		: processor(processor_) {
		paramLeft = reinterpret_cast<IntParameter*>(processor->getParameter("left"));
		paramRight = reinterpret_cast<IntParameter*>(processor->getParameter("right"));
		paramTop = reinterpret_cast<IntParameter*>(processor->getParameter("top"));
		paramBottom = reinterpret_cast<IntParameter*>(processor->getParameter("bottom"));
		paramPPM = reinterpret_cast<FloatParameter*>(processor->getParameter("ppm"));
	}

	PosPlot::~PosPlot(){}

    void PosPlot::paint(Graphics& g)
    {
        const auto metadata = processor->getCameraMetadata();
        if (metadata->present && !metadata->validWindow()) {
            g.setColour(Colours::white);
            g.drawText("Waiting for camera crop bounds", getLocalBounds(), Justification::centred);
            return;
        }
        const float left = metadata->present ? float(metadata->window[0]) : float(paramLeft->getValue());
        const float top = metadata->present ? float(metadata->window[1]) : float(paramTop->getValue());
        const float right = metadata->present ? float(metadata->window[2]) : float(paramRight->getValue());
        const float bottom = metadata->present ? float(metadata->window[3]) : float(paramBottom->getValue());
        const float W = right - left, H = bottom - top;
        const double ppm = metadata->present ? metadata->ppm : double(float(paramPPM->getValue()));
        if (W <= 0 || H <= 0 || getWidth() <= 2 * margin || getHeight() <= 2 * margin) return;
        const float pixelFactor = std::min((getWidth() - margin * 2) / W, (getHeight() - margin * 2) / H);
        const auto toX = [=](double x) { return float(margin + (x - left) * pixelFactor); };
        const auto toY = [=](double y) { return float(margin + (y - top) * pixelFactor); };
        const Rectangle<float> bounds(float(margin), float(margin), W * pixelFactor, H * pixelFactor);
        Path arena;
        if (metadata->present && metadata->shape == CameraMetadata::Shape::Circle) {
            arena.addEllipse(toX(metadata->cx - metadata->width / 2), toY(metadata->cy - metadata->height / 2),
                             float(metadata->width * pixelFactor), float(metadata->height * pixelFactor));
        } else if (metadata->present && metadata->shape == CameraMetadata::Shape::Rectangle) {
            const double radians = metadata->angleDegrees * MathConstants<double>::pi / 180;
            for (int i = 0; i < 4; ++i) {
                const double dx = (i == 0 || i == 3 ? -1 : 1) * metadata->width / 2;
                const double dy = (i < 2 ? -1 : 1) * metadata->height / 2;
                const float x = toX(metadata->cx + dx * std::cos(radians) - dy * std::sin(radians));
                const float y = toY(metadata->cy + dx * std::sin(radians) + dy * std::cos(radians));
                if (i == 0) arena.startNewSubPath(x, y); else arena.lineTo(x, y);
            }
            arena.closeSubPath();
        } else arena.addRectangle(bounds);

        g.setColour(Colours::white);
        g.setFont(FontOptions("Arial", 14, Font::FontStyleFlags::bold));
        const auto distanceLabel = [ppm](float pixels) -> String {
            return std::isfinite(ppm) && ppm > 0 ? formatFloat(float(pixels * 100 / ppm), 0) + "cm"
                                               : formatFloat(pixels, 0) + "px (uncalibrated)";
        };
        g.drawSingleLineText("< " + distanceLabel(W) + " >", int(toX((left + right) / 2)), margin - 6, Justification::centred);
        g.saveState();
        g.addTransform(AffineTransform::rotation(-MathConstants<float>::halfPi, margin - 6.0f, toY((top + bottom) / 2)));
        g.drawSingleLineText("< " + distanceLabel(H) + " >", margin - 6, int(toY((top + bottom) / 2)), Justification::centred);
        g.restoreState();

        // Clip both the arena and its bounding box: rotated crops can extend
        // beyond the camera frame, whose clipped window is the visible extent.
        g.saveState();
        g.reduceClipRegion(bounds.toNearestInt());
        g.reduceClipRegion(arena);
        g.setColour(Colours::white);
        g.fillPath(arena);
        const bool isRecording = processor->isRecording.load();
        const auto displayX = [&](float x) { return metadata->present ? x : clamp(x, left, right); };
        const auto displayY = [&](float y) { return metadata->present ? y : clamp(y, top, bottom); };
        if (processor->recordingBuffer.start_read() >= 2) {
            g.setColour(isRecording ? Colours::black : Colours::grey);
            PosPoint previous, next;
            processor->recordingBuffer.read(previous);
            while (processor->recordingBuffer.read(next)) {
                g.drawLine(toX(displayX(previous.x)), toY(displayY(previous.y)),
                           toX(displayX(next.x)), toY(displayY(next.y)), 1.0f);
                previous = next;
            }
        }
        const float x1 = processor->latestPosSamp.x1.load(), y1 = processor->latestPosSamp.y1.load();
        const float x2 = processor->latestPosSamp.x2.load(), y2 = processor->latestPosSamp.y2.load();
        const float area1 = processor->latestPosSamp.numpix1.load(), area2 = processor->latestPosSamp.numpix2.load();
        const auto drawBlob = [&](float x, float y, float area, Colour colour) {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(area) || area <= 0) return;
            const float diameter = std::max(2.0f, 2 * std::sqrt(area / MathConstants<float>::pi) * pixelFactor);
            g.setColour(colour);
            g.fillEllipse(toX(displayX(x)) - diameter / 2, toY(displayY(y)) - diameter / 2, diameter, diameter);
        };
        drawBlob(x1, y1, area1, Colours::green);
        drawBlob(x2, y2, area2, Colours::red);
        g.restoreState();

        const auto drawCoordinates = [&](float x, float y, float area, Colour colour, int offset) {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(area) || area <= 0) return;
            g.setColour(colour);
            g.drawSingleLineText("(" + formatFloat(x, 1) + ", " + formatFloat(y, 1) + ")", margin, int(toY(bottom)) + offset);
        };
        drawCoordinates(x1, y1, area1, Colours::green, 16);
        drawCoordinates(x2, y2, area2, Colours::red, 32);
        const float timestamp = processor->latestPosSamp.timestamp.load();
        g.setColour(isRecording ? Colours::red : Colours::white);
        if (isRecording) g.fillEllipse(toX(right) - 10, toY(bottom) + 7, 10, 10);
        g.drawSingleLineText(std::isfinite(timestamp) ? formatAsMinSecs(timestamp, 1) : String("no frame"),
                            int(toX(right)) - (isRecording ? 16 : 0), int(toY(bottom)) + 16, Justification::right);
    }


	PosVisualizerPluginCanvas::PosVisualizerPluginCanvas(PosVisualizerPlugin* processor_)
		: processor(processor_), plt(processor_) {
		
		addAndMakeVisible(plt);
		plt.setBounds(0, 0, getWidth(), getHeight());

	}

	PosVisualizerPluginCanvas::~PosVisualizerPluginCanvas()
	{

	}


	void PosVisualizerPluginCanvas::resized()
	{
		plt.setBounds(0, 0, getWidth(), getHeight());
	}

	void PosVisualizerPluginCanvas::refreshState()
	{

	}



	void PosVisualizerPluginCanvas::refresh()
	{
	}

	void PosVisualizerPluginCanvas::paint(Graphics& g){
		g.fillAll(Colours::black);
		plt.repaint();
	}

}