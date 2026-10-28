#!/usr/bin/env python3
"""Execute the production LTX engine with an API shim and inspect its binary files.

Optional --gui-source checks the changed files against the actual configured GUI
headers too. This is not a GUI/hardware integration test.
"""
import argparse
import itertools
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--gui-source", type=Path)
parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
parser.add_argument("--keep-output", type=Path)
parser.add_argument("--cmake", action="store_true", help="build using CMake, including MSVC on Windows")
parser.add_argument("--native-api", type=Path, help="also run the real GUI metadata regression (requires --cmake)")
parser.add_argument("--cmake-executable", default="cmake")
args = parser.parse_args()
if args.native_api and not args.cmake:
    parser.error("--native-api requires --cmake")


def read_file(path):
    raw = path.read_bytes()
    marker = raw.find(b"data_start")
    header = raw[:marker] if marker >= 0 else raw
    fields = dict(line.split(" ", 1) for line in header.decode("ascii").splitlines() if " " in line)
    if marker >= 0:
        assert raw.endswith(b"\r\ndata_end"), path
        payload = raw[marker + len(b"data_start"):-len(b"\r\ndata_end")]
    else:
        payload = b""
    return fields, payload


def verify(root, name, delays, suffix="", nan_head=False):
    base = root / (name + suffix)
    h, data = read_file(Path(str(base) + ".pos"))
    assert int(h["num_pos_samples"]) == 3
    assert h["position_metadata_source"] == "legacy_viewer"
    assert "data_source" not in h
    rows = list(struct.iter_unpack(">i8H", data))
    assert len(rows) == 3 and rows[0][0] == 0
    if nan_head:
        assert rows[0][1:7] == (1023, 1023, 1023, 1023, 0, 0)
    else:
        assert rows[0][1:7] == (101, 102, 103, 104, 105, 106)
    assert all(a[0] < b[0] for a, b in zip(rows, rows[1:]))
    for ext in (".egf", ".egf2"):
        h, data = read_file(Path(str(base) + ext))
        assert int(h["num_EEG_samples"]) == 3
        assert data == bytes((50, 53, 56)), (name, ext, data)
    assert not Path(str(base) + ".egf3").exists()
    h, data = read_file(Path(str(base) + ".1"))
    assert int(h["num_spikes"]) == 1 and len(data) == 216
    expected = round(0.02 * 96000)
    assert all(struct.unpack_from(">i", data, channel * 54)[0] == expected for channel in range(4))
    assert all(data[channel * 54 + 4] == 42 for channel in range(4))
    h, data = read_file(Path(str(base) + ".ttl"))
    line, timestamp, state = data.decode().split()
    assert line == "ttl_2" and state == "1"
    assert abs(float(timestamp) - 0.01) < 1e-10
    for node, delay in zip((10, 20, 30), delays):
        meta = json.loads(Path(str(base) + f".timing-{node}.json").read_text())
        assert meta["complete"] and not meta["error"], meta
        assert meta["estimated"] and not meta["correction_applied"] and meta["timing_available"]
        assert abs(meta["record_node_offset_seconds"] - delay / 1000) < 1e-12
        assert len(meta["participants"]) == 3
        extensions = {10: (".pos",), 20: (".egf", ".egf2"), 30: (".set", ".1", ".ttl")}[node]
        for ext in extensions:
            header, _ = read_file(Path(str(base) + ext))
            assert int(header["record_node_start_offset_ms"]) == delay
            assert header["startup_timing_status"].strip() == "measured_uncorrected"
            assert header["startup_timing_correction_applied"] == "0"
            assert not any("padding" in key or key == "timestamp_start_offset_seconds" for key in header)


def verify_camera(output, name, uncalibrated=False, unknown=False):
    header, data = read_file(output / (name + ".pos"))
    assert header["data_source"] == "open-ephys-plugin-camera v0.1"
    assert header["position_metadata_source"] == "camera_tracker"
    assert header["camera_tracker.window"].strip() == ("0 0 0 0" if unknown else "100 50 1540 1130")
    assert header["camera_tracker.frame_size"] == "1920 1200"
    assert int(header["num_pos_samples"]) == 8
    rows = list(struct.iter_unpack(">i8H", data))
    scale = 1 if unknown else 1022 / 1440
    if unknown:
        assert header["position_coordinate_transform"] == "identity"
        assert int(header["window_max_x"]) == 700
    else:
        assert header["position_coordinate_transform"] == "crop_relative_uniform_scale"
        assert float(header["position_coordinate_scale"]) == scale
        assert float(header["position_coordinate_origin_x"]) == 100
        assert float(header["position_coordinate_origin_y"]) == 50
        assert rows[0][1:5] == (0, 0, 0, 0)
        assert rows[1][1] == round((1023 - 100) * scale) != 1023
        assert rows[1][2] == round((1023 - 50) * scale) != 1023
        assert rows[2][1] == round((1539 - 100) * scale)
        assert rows[2][2] == round((1129 - 50) * scale)
        assert int(header["window_max_x"]) == 1022
        assert int(header["window_max_y"]) == math.floor(1080 * scale + 0.5)
    for index in (3, 4, 5, 6):
        assert rows[index][1:5] == (1023,) * 4
    assert rows[1][5:7] == (65535, 65535)  # actual areas saturate, never wrap
    assert rows[2][5:7] == (65535, 65535)
    assert all(rows[index][5:7] == (0, 0) for index in (3, 4, 5))
    assert rows[7][5:7] == (1, 1)  # positive fractional areas do not become "missing"
    assert all(row[7:] == (0, 0) for row in rows)
    assert math.isnan(float(header["pixels_per_metre"])) if uncalibrated else abs(float(header["pixels_per_metre"]) - 800 * scale) < 1e-10


def run(work):
    exe = work / ("recording-timing-test.exe" if os.name == "nt" else "recording-timing-test")
    sources = [ROOT / "Tests/recording_timing_test.cpp", ROOT / "Source/LTXRecordEnginePlugin.cpp", ROOT / "Source/LTXFile.cpp"]
    compiler = args.compiler
    if args.cmake:
        build = work / "build"
        configure = [args.cmake_executable, "-S", str(ROOT / "Tests"), "-B", str(build)]
        if args.native_api:
            configure.append("-DLTX_GUI_SOURCE=" + str(args.native_api.resolve()))
        subprocess.run(configure, check=True)
        subprocess.run([args.cmake_executable, "--build", str(build), "--config", "Release", "--parallel", "2"], check=True)
        candidates = [build / "Release" / exe.name, build / exe.name]
        exe = next(p for p in candidates if p.is_file())
        if args.native_api:
            native_exe = exe.with_name("native-metadata-test" + (".exe" if os.name == "nt" else ""))
            subprocess.run([str(native_exe)], check=True, timeout=30)
        command = None
    elif Path(compiler).name.lower() in ("cl", "cl.exe"):
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/MD", "/W4",
                   "/I" + str(ROOT / "Tests/stubs"), "/I" + str(ROOT / "Source"),
                   *map(str, sources), "/Fe:" + str(exe)]
    else:
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Wno-unused-parameter",
                   "-Wno-writable-strings", "-Wno-format-security",
                   "-I", str(ROOT / "Tests/stubs"), "-I", str(ROOT / "Source"),
                   *map(str, sources), "-o", str(exe)]
    if command:
        subprocess.run(command, cwd=work, check=True)
    output = work / "output"
    output.mkdir()
    subprocess.run([str(exe), str(output)], check=True, timeout=30)
    for number, order in enumerate(itertools.permutations(range(3))):
        delays = [0, 0, 0]
        for rank, mode in enumerate(order):
            delays[mode] = rank * 200
        verify(output, f"order-{number}", delays)
        messages = (output / f"order-{number}/messages.txt").read_text().splitlines()
        assert len(messages) == 2, messages
    verify(output, "tied", [0, 0, 0])
    assert not (output / "tied/messages.txt").read_text()
    verify(output, "fractional", [397, 201, 0])
    verify(output, "nan-head", [400, 0, 200], nan_head=True)
    verify(output, "repeat", [0, 400, 200])
    verify(output, "repeat", [400, 0, 0], suffix=" r2")
    verify(output, "threshold", [50, 51, 0])
    messages = (output / "threshold/messages.txt").read_text().splitlines()
    assert len(messages) == 1 and "Node 20" in messages[0], messages
    verify(output, "large-discrepancy", [600001, 0, 200])
    # The diagnostic discrepancy must never change the binary payload, regardless
    # of which mode is first or the size of the difference.
    for ext in (".pos", ".egf", ".egf2", ".1", ".ttl"):
        reference = read_file(output / ("tied" + ext))[1]
        for name in [*(f"order-{i}" for i in range(6)), "fractional", "threshold", "large-discrepancy"]:
            assert read_file(output / (name + ext))[1] == reference, (name, ext)
    for name in ("missing-anchor", "missing-participant", "duplicate"):
        meta = json.loads((output / f"{name}.timing-10.json").read_text())
        assert meta["complete"] and not meta["timing_available"] and not meta["correction_applied"], meta
        header, payload = read_file(output / f"{name}.pos")
        assert header["record_node_start_offset_ms"].strip() == "unavailable"
        assert payload == read_file(output / "tied.pos")[1]
    for name in ("missing-native", "capacity"):
        meta = json.loads((output / f"{name}.timing-10.json").read_text())
        assert not meta["complete"] and meta["error"], meta
    meta = json.loads((output / "empty.timing-10.json").read_text())
    assert meta["complete"] and not meta["error"] and meta["participants"][0]["inactive"]
    h, data = read_file(output / "inactive.pos")
    assert len(data) == 3 * 20 and int(h["record_node_start_offset_ms"]) == 0
    meta = json.loads((output / "inactive.timing-10.json").read_text())
    assert meta["complete"] and meta["common_epoch_unix_ms"] == 1700000000400
    header, payload = read_file(output / "fractional-rate.pos")
    rate = float(header["sample_rate"].split()[0])
    assert 29.969 < rate < 29.971
    times = [row[0] for row in struct.iter_unpack(">i8H", payload)]
    assert len(times) == 4
    assert all(abs(t - i / rate * 96000) < 1 for i, t in enumerate(times)), times
    assert header["startup_timing_status"].strip() == "unavailable"
    verify_camera(output, "camera")
    verify_camera(output, "camera-uncalibrated", uncalibrated=True)
    verify_camera(output, "camera-unknown-bounds", uncalibrated=True, unknown=True)
    if args.keep_output:
        shutil.copytree(output, args.keep_output, dirs_exist_ok=True)
    print("PASS: all six recorder orders, reversed callbacks, repeat recordings, >50 ms warnings,")
    print("      unchanged binary data across start discrepancies, missing/duplicate software anchors,")
    print("      camera metadata/source headers, crop geometry, coordinate scaling, safe uint16 areas,")
    print("      NaNs/infinities, legacy fallback, sample phase, native-origin/staging failure checks.")


with tempfile.TemporaryDirectory(prefix="ltx-timing-tests-") as folder:
    run(Path(folder))

if args.gui_source:
    gui = args.gui_source.resolve()
    command = [args.compiler, "-std=c++17", "-fsyntax-only", "-DOEPLUGIN", "-DNDEBUG=1",
               "-I" + str(gui / "JuceLibraryCode"), "-I" + str(gui / "JuceLibraryCode/modules"),
               "-I" + str(gui / "Plugins/Headers"),
               str(ROOT / "Source/LTXRecordEnginePlugin.cpp"), str(ROOT / "Source/LTXFile.cpp"),
               str(ROOT / "Source/OpenEphysLib.cpp"),
               str(ROOT / "Source/LTXPosVisualizerPlugin.cpp"),
               str(ROOT / "Source/LTXPosVisualizerPluginEditor.cpp"),
               str(ROOT / "Source/LTXPosVisualizerPluginCanvas.cpp")]
    subprocess.run(command, check=True)
    print("PASS: compile check against configured GUI headers:", gui)
