#!/usr/bin/env python3
"""Find callers of a given function and decompile them."""
import os, sys, pathlib
HERE = pathlib.Path(__file__).parent
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"

TARGET = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x400de2b4

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
    ref_mgr = program.getReferenceManager()
    di = DecompInterface()
    di.openProgram(program)

    target = space.getAddress(TARGET)
    callers = set()
    for r in ref_mgr.getReferencesTo(target):
        f = fm.getFunctionContaining(r.getFromAddress())
        if f:
            callers.add(f.getEntryPoint())
    print(f"callers of 0x{TARGET:x}: {len(callers)}")
    for entry in sorted(callers, key=lambda a: a.getOffset()):
        f = fm.getFunctionAt(entry)
        res = di.decompileFunction(f, 120, mon)
        if not (res and res.decompileCompleted()):
            continue
        c = res.getDecompiledFunction().getC()
        out = OUT_DIR / f"caller_of_{TARGET:x}__{entry}.c"
        out.write_text(c)
        print(f"  {f.getName()} @ {entry} -> {out.name} ({len(c)}B)")

    project.close(program)
finally:
    project.close()
