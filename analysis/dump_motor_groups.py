#!/usr/bin/env python3
"""Dump the 4 motor-group config structs pointed to by PTR_PTR_400d0d9c."""
import os, pathlib
HERE = pathlib.Path(__file__).parent
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"
import pyghidra
pyghidra.start(verbose=False)
from ghidra.base.project import GhidraProject
from ghidra.util.task import ConsoleTaskMonitor

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()

    def rb(addr, n):
        a = space.getAddress(addr); out = bytearray(n)
        for i in range(n): out[i] = mem.getByte(a.add(i)) & 0xff
        return bytes(out)
    def ru32(addr): return int.from_bytes(rb(addr, 4), "little")

    slot = 0x400d0d9c
    arr_ptr = ru32(slot)
    print(f"PTR_PTR_400d0d9c @ 0x{slot:x} -> arr_ptr = 0x{arr_ptr:08x}")
    # local_30[i] = *(u32*)(arr_ptr + i*4) → 4 element pointer table
    grp_ptrs = [ru32(arr_ptr + i*4) for i in range(4)]
    print("group pointer table:")
    for i, p in enumerate(grp_ptrs):
        print(f"  local_30[{i}] = 0x{p:08x}")
    # Each struct is 0x24 bytes.
    for i, p in enumerate(grp_ptrs):
        b = rb(p, 0x24)
        words = [int.from_bytes(b[j:j+4], "little") for j in range(0, 0x24, 4)]
        print(f"\ngroup {i} struct @ 0x{p:08x}:")
        for j, w in enumerate(words):
            print(f"  +0x{j*4:02x}: 0x{w:08x} ({w})")
    project.close(program)
finally:
    project.close()
