#!/usr/bin/env python3
"""End-to-end pyghidra pipeline: import ESP32 firmware, map all segments at
their real load addresses, run auto-analysis, then decompile target functions
so we can read off GPIO immediates.

Run with: analysis/venv/bin/python3 analysis/run_pyghidra.py
"""
import os, re, glob, pathlib

HERE = pathlib.Path(__file__).parent
SEG_DIR = HERE / "segments"
OUT_DIR = HERE / "decomp"
PROJECT_DIR = HERE / "ghidra_project"
PROJECT_NAME = "pandavent"
PRIMARY_SEG_NAME = "seg4_400d0020.bin"
PRIMARY_BASE = 0x400d0020

TARGETS = [
    "motor_pwm_init",
    "motor_ledc_timer_init",
    "hall_adc_init",
    "hall_get_state",
    "rgb_init",
    "rgb_light_mode",
    "rgb_switch",
    "rmt_new_led_strip_encoder",
    "app_main",
]

os.environ["GHIDRA_INSTALL_DIR"] = "/opt/homebrew/Cellar/ghidra/12.1.2/libexec"

OUT_DIR.mkdir(exist_ok=True)
PROJECT_DIR.mkdir(exist_ok=True)

import pyghidra
pyghidra.start(verbose=False)

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.flatapi import FlatProgramAPI
from ghidra.program.model.symbol import RefType
from ghidra.program.model.address import AddressFactory
from java.io import ByteArrayInputStream

primary_path = str(SEG_DIR / PRIMARY_SEG_NAME)

seg_re = re.compile(r"seg(\d+)_([0-9a-f]{8})\.bin")

with pyghidra.open_program(
    primary_path,
    project_location=str(PROJECT_DIR),
    project_name=PROJECT_NAME,
    analyze=False,
    language="Xtensa:LE:32:default",
    loader="ghidra.app.util.opinion.BinaryLoader",
) as flat_api:
    program = flat_api.getCurrentProgram()
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    mon = ConsoleTaskMonitor()

    # Rebase the primary block to PRIMARY_BASE if BinaryLoader didn't apply it.
    primary_blk = mem.getBlocks()[0]
    tx = program.startTransaction("rebase+map segments")
    try:
        if primary_blk.getStart().getOffset() != PRIMARY_BASE:
            print(f"rebasing primary block from {primary_blk.getStart()} to {PRIMARY_BASE:#x}")
            mem.moveBlock(primary_blk, space.getAddress(PRIMARY_BASE), mon)

        # Map remaining segments as their own memory blocks.
        for path in sorted(glob.glob(str(SEG_DIR / "seg*.bin"))):
            fname = os.path.basename(path)
            m = seg_re.match(fname)
            if not m:
                continue
            if fname == PRIMARY_SEG_NAME:
                continue
            seg_id = "seg" + m.group(1)
            load_addr = int(m.group(2), 16)
            data = open(path, "rb").read()
            print(f"mapping {seg_id} @ 0x{load_addr:08x} len=0x{len(data):x}")
            blk = mem.createInitializedBlock(
                seg_id, space.getAddress(load_addr),
                ByteArrayInputStream(data), len(data), mon, False,
            )
            blk.setRead(True); blk.setWrite(True)
            blk.setExecute(0x40000000 <= load_addr < 0x40400000)
    finally:
        program.endTransaction(tx, True)

    # Run auto-analysis.
    from ghidra.program.util import GhidraProgramUtilities
    from ghidra.app.plugin.core.analysis import AutoAnalysisManager
    print("running auto-analysis...")
    mgr = AutoAnalysisManager.getAnalysisManager(program)
    tx = program.startTransaction("analyze")
    try:
        mgr.initializeOptions()
        mgr.reAnalyzeAll(None)
        mgr.startAnalysis(mon)
    finally:
        program.endTransaction(tx, True)
    print("analysis done.")

    # Find target strings and decompile referring functions.
    listing = program.getListing()
    ref_mgr = program.getReferenceManager()
    fm = program.getFunctionManager()
    di = DecompInterface()
    di.openProgram(program)

    # Build string index.
    print("indexing strings...")
    string_addrs = {}
    for d in listing.getDefinedData(True):
        if d.hasStringValue():
            v = d.getValue()
            if v in TARGETS or (isinstance(v, str) and v in TARGETS):
                string_addrs.setdefault(str(v), []).append(d.getAddress())
    print(f"found strings for: {sorted(string_addrs.keys())}")

    for name in TARGETS:
        addrs = string_addrs.get(name, [])
        if not addrs:
            print(f"[{name}] no matching string in binary")
            continue
        seen_funcs = set()
        for saddr in addrs:
            for r in ref_mgr.getReferencesTo(saddr):
                from_addr = r.getFromAddress()
                f = fm.getFunctionContaining(from_addr)
                if f is None or f.getEntryPoint() in seen_funcs:
                    continue
                seen_funcs.add(f.getEntryPoint())
                print(f"[{name}] decompiling {f.getName()} @ {f.getEntryPoint()}")
                res = di.decompileFunction(f, 120, mon)
                if res and res.decompileCompleted():
                    c = res.getDecompiledFunction().getC()
                    out = OUT_DIR / f"{name}__{f.getEntryPoint()}.c"
                    out.write_text(c)
                    print(f"  -> wrote {out}")
                else:
                    print(f"  decompile failed: {res.getErrorMessage() if res else 'no result'}")

print("all done.")
