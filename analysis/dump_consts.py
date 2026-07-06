#!/usr/bin/env python3
"""Dump specific constants referenced by the motor code."""
import os, pathlib
HERE = pathlib.Path(__file__).parent
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"
import pyghidra
pyghidra.start(verbose=False)
from ghidra.base.project import GhidraProject

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    def ru32(addr):
        a = space.getAddress(addr)
        b = bytearray(4)
        for i in range(4): b[i] = mem.getByte(a.add(i)) & 0xff
        return int.from_bytes(bytes(b), "little")

    # LEDC freq constant — DAT_400d0e1c
    freq = ru32(0x400d0e1c)
    print(f"DAT_400d0e1c (LEDC frequency) = {freq} Hz (0x{freq:x})")
    # Hall threshold offset — DAT_400d0e28
    off = ru32(0x400d0e28)
    print(f"DAT_400d0e28 (hall bucket offset) = {off} ({off:#x}, signed={off if off < 0x80000000 else off - 0x100000000})")

    project.close(program)
finally:
    project.close()
