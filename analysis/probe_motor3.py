#!/usr/bin/env python3
"""The motor.c file-path string is loaded via a constant-pool slot at 0x400d0d7c.
Find which function contains that constant-pool entry, then decompile it.
"""
import os, pathlib
HERE = pathlib.Path(__file__).parent
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
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
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    fm = program.getFunctionManager()
    ref_mgr = program.getReferenceManager()

    # 1) Refs TO the constant-pool slot 0x400d0d7c (this is the L32R target).
    slot = space.getAddress(0x400d0d7c)
    print(f"refs to constant-pool slot @ {slot}:")
    for r in ref_mgr.getReferencesTo(slot):
        print(f"  from {r.getFromAddress()} type={r.getReferenceType()}")

    # 2) Function containing that slot address.
    f = fm.getFunctionContaining(slot)
    print(f"containing function: {f}")

    # 3) Walk backwards from 0x400d0d7c to find nearest preceding function start.
    listing = program.getListing()
    it = fm.getFunctionsOverlapping(af.getAddressSet(space.getAddress(0x400d0800), slot))
    funcs_near = []
    for fn in it:
        funcs_near.append(fn)
    print(f"functions in [0x400d0800..0x400d0d7c]: {len(funcs_near)}")
    for fn in funcs_near:
        print(f"  {fn.getName()} @ {fn.getEntryPoint()}..{fn.getBody().getMaxAddress()}")

    # 4) Also — search for refs TO the string address via ANY reference type.
    string_addr = space.getAddress(0x3f403b84)
    print(f"\nall refs to string @ {string_addr}:")
    for r in ref_mgr.getReferencesTo(string_addr):
        print(f"  from {r.getFromAddress()} type={r.getReferenceType()}")
    # Iterate all instructions and check operand values
    print("\nManual scan of instructions loading 0x3f403b84 as operand...")
    inst_iter = listing.getInstructions(True)
    found = []
    count = 0
    for inst in inst_iter:
        count += 1
        for i in range(inst.getNumOperands()):
            refs = inst.getOperandReferences(i)
            for r in refs:
                if r.getToAddress().getOffset() == 0x3f403b84:
                    found.append((inst.getAddress(), inst.toString()))
    print(f"scanned {count} instructions; found {len(found)} operand refs")
    for a, s in found[:20]:
        print(f"  {a}: {s}")

    project.close(program)
finally:
    project.close()
