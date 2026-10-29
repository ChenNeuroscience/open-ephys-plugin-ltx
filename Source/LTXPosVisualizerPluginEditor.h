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

//This prevents include loops. We recommend changing the macro to a name suitable for your plugin
#ifndef VISUALIZERPLUGINEDITOR_H_DEFINED
#define VISUALIZERPLUGINEDITOR_H_DEFINED

#include <VisualizerEditorHeaders.h>

namespace LTX {
/** 
	The editor for the PosVisualizerPlugin

	Includes buttons for opening the canvas in a tab or window
*/

class PosVisualizerPluginEditor : public VisualizerEditor, Button::Listener
{
public:

	/** Constructor */
	PosVisualizerPluginEditor(GenericProcessor* parentNode);

	/** Destructor */
	~PosVisualizerPluginEditor() { }

	/** Creates the canvas */
	Visualizer* createNewCanvas() override;

	void buttonClicked(Button* button) override;
    void refreshMetadataControls();

private:

	std::unique_ptr<UtilityButton> clearButton;
    Label metadataLabel;
    bool metadataControlsLocked = false;

	/** Generates an assertion if this class leaks */
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PosVisualizerPluginEditor);
};

}
#endif // VISUALIZERPLUGINEDITOR_H_DEFINED
