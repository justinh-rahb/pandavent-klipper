#!/usr/bin/env python3
"""Follow-up pyghidra probe: reopens the analyzed project, dumps the
rgb_channels[] initializer, and finds/decompiles motor_pwm_init /
motor_ledc_timer_init via cross-refs to their source-file path string.

Reads project created by run_pyghidra.py — do NOT run run_pyghidra.py first
without preserving the project.
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
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    listing = program.getListing()
    ref_mgr = program.getReferenceManager()
    fm = program.getFunctionManager()

    def read_bytes(addr_int, n):
        a = space.getAddress(addr_int)
        out = bytearray(n)
        for i in range(n):
            out[i] = mem.getByte(a.add(i)) & 0xff
        return bytes(out)

    def read_u32(addr_int):
        b = read_bytes(addr_int, 4)
        return int.from_bytes(b, "little")

    # 1) Dump memory at the PTR_DAT_400d0c18 pointer (the rgb_channels[] table).
    print("\n=== rgb_channels[] table probe ===")
    ptr_addr = 0x400d0c18
    tbl_ptr = read_u32(ptr_addr)
    print(f"PTR_DAT_400d0c18 -> 0x{tbl_ptr:08x}")
    # rgb_channels stride is 0x1c bytes; loop covers 2 elements → 0x38 bytes total.
    for i in range(2):
        base = tbl_ptr + i * 0x1c
        row = read_bytes(base, 0x1c)
        words = [int.from_bytes(row[j:j+4], "little") for j in range(0, 0x1c, 4)]
        print(f"  rgb_channels[{i}] @ 0x{base:08x}: " + " ".join(f"{w:08x}" for w in words))
        # gpio_num was accessed at offset 0x10 in rgb_init
        print(f"    +0x10 (gpio_num candidate) = {words[4]} (0x{words[4]:x})")

    # 2) Find motor.c / motor_adc.c string, then functions that reference them,
    #    then decompile any that also reference LEDC or GPIO helpers.
    print("\n=== motor.c reference sweep ===")
    di = DecompInterface()
    di.openProgram(program)
    targets = [
        "./main/motor/motor.c",
        "./main/motor/motor_adc.c",
        "motor_pwm_init",
        "motor_ledc_timer_init",
        "ledc_channel_config",
        "gpio_set_direction",
        "gpio_config",
    ]
    string_addrs = {}
    for d in listing.getDefinedData(True):
        if d.hasStringValue():
            v = str(d.getValue())
            if v in targets:
                string_addrs.setdefault(v, []).append(d.getAddress())
    for k, v in string_addrs.items():
        print(f"  string {k!r} @ {[str(a) for a in v]}")

    seen_funcs = set()
    for name in ["./main/motor/motor.c", "motor_pwm_init", "motor_ledc_timer_init",
                 "ledc_channel_config", "gpio_set_direction", "gpio_config"]:
        for saddr in string_addrs.get(name, []):
            for r in ref_mgr.getReferencesTo(saddr):
                f = fm.getFunctionContaining(r.getFromAddress())
                if f is None or f.getEntryPoint() in seen_funcs:
                    continue
                seen_funcs.add(f.getEntryPoint())
                print(f"[{name}] decompiling {f.getName()} @ {f.getEntryPoint()}")
                res = di.decompileFunction(f, 120, mon)
                if res and res.decompileCompleted():
                    c = res.getDecompiledFunction().getC()
                    label = name.replace("/", "_").replace(".", "_")
                    out = OUT_DIR / f"motor_ref_{label}__{f.getEntryPoint()}.c"
                    out.write_text(c)
                    print(f"  -> wrote {out}")

    project.close(program)
finally:
    project.close()

print("done.")
