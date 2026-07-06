#!/usr/bin/env python3
"""Follow xrefs to 'button_task' string and decompile."""
import os, pathlib
HERE = pathlib.Path(__file__).parent
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
OUT_DIR = HERE / "decomp"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"
import pyghidra
pyghidra.start(verbose=False)
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mon = ConsoleTaskMonitor()
    ref_mgr = program.getReferenceManager()
    fm = program.getFunctionManager()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    di = DecompInterface()
    di.openProgram(program)

    string_addr = space.getAddress(0x3f403e90)
    refs = list(ref_mgr.getReferencesTo(string_addr))
    print(f"refs to 'button_task' string: {len(refs)}")
    for r in refs:
        print(f"  from {r.getFromAddress()} type={r.getReferenceType()}")

    seen = set()
    for r in refs:
        f = fm.getFunctionContaining(r.getFromAddress())
        if not f or f.getEntryPoint() in seen: continue
        seen.add(f.getEntryPoint())
        res = di.decompileFunction(f, 120, mon)
        if not res or not res.decompileCompleted(): continue
        c = res.getDecompiledFunction().getC()
        out = OUT_DIR / f"button_{f.getEntryPoint()}.c"
        out.write_text(c)
        print(f"  -> {out.name} ({len(c)}B)")

    project.close(program)
finally:
    project.close()
