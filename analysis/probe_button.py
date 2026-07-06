#!/usr/bin/env python3
"""Find how the stock firmware handles the illuminated user button. Search for
GPIO 12 references and adjacent gpio_config / gpio_set_direction calls."""
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
    listing = program.getListing()
    fm = program.getFunctionManager()
    di = DecompInterface()
    di.openProgram(program)

    # Find string paths for button-related source files.
    targets = [
        "./main/button/button.c",
        "./main/button.c",
        "button_task",
        "button_init",
        "button_handler",
    ]
    string_addrs = {}
    for d in listing.getDefinedData(True):
        if d.hasStringValue():
            v = str(d.getValue())
            for t in targets:
                if v == t or (t.endswith(".c") and v.endswith(t.lstrip("./"))):
                    string_addrs.setdefault(v, []).append(d.getAddress())

    print("string addrs:")
    for k, v in string_addrs.items():
        print(f"  {k!r} @ {[str(a) for a in v]}")

    # Also scan every instruction for immediates equal to 12 (GPIO number). Too
    # noisy alone, but useful cross-referenced with gpio_config strings.
    print("\nSearching for gpio pin=12 assignments in code...")
    inst_iter = listing.getInstructions(True)
    hits = []
    for inst in inst_iter:
        mnem = inst.getMnemonicString()
        if mnem != "MOVI":
            continue
        # Xtensa MOVI a2, #imm — look for the immediate
        for i in range(inst.getNumOperands()):
            reps = inst.getDefaultOperandRepresentationList(i)
            if reps is None: continue
            for r in reps:
                s = str(r)
                if s in ("0xc", "12", "#12", "#0xc"):
                    hits.append((inst.getAddress(), inst.toString()))
                    break
    print(f"found {len(hits)} MOVI #12 candidates (many will be false positives)")
    # Cluster by function
    from collections import defaultdict
    per_fn = defaultdict(list)
    for a, s in hits:
        f = fm.getFunctionContaining(a)
        if f: per_fn[f.getEntryPoint()].append((a, s))
    # Print functions with the most hits (likely button init)
    top = sorted(per_fn.items(), key=lambda kv: -len(kv[1]))[:10]
    for entry, hs in top:
        f = fm.getFunctionAt(entry)
        print(f"  fn {f.getName()} @ {entry}: {len(hs)} hits")

    project.close(program)
finally:
    project.close()
