#!/usr/bin/env python3
"""Carve segments from an ESP32 firmware image into individual files.

Reads the ESP32 image header + segment headers directly (no esptool dep),
writes each segment to segments/segN_<load_addr>.bin, and prints a manifest
suitable for feeding into Ghidra as separate memory blocks.
"""
import struct, sys, pathlib

FW = pathlib.Path(__file__).parent.parent / "vendor/Panda-Vent/Firmware/panda_vent_v1.0.0.bin"
OUT = pathlib.Path(__file__).parent / "segments"

def main():
    data = FW.read_bytes()
    assert data[0] == 0xE9, f"bad magic {data[0]:#x}"
    seg_count = data[1]
    entry = struct.unpack_from("<I", data, 4)[0]
    print(f"entry=0x{entry:08x}  segments={seg_count}")

    OUT.mkdir(exist_ok=True)
    # ESP32 image header is 24 bytes (8 base + 16 extended)
    off = 24
    manifest = []
    for i in range(seg_count):
        load_addr, length = struct.unpack_from("<II", data, off)
        off += 8
        seg = data[off:off + length]
        off += length
        name = f"seg{i}_{load_addr:08x}.bin"
        (OUT / name).write_bytes(seg)
        print(f"  seg{i}: load=0x{load_addr:08x}  len=0x{length:05x}  -> {name}")
        manifest.append((i, load_addr, length, name))
    return manifest

if __name__ == "__main__":
    main()
