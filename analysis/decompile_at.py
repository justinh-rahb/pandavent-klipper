#!/usr/bin/env python3
"""Decompile the function(s) containing given addresses."""
import os, sys, pathlib
HERE = pathlib.Path(__file__).parent
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"

ADDRS = [int(a, 16) for a in sys.argv[1:]] or [0x400de2e1, 0x400de312]

import pyghidra
pyghidra.start(verbose=False)
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mon = ConsoleTaskMonitor()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    fm = program.getFunctionManager()
    di = DecompInterface()
    di.openProgram(program)

    seen = set()
    for a in ADDRS:
        f = fm.getFunctionContaining(space.getAddress(a))
        if not f or f.getEntryPoint() in seen:
            print(f"0x{a:08x}: no fn or dup")
            continue
        seen.add(f.getEntryPoint())
        res = di.decompileFunction(f, 120, mon)
        if not (res and res.decompileCompleted()):
            print(f"decompile failed for {f}")
            continue
        c = res.getDecompiledFunction().getC()
        out = OUT_DIR / f"at_{f.getEntryPoint()}.c"
        out.write_text(c)
        print(f"0x{a:08x} in {f.getName()} @ {f.getEntryPoint()} -> {out.name} ({len(c)}B)")

    project.close(program)
finally:
    project.close()
