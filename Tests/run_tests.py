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
parser.add_argument("--cmake-executable", default="cmake")
args = parser.parse_args()


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
    position_padding = math.floor(delays[0] / 1000 * 50 + 0.5)
    eeg_padding = math.floor(delays[1] / 1000 * 5000 + 0.5)
    h, data = read_file(Path(str(base) + ".pos"))
    assert int(h["num_pos_samples"]) == position_padding + 3
    assert int(h["startup_padding_samples"]) == position_padding
    rows = list(struct.iter_unpack(">i8H", data))
    assert len(rows) == position_padding + 3
    for index, row in enumerate(rows[:position_padding]):
        assert row[0] == index * 1920
        assert row[1:5] == (1023, 1023, 1023, 1023)
        assert row[5:] == (0, 0, 0, 0)
    assert rows[position_padding][0] == position_padding * 1920
    if nan_head:
        assert rows[position_padding][1:7] == (1023, 1023, 1023, 1023, 0, 0)
    else:
        assert rows[position_padding][1:7] == (101, 102, 103, 104, 105, 106)
    assert all(a[0] < b[0] for a, b in zip(rows, rows[1:]))
    for ext in (".egf", ".egf2"):
        h, data = read_file(Path(str(base) + ext))
        assert int(h["num_EEG_samples"]) == eeg_padding + 3
        assert int(h["startup_padding_samples"]) == eeg_padding
        assert data[:eeg_padding] == bytes(eeg_padding)
        assert data[eeg_padding:] == bytes((50, 53, 56)), (name, ext, data[eeg_padding:])
    assert not Path(str(base) + ".egf3").exists()
    h, data = read_file(Path(str(base) + ".1"))
    assert int(h["num_spikes"]) == 1 and len(data) == 216
    expected = round((delays[2] / 1000 + 0.02) * 96000)
    assert all(struct.unpack_from(">i", data, channel * 54)[0] == expected for channel in range(4))
    assert all(data[channel * 54 + 4] == 42 for channel in range(4))
    h, data = read_file(Path(str(base) + ".ttl"))
    line, timestamp, state = data.decode().split()
    assert line == "ttl_2" and state == "1"
    assert abs(float(timestamp) - (delays[2] / 1000 + 0.01)) < 1e-10
    for node, delay in zip((10, 20, 30), delays):
        meta = json.loads(Path(str(base) + f".timing-{node}.json").read_text())
        assert meta["complete"] and not meta["error"], meta
        assert meta["alignment_estimated"]
        assert abs(meta["record_node_offset_seconds"] - delay / 1000) < 1e-12
        assert len(meta["participants"]) == 3
        if node == 10:
            assert meta["position_padding_samples"] == position_padding
        elif node == 20:
            assert len(meta["eeg_channels"]) == 2
            assert all(c["padding_samples"] == eeg_padding for c in meta["eeg_channels"])


def run(work):
    exe = work / ("recording-timing-test.exe" if os.name == "nt" else "recording-timing-test")
    sources = [ROOT / "Tests/recording_timing_test.cpp", ROOT / "Source/LTXRecordEnginePlugin.cpp", ROOT / "Source/LTXFile.cpp"]
    compiler = args.compiler
    if args.cmake:
        build = work / "build"
        subprocess.run([args.cmake_executable, "-S", str(ROOT / "Tests"), "-B", str(build)], check=True)
        subprocess.run([args.cmake_executable, "--build", str(build), "--config", "Release", "--parallel", "2"], check=True)
        candidates = [build / "Release" / exe.name, build / exe.name]
        exe = next(p for p in candidates if p.is_file())
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
    meta = json.loads((output / "missing-anchor.timing-10.json").read_text())
    assert not meta["complete"] and "without" in meta["error"]
    for name in ("missing-participant", "missing-native", "capacity", "duplicate"):
        meta = json.loads((output / f"{name}.timing-10.json").read_text())
        assert not meta["complete"] and meta["error"], meta
    meta = json.loads((output / "empty.timing-10.json").read_text())
    assert meta["complete"] and not meta["error"] and meta["participants"][0]["inactive"]
    h, data = read_file(output / "inactive.pos")
    assert int(h["startup_padding_samples"]) == 0 and len(data) == 3 * 20
    meta = json.loads((output / "inactive.timing-10.json").read_text())
    assert meta["complete"] and meta["common_epoch_unix_ms"] == 1700000000400
    if args.keep_output:
        shutil.copytree(output, args.keep_output, dirs_exist_ok=True)
    print("PASS: all six recorder orders, reversed callbacks, padding bytes/counts, spike/TTL shifts,")
    print("      two EEG channels plus ignored memory stream, sample phase, NaN head, repeat recording,")
    print("      fractional rounding, 50 ms warning boundary, concurrent origins, empty/inactive nodes,")
    print("      missing/duplicate anchors, missing participants, excessive skew, and staging-limit failure.")


with tempfile.TemporaryDirectory(prefix="ltx-timing-tests-") as folder:
    run(Path(folder))

if args.gui_source:
    gui = args.gui_source.resolve()
    command = [args.compiler, "-std=c++17", "-fsyntax-only", "-DOEPLUGIN", "-DNDEBUG=1",
               "-I" + str(gui / "JuceLibraryCode"), "-I" + str(gui / "JuceLibraryCode/modules"),
               "-I" + str(gui / "Plugins/Headers"),
               str(ROOT / "Source/LTXRecordEnginePlugin.cpp"), str(ROOT / "Source/LTXFile.cpp"),
               str(ROOT / "Source/OpenEphysLib.cpp")]
    subprocess.run(command, check=True)
    print("PASS: compile check against configured GUI headers:", gui)
