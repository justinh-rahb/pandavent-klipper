#!/usr/bin/env python3
"""Find functions in motor.c by tracing every xref to the './main/motor/motor.c'
file-path string (used as the tag argument in every ESP_LOG* macro call from
that translation unit), then decompile each caller.
"""
import os, pathlib
HERE = pathlib.Path(__file__).parent
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"

os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"
import pyghidra
pyghidra.start(verbose=False)
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.base.project import GhidraProject

project = GhidraProject.openProject(str(PROJECT_DIR / PROJECT_NAME), PROJECT_NAME, False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mon = ConsoleTaskMonitor()
    ref_mgr = program.getReferenceManager()
    fm = program.getFunctionManager()
    listing = program.getListing()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()

    # Find target string addresses.
    targets = ["./main/motor/motor.c", "./main/motor/motor_adc.c"]
    string_addrs = {}
    for d in listing.getDefinedData(True):
        if d.hasStringValue() and str(d.getValue()) in targets:
            string_addrs.setdefault(str(d.getValue()), []).append(d.getAddress())
    print("string addrs:", {k: [str(a) for a in v] for k, v in string_addrs.items()})

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
            # Only save if it looks GPIO-related: contains ledc/gpio helper calls.
            hints = ("ledc", "gpio", "LEDC", "GPIO", "PWM", "pwm", "fwd", "rev", "duty")
            has_hint = any(h in c for h in hints)
            label = tgt.replace("/", "_").replace(".", "_").strip("_")
            out = OUT_DIR / f"{label}__{entry}.c"
            out.write_text(c)
            print(f"  {'*' if has_hint else ' '} {f.getName()} @ {entry} ({len(c)} bytes) -> {out.name}")

    project.close(program)
finally:
    project.close()
print("done.")
