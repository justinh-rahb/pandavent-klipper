#!/usr/bin/env python3
"""Fallback: scan all code segments for constant-pool words equal to the
motor.c string address, disassemble at nearby code, and dump surrounding
functions.
"""
import os, pathlib, struct
HERE = pathlib.Path(__file__).parent
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"

import pyghidra
pyghidra.start(verbose=False)
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.program.model.address import AddressSet

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mon = ConsoleTaskMonitor()
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    fm = program.getFunctionManager()

    # Force disassemble seg5 (IRAM main code segment).
    for blk in mem.getBlocks():
        print(f"block {blk.getName()}: {blk.getStart()}..{blk.getEnd()} exec={blk.isExecute()}")

    print("\nDisassembling seg5 (IRAM)...")
    seg5 = mem.getBlock("seg5")
    if seg5 is not None:
        tx = program.startTransaction("disasm seg5")
        try:
            cmd = DisassembleCommand(AddressSet(seg5.getStart(), seg5.getEnd()), None, True)
            cmd.applyTo(program, mon)
        finally:
            program.endTransaction(tx, True)

    # Re-run analysis to pick up new functions and references.
    from ghidra.app.plugin.core.analysis import AutoAnalysisManager
    print("Re-running auto-analysis...")
    mgr = AutoAnalysisManager.getAnalysisManager(program)
    tx = program.startTransaction("reanalyze")
    try:
        mgr.reAnalyzeAll(None)
        mgr.startAnalysis(mon)
    finally:
        program.endTransaction(tx, True)

    # Now redo the motor.c ref search.
    listing = program.getListing()
    ref_mgr = program.getReferenceManager()
    targets = ["./main/motor/motor.c", "./main/motor/motor_adc.c"]
    string_addrs = {}
    for d in listing.getDefinedData(True):
        if d.hasStringValue() and str(d.getValue()) in targets:
            string_addrs.setdefault(str(d.getValue()), []).append(d.getAddress())

    di = DecompInterface()
    di.openProgram(program)
    for tgt, addrs in string_addrs.items():
        callers = set()
        for saddr in addrs:
            for r in ref_mgr.getReferencesTo(saddr):
                f = fm.getFunctionContaining(r.getFromAddress())
                if f:
                    callers.add(f.getEntryPoint())
        print(f"\n{tgt}: {len(callers)} caller functions")
        for entry in sorted(callers, key=lambda a: a.getOffset()):
            f = fm.getFunctionAt(entry)
            res = di.decompileFunction(f, 120, mon)
            if not (res and res.decompileCompleted()):
                continue
            c = res.getDecompiledFunction().getC()
            label = tgt.replace("/", "_").replace(".", "_").strip("_")
            out = OUT_DIR / f"{label}__{entry}.c"
            out.write_text(c)
            hint = any(h in c for h in ("ledc","gpio","LEDC","GPIO","PWM","duty","fwd","rev"))
            print(f"  {'*' if hint else ' '} @ {entry} ({len(c)}B) -> {out.name}")

    project.save(program)
    project.close(program)
finally:
    project.close()
print("done.")
