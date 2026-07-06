# Ghidra postscript (Jython 2.7). Runs after auto-analysis.
# For each target __func__ string, locate the referring function and dump its
# decompiled C. The gpio_num immediates loaded into the LEDC/RMT/GPIO config
# structs will be visible directly in the decompilation.
#
# Output: analysis/decomp/<funcname>.c
#
# @category PandaVent

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [
    "motor_pwm_init",
    "motor_ledc_timer_init",
    "hall_adc_init",
    "hall_get_state",
    "rgb_init",
    "rgb_light_mode",
]

OUT_DIR = os.path.join(os.environ.get("ANALYSIS_OUT", "/tmp"), "decomp")
try:
    os.makedirs(OUT_DIR)
except OSError:
    pass

def find_string_addr(needle):
    # Ghidra: iterate defined strings in memory
    listing = currentProgram.getListing()
    data_iter = listing.getDefinedData(True)
    for d in data_iter:
        if d.hasStringValue():
            v = d.getValue()
            if v == needle:
                return d.getAddress()
    return None

def containing_function(addr):
    fm = currentProgram.getFunctionManager()
    return fm.getFunctionContaining(addr)

def decompile(func):
    di = DecompInterface()
    di.openProgram(currentProgram)
    res = di.decompileFunction(func, 60, ConsoleTaskMonitor())
    if res and res.decompileCompleted():
        return res.getDecompiledFunction().getC()
    return None

ref_mgr = currentProgram.getReferenceManager()

for name in TARGETS:
    print("=== target:", name)
    saddr = find_string_addr(name)
    if saddr is None:
        print("  string not found")
        continue
    print("  string @", saddr)
    refs = ref_mgr.getReferencesTo(saddr)
    seen = set()
    for r in refs:
        from_addr = r.getFromAddress()
        f = containing_function(from_addr)
        if f is None or f.getEntryPoint() in seen:
            continue
        seen.add(f.getEntryPoint())
        print("    ref from func @", f.getEntryPoint(), f.getName())
        c = decompile(f)
        if c:
            out_path = os.path.join(OUT_DIR, "%s__%s.c" % (name, f.getEntryPoint()))
            with open(out_path, "w") as fh:
                fh.write(c)
            print("    -> wrote", out_path)

print("done.")
