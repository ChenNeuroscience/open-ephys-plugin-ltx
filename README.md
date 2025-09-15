# Open Ephys (v1*) Plugin - LTX Record Engine

*Currently this requires a fork of v1.0.1 OE. The fork is available for windows [here](https://github.com/d1manson/open-ephys-main/releases/tag/d728c1b2a).
 
The LTX format is broadly compatible with **L**egacy analysis code used with **T**etrode recordings in various rodent labs. However it is _not_ e**X**plicitly defined.
Amongst other pre-existing tools, the LTX files should be viewable within the [Waveform GUI](https://github.com/d1manson/waveform).

Roughly speaking, all files contain multiple header lines of the format: `key<space>value`, followed by the special token `data_start`, then binary data, and then `data_end`.

It produces:

- experiment_name.set - a file that only contains header info.
- experiment_name.1, experiment_name.2, ... - tetrode spike data. For each spike, the binary data gives `4 x [4 byte timestamp | 50 one-byte voltage values]`.
- experiment_name.efg, experiment_name.efg2, ... - continuous data downsampled to 1kHz and stored as single bytes without any timestamp.
- experiment_name.pos - position data. Expects 7 continuous data channels from bonsai containing timestamp, and then x1,y1,x2,y2,numpix1,numpix2 respectively.
  Currently assumes the x and y values are within the range [0,1000]. To get the data from bonsai into Openephys, use the separate [bonsai plugin](https://github.com/d1manson/open-ephys-plugin-bonsai)
   (note there are a few open ephys plugins that aim to stich together Bonsai and Openephys, so make sure you are using the right one). The timestamp used here is
  not using the proper timestamp machinery of openephys, it's literally a timestamp within a 32bit float channel. There is a slight problem with this
  in that the precision of 32 bit floats starts to degrade above 20,000 (5hs in seconds); at that point there are about 5 representable values in the third decimal place,
- for each value in the second decimal place. But hopefully that's enough here. Note that the bonsai plugin does start from zero for the first timestamp it sees.

Note that you'll need three separate nodes in openephys, one to create set+tet file, one for eeg, and one for pos.

## The plugins here

1. **LTX Record Engine**. This can write all the file types mentioned above. For a given record node in the signal chain, it will autodetect which mode it's in using the following logic:
- if the record node has spikes enabled then it will write `.set` and `.1`, `.2`, ... files only. The voltage data in the spike channels should be in the range +- 250, and will be divided
  by two to fit into a signed 8-bit integer. Use the gain plugin to configure this per channel, if required.
- alternatively if the continuous stream it has access to is named `'bonsai'`, it will write `.pos` data. See the separate [bonsai plugin](https://github.com/d1manson/open-ephys-plugin-bonsai)
  which allows you to bring position data from Bonsai into open ephys. That plugin is fairly generic, but here we require that you setup the bonsai plugin to recieve a timestamp and 6 float values.
  Specifically the format is: `t,x1,y1,x2,y2,numpix1,numpix2`, where `t` is a double and the rest are singles. The 6 values will be converted to 16-bit ints without any multiplication, so to get
  decent resolution it's worth having Bonsai provide numbers over a fairly large integer range (e.g. 0-1000 rather than 0-100).
- otherwise it will write `.egf` data. As with the spike data, the voltage data here should be in the range +- 250, and will be divided by two to fit into a signed 8-bit integer.

2. **Gain**. This is a simple processor "filter" plugin. It allows you to multiply the voltage on each channel by a value between -2 and 2. Probably you want to stay positive, but occasionally
   it's useful to be able to flip the voltage. As noted above, the point of this plugin is to allow you to scale the output voltage range to fit into an 8-bit signed integer.

3. **Pos Viewer** This expects the same kind of data as the pos mode record engine (described above). It displays the current (x1,y1) and (x2,y2) values with circles sized acording to `numpix1` and `numpix2`
    respectively. When recording is on, it also shows the full path of `(x1,y1)` over the course of the recording. At the end of the recording the path stays until you either start a new recording or click
   the 'clear path' button. This plugin lets you configure the window dimensions, and these values are actually read by the record engine and stored in the `.pos` file header (this is
   a bit counter intuitive becuase it looks like this plugin is a passive view-only node, but it is actually providing this little bit of metadta to the record engine; this is implemented in a very hacky way
   under the hood using global variables with `extern` as I couln't work out how to do it using the official plugin API).


   <img width="765" alt="Screenshot 2024-10-27 at 17 38 08" src="https://github.com/user-attachments/assets/f79dc117-d9a4-42d6-9ac8-338a34207a7c">


We do provide here a sample Open Ephys config file here, `./oe_sample_config` (though it may be a bit out of data compared to later revisions outside of source control).

## Installing

This plugin is not part of the offical set of Open Ephys plugins, but you can easily install it yourself by downloading the `.dll` file from the [latest release](https://github.com/d1manson/open-ephys-plugin-ltx/releases)
and then moving it into your `C:\Program Files\Open Ephys\plugins` folder (Mac/Linux binaries not provided here currently).

## Development

See the [Open Ephys Plugin API docs](https://open-ephys.github.io/gui-docs/Developer-Guide/Open-Ephys-Plugin-API/index.html) for info on how to develop a plugin
including links to the template repos. It's also worth looking at the source code for other plugins and the plugin-gui repo itself. A lot of what's been done here was
inspired by existing code.

As shown in `Source/OpenEphysLib` we have a few different kinds of plugin in this repo, so make sure to read all the relevant docs.


I found on Mac that you can build using this:

```bash
brew install cmake

# run this is the main GUI's Build dir (need to do this first)
CMAKE_C_COMPILER=clang CMAKE_CXX_COMPILER=clang cmake -G "Xcode" ..

# run this in the root of this plugin (set the GUI_BASE_DIR appropriately)
GUI_BASE_DIR=../main-gui CMAKE_C_COMPILER=clang CMAKE_CXX_COMPILER=clang cmake -G "Xcode" .

# and then to actually build
cmake --build . --config Release -- -arch x86_64 &&
cp -R Release/open-ephys-plugin-ltx.bundle /Users/<YOUR_MAC_USER_NAME_HERE>/Library/Application\ Support/open-ephys/plugins-api8/ 

# or (if copying into the dev build of the gui):
cp -R  Release/open-ephys-plugin-ltx.bundle "../main-gui/Build/Release/Open Ephys GUI.app/Contents/Plugins/" 
```
