# Open Ephys (v1*) Plugin - LTX Record Engine

*Currently this requires the [Windows fork of v1.0.1 OE, release d728c1b2a](https://github.com/ChenNeuroscience/open-ephys-main/releases/tag/d728c1b2a). The startup diagnostics below use that build's existing timing messages; it does not require a GUI modification.
 
The LTX format is broadly compatible with **L**egacy analysis code used with **T**etrode recordings in various rodent labs. However it is _not_ e**X**plicitly defined.
Amongst other pre-existing tools, the LTX files should be viewable within the [Waveform GUI](https://github.com/d1manson/waveform).

Roughly speaking, all files contain multiple header lines of the format: `key<space>value`, followed by the special token `data_start`, then binary data, and then `data_end`.

It produces:

- experiment_name.set - a file that only contains header info.
- experiment_name.1, experiment_name.2, ... - tetrode spike data. For each spike, the binary data gives `4 x [4 byte timestamp | 50 one-byte voltage values]`.
- experiment_name.egf, experiment_name.egf2, ... - continuous data downsampled to 5kHz and stored as single bytes without any timestamp.
- experiment_name.pos - position data. Expects 7 continuous data channels from bonsai containing timestamp, and then x1,y1,x2,y2,numpix1,numpix2 respectively.
  Both the legacy OSC/Bonsai plugin and the new [Camera plugin](https://github.com/ChenNeuroscience/open-ephys-plugin-bonsai/pull/3) use this layout. The timestamp is carried in a float32 data channel, so its fractional precision degrades in long acquisitions; restart acquisition before four hours when recording positions with this engine.

Note that you'll need three separate nodes in openephys, one to create set+tet file, one for eeg, and one for pos.

## Recording startup diagnostics (1.1.0)

LTX measures the difference between participating Record Nodes' first recording blocks, writes that discrepancy to every output file's header, and shows a MessageCenter warning for each node starting **more than 50 ms** after the earliest one. Position, EEG, and spike nodes can start in any order. The measurement uses the GUI's existing `Software Time` message: its `sampleNumber` contains Unix milliseconds captured on the processing thread. Disk-thread scheduling and file-opening times are not used as recording origins.

This is **diagnostic only**. LTX does not pad data, shift timestamps, or change sample counts to correct the discrepancy. Each file keeps its own existing recording-relative timeline. For example, a position node starting 400 ms late records `record_node_start_offset_ms 400`, still starts its position timeline at zero, and warns that no correction was applied.

| Header | Meaning |
| --- | --- |
| `timing_method` | `oe_first_recording_block_wall_clock` |
| `recording_epoch_unix_ms` | Earliest captured recording-block time among participating LTX nodes; a diagnostic reference |
| `record_node_start_unix_ms` | This node's captured first recording-block time |
| `record_node_id` | Open Ephys Record Node ID |
| `record_node_start_offset_ms` | This node's delay relative to the earliest participant |
| `startup_timing_estimated` | `1`: host-clock estimate |
| `startup_timing_correction_applied` | `0` |
| `startup_timing_status` | `measured_uncorrected`, or `unavailable` if the measurement cannot be resolved |

Diagnostic headers use fixed-width fields, finalized once at close. Until then they remain provisional (`unavailable`). Timing-message delivery triggers warnings; ordinary continuous/spike/TTL writes do not inspect coordinator state or rewrite diagnostic headers. Unresolved values say `unavailable`, never zero. Missing or duplicate software-time messages leave recording data intact and produce a warning at close. Repeated recordings get separate coordinator state. Empty nodes are excluded when identified. Recording never waits for other nodes' timing messages.

Each node also writes `experiment_name.timing-NODE_ID.json` with participant origins/modes, the estimated discrepancy, `timing_available`, `correction_applied: false`, and data completion/error state. Actual data integrity failures, such as a missing native stream origin, still request a recording stop. Initial data waiting for its own native stream origin is bounded to 256 MiB; software-clock diagnostics do not gate data writing. Once the native origin arrives, continuous/spike/TTL writes bypass the startup staging queue, avoiding repeated vector copies and deferred callbacks.

The measurement cannot establish camera exposure latency, USB/OSC backlog, hardware clock drift, or wall-clock adjustments. Paired LED/TTL events on the Windows acquisition machine remain the alignment check. This covers LTX engines in the same loaded plugin, not other recording engines. Nodes writing the same file type still require distinct output basenames.

## Camera stream metadata and compatible position encoding

When `camera_tracker.*` metadata is present, the Pos Viewer renders the stream's crop bounds and shape: none (full frame), circle, or rotated square/rectangle, clipped to the frame. Left/right/top/bottom and PPM display the supplied values with editing disabled. Metadata is refreshed on the message thread every 250 ms, matching the source's update cadence. No calibration is shown as **uncalibrated**, rather than borrowing a viewer PPM. Unknown crop formats use the bounding rectangle. Until valid bounds arrive the viewer displays a waiting message. Saved legacy parameter values are retained and become editable again when a legacy stream is connected.

The recorder snapshots metadata directly from the matching data stream in `openFiles()`; it needs no Pos Viewer for new streams. It writes these original fields to the `.pos` header when supplied:

- `camera_tracker.pixels_per_metre`
- `camera_tracker.window` (`min_x min_y max_x max_y` in camera pixels)
- `camera_tracker.crop` (the source's shape/rotation description)
- `camera_tracker.frame_size` (`width height` in camera pixels)

It also writes `data_source open-ephys-plugin-camera v0.1`. **`v0.1` is a placeholder identifier**, not a version queried from the source. `position_metadata_source camera_tracker` distinguishes stream metadata from the legacy shared viewer configuration.

Real camera coordinates can equal `1023`, which legacy `.pos` readers interpret as missing. For new streams with valid window bounds, positions are translated by the crop bounding-box origin and uniformly scaled, only if necessary, to fit within 0–1022:

```
scale = min(1, 1022 / max(window_width, window_height))
stored_x = round((camera_x - window_min_x) * scale)
stored_y = round((camera_y - window_min_y) * scale)
stored_pixels_per_metre = camera_pixels_per_metre * scale
```

The usual `window_*` and `min/max_*` headers use the same encoded coordinate system, so existing readers preserve physical distances. The inverse transform is documented by `position_coordinate_origin_x`, `position_coordinate_origin_y`, and `position_coordinate_scale`; `position_coordinate_transform` is `crop_relative_uniform_scale`. Original crop metadata stays in camera pixels. Quantization is at most half an encoded pixel per axis.

Nonfinite/invalid coordinates become `1023`. Areas remain actual camera pixel counts, independent of coordinate scaling, saturating at 65535 instead of wrapping; nonfinite/nonpositive areas become zero. Positive fractional areas are at least one. `position_area_units camera_pixels_saturated_uint16` documents this distinction. Missing timestamps receive the local sample-grid time; real camera timestamp differences are retained. Fractional stream rates such as 29.97 Hz are preserved.

Without metadata, the recorder retains legacy coordinate mapping and reads bounds/PPM from the shared viewer configuration (`position_metadata_source legacy_viewer`); the viewer controls remain editable. Incomplete metadata with invalid bounds preserves the supplied fields but falls back to legacy bounds/mapping with a warning. Such fallback cannot disambiguate a real raw coordinate of 1023. An explicitly uncalibrated stream stores NaN PPM, rather than silently inventing calibration.

## Position recording across buffer wraps (1.1.1)

Open Ephys can deliver two pieces of one channel consecutively when its recording queue wraps: `timestamp-tail, timestamp-head, x1-tail, x1-head, ...`. Channels may also arrive in different-sized pieces. The recorder now assembles complete rows across these callbacks and checks that their native timestamps agree. It no longer assumes that one callback from each of the seven channels makes a complete block.

The earlier implementation replaced its timestamp rows on each timestamp callback. At a wrap, it could discard the first piece, then reject the next coordinate piece as inconsistent. Before that check was added, the same assumption could overwrite rows or write beyond a vector's logical size. Simply removing the check would preserve the data-corruption risk.

The queue in GUI release `d728c1b2a` holds `audio_buffer_size * 300` samples per recorded channel. At 50 Hz, a 64-sample audio buffer wraps every 384 seconds (6 min 24 s); a 1024-sample buffer wraps every 6144 seconds. A split read depends on batching at the boundary, so a wrap does not invariably trigger the old failure. This provides a reproducible mechanism for a delayed failure; the affected rig's audio buffer size is still needed to match its reported seven-minute interval.

Complete rows retain the existing position timestamp/coordinate encoding. Genuine native timestamp disagreements, excessive unmatched rows (more than 65,536), and incomplete rows at close still report an error instead of shifting channels silently. Startup timing remains diagnostic only.

`LTXFile` uses one `ReserveHeader` / `UpdateHeader` mechanism for duration, sample counts, and timing diagnostics. Fixed-width replacements preserve the binary file position and reject overlong values; binary writes and close check errors. File ownership is automatic, trial-date conversion is thread-safe, and file positions use 64-bit APIs on Windows.

## The plugins here

1. **LTX Record Engine**. This can write all the file types mentioned above. For a given record node in the signal chain, it will autodetect which mode it's in using the following logic:
- if the record node has spikes enabled then it will write `.set` and `.1`, `.2`, ... files only. The voltage data in the spike channels should be in the range +- 250, and will be divided
  by two to fit into a signed 8-bit integer. Use the gain plugin to configure this per channel, if required.
- alternatively if the continuous stream it has access to is named `'bonsai'`, it will write `.pos` data. See the separate [bonsai plugin](https://github.com/d1manson/open-ephys-plugin-bonsai)
  which allows you to bring position data from Bonsai into open ephys. That plugin is fairly generic, but here we require that you setup the bonsai plugin to recieve a timestamp and 6 float values.
  Specifically the format is: `t,x1,y1,x2,y2,numpix1,numpix2`, where `t` is a double and the rest are singles. Legacy coordinates are converted directly to 16-bit integers. New camera streams use the compatible encoding described above; area channels keep real pixel counts.
- otherwise it will write `.egf` data. As with the spike data, the voltage data here should be in the range +- 250, and will be divided by two to fit into a signed 8-bit integer.

2. **Gain**. This is a simple processor "filter" plugin. It allows you to multiply the voltage on each channel by a value between -2 and 2. Probably you want to stay positive, but occasionally
   it's useful to be able to flip the voltage. As noted above, the point of this plugin is to allow you to scale the output voltage range to fit into an 8-bit signed integer.

3. **Pos Viewer** This expects the same kind of data as the pos mode record engine (described above). It displays the current (x1,y1) and (x2,y2) values with circles sized acording to `numpix1` and `numpix2`
    respectively. When recording is on, it also shows the full path of `(x1,y1)` over the course of the recording. At the end of the recording the path stays until you either start a new recording or click
   the 'clear path' button. New camera metadata supplies the arena and calibration directly. For legacy streams this plugin lets you configure the window dimensions and PPM, and the record engine uses the shared configuration as its fallback `.pos` headers.


   <img width="765" alt="Screenshot 2024-10-27 at 17 38 08" src="https://github.com/user-attachments/assets/f79dc117-d9a4-42d6-9ac8-338a34207a7c">


We do provide here a sample Open Ephys config file here, `./oe_sample_config` (though it may be a bit out of data compared to later revisions outside of source control).

## Installing

This plugin is not part of the offical set of Open Ephys plugins, but you can easily install it yourself by downloading the `.dll` file from the [latest release](https://github.com/d1manson/open-ephys-plugin-ltx/releases)
and then moving it into your `C:\Program Files\Open Ephys\plugins` folder (Mac/Linux binaries not provided here currently).

## Development

Run the production-writer and camera metadata tests without building the GUI:

```sh
# macOS/Linux with clang++ and Python 3
python3 Tests/run_tests.py

# Windows with Visual Studio, CMake, and Python 3
python Tests/run_tests.py --cmake
```

These tests compile `LTXRecordEnginePlugin.cpp` and `LTXFile.cpp` with small API substitutes, then parse the resulting binary files. They cover all six start orders, unchanged binary payloads across discrepancies, reversed callback delivery, repeated recordings, downsampling phase, warning thresholds, missing timing messages, metadata/source headers, crop geometry, coordinate transforms, safe area encoding, split/ragged channel delivery, and a simulated 20-minute recording across three buffer wraps. Reserved-header checks verify updates around binary writes and byte-for-byte payload preservation. To also compile-check against an already configured copy of the exact GUI headers:

```sh
python3 Tests/run_tests.py --gui-source /path/to/open-ephys-main
```

Windows CI runs the same file-level tests and builds the plugin against release `d728c1b2a`. PR/manual builds do not create releases. Windows GUI/hardware LED–TTL validation remains a separate integration check.

The native regressions also link the GUI's actual `Metadata.cpp`, `DataQueue.cpp`, and JUCE modules:

```sh
python3 Tests/run_tests.py --cmake --native-api /path/to/open-ephys-main
```

These exercise the actual recording queue across three wraps and check a metadata read before acquisition, including the GUI's extra byte for `CHAR` fields, full-length crop text, and unknown crop formats. The viewer and recorder use the bounded `String` getter; the pointer getter copies 129 bytes for a declared `CHAR[128]` field. Windows CI runs this regression against the pinned GUI sources.

## Troubleshooting a viewer crash on Windows

For the installed GUI release `d728c1b2a`, paste this into File Explorer's address bar:

```text
%LOCALAPPDATA%\Open Ephys\configs-api10
```

Look for `activity_MM-DD-YYYY_HH_MM_SS.log` (the saved crash log), `activity.log` (the current session), and `recoveryConfig.xml` (the recoverable signal chain). If the crash handler could not finish, only `activity.log` may exist; copy it before restarting, because the next launch replaces it. Development builds under `plugin-GUI\Build` store these files beside the executable instead.

Exit Open Ephys completely before replacing the LTX DLL, keep one installed copy, then restart. A viewer add/remove does not unload a DLL already loaded by the process. The metadata-reader fix also protects position recording, since the viewer and recorder share that reader.

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
