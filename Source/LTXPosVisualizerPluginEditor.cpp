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


#include "LTXPosVisualizerPluginEditor.h"
#include "LTXPosVisualizerPluginCanvas.h"
#include "LTXPosVisualizerPlugin.h"
#include "util.h"

namespace LTX {
PosVisualizerPluginEditor::PosVisualizerPluginEditor(GenericProcessor* p)
    : VisualizerEditor(p, "Pos", 190)
{
    desiredWidth = 240;;
    addTextBoxParameterEditor(Parameter::ParameterScope::PROCESSOR_SCOPE, "left", 10, 25);
    addTextBoxParameterEditor(Parameter::ParameterScope::PROCESSOR_SCOPE, "right", 10, 45);

    addTextBoxParameterEditor(Parameter::ParameterScope::PROCESSOR_SCOPE, "top", 10, 65);
    addTextBoxParameterEditor(Parameter::ParameterScope::PROCESSOR_SCOPE, "bottom", 10, 85);
    addTextBoxParameterEditor(Parameter::ParameterScope::PROCESSOR_SCOPE, "ppm", 10, 105);
    
    clearButton = std::make_unique<UtilityButton>("Clear Path");
    clearButton->setFont(FontOptions("Fira Code", "Regular", 10));
    clearButton->setBounds(150, 104, 80, 20);
    clearButton->addListener(this);
    clearButton->setTooltip("Can only clear display after recording has been stopped.");
    addAndMakeVisible(clearButton.get()); // makes the button a child component of the editor and makes it visible
    metadataLabel.setBounds(10, 128, 220, 35);
    metadataLabel.setFont(FontOptions(11.0f));
    addAndMakeVisible(metadataLabel);
    refreshMetadataControls();
}

void PosVisualizerPluginEditor::refreshMetadataControls() {
    const auto* processor = dynamic_cast<PosVisualizerPlugin*>(getProcessor());
    if (!processor) return;
    const auto metadata = processor->getCameraMetadata();
    const char* names[] = {"left", "right", "top", "bottom", "ppm"};
    const int indices[] = {0, 2, 1, 3};
    for (int i = 0; i < 5; ++i) {
        auto* control = dynamic_cast<TextBoxParameterEditor*>(getParameterEditor(names[i]));
        if (!control) continue;
        control->setEnabled(!metadata->present);
        if (metadata->present) {
            const String value = i == 4 ? (metadata->calibrated() ? String(metadata->ppm, 2) : String("uncalibrated"))
                : (metadata->validWindow() ? String(metadata->window[indices[i]]) : String("unavailable"));
            if (auto* label = control->getValueLabel()) {
                label->setText(value, dontSendNotification);
                label->setEditable(false, false, false);
            }
        } else if (metadataControlsLocked) control->updateView();
    }
    metadataControlsLocked = metadata->present;
    metadataLabel.setText(metadata->present ? "Camera metadata (read only)" : "Legacy viewer settings", dontSendNotification);
    metadataLabel.setTooltip(metadata->present ? String(metadata->cropText) : String("Used when the stream supplies no camera metadata."));
}

Visualizer* PosVisualizerPluginEditor::createNewCanvas()
{
    return new PosVisualizerPluginCanvas((PosVisualizerPlugin*) getProcessor());;
}

void PosVisualizerPluginEditor::buttonClicked(Button* button) {
    reinterpret_cast<PosVisualizerPlugin*>(getProcessor())->clearRecording();
}

}
