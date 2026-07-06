#!/usr/bin/env python3
"""Hunt for the button-LED wiring:
1. String search for LED-related identifiers ("led_button", "btn_led", etc.)
2. Find the caller of the button-task creator (FUN_400df080). Its callbacks
   are the short-press and long-press handlers — those likely poke the LED.
3. Look at gpio_set_level / gpio_set_direction calls to find any GPIO writes
   we haven't mapped yet.
"""
import os, pathlib
HERE = pathlib.Path(__file__).parent
PROJECT_DIR = HERE / "ghidra_project" / "pandavent"
os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"
import pyghidra
pyghidra.start(verbose=False)
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

project = GhidraProject.openProject(str(PROJECT_DIR.resolve()), "pandavent", False)
try:
    program = project.openProgram("/", "seg4_400d0020.bin", True)
    mon = ConsoleTaskMonitor()
    listing = program.getListing()
    fm = program.getFunctionManager()
    ref_mgr = program.getReferenceManager()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    di = DecompInterface(); di.openProgram(program)

    print("=== 1) strings with LED / button hints ===")
    hits = []
    for d in listing.getDefinedData(True):
        if d.hasStringValue():
            v = str(d.getValue()).lower()
            if any(t in v for t in ["led_button", "btn_led", "button_led",
                                    "led_on", "led_off", "led_task",
                                    "led_init", "button.c", "button/",
                                    "sys_led", "status_led", "power_led"]):
                hits.append((d.getAddress(), d.getValue()))
    for a, v in hits:
        print(f"  {a}: {v!r}")

    print("\n=== 2) callers of button_task creator FUN_400df080 ===")
    tgt = space.getAddress(0x400df080)
    callers = set()
    for r in ref_mgr.getReferencesTo(tgt):
        f = fm.getFunctionContaining(r.getFromAddress())
        if f:
            callers.add(f.getEntryPoint())
    out_dir = HERE / "decomp"
    for entry in callers:
        f = fm.getFunctionAt(entry)
        res = di.decompileFunction(f, 120, mon)
        if not (res and res.decompileCompleted()): continue
        c = res.getDecompiledFunction().getC()
        out = out_dir / f"button_caller_{entry}.c"
        out.write_text(c)
        print(f"  {f.getName()} @ {entry} -> {out.name}")

    project.close(program)
finally:
    project.close()
