# Ghidra preScript (Jython 2.7): create memory blocks for every ESP32 segment
# except the one already imported as the primary file.
#
# The wrapper imports seg4 (IROM, main code) as the primary. This script maps
# the remaining segments — DROM (strings live here!), IRAM, RTC — at their
# real load addresses so cross-references resolve properly.

import os, glob, re
from ghidra.program.model.address import AddressFactory
from ghidra.app.util.importer import MessageLog
from ghidra.util.task import ConsoleTaskMonitor

SEG_DIR = os.environ["SEGMENT_DIR"]
PRIMARY = os.environ.get("PRIMARY_SEG", "seg4")

af = currentProgram.getAddressFactory()
mem = currentProgram.getMemory()
mon = ConsoleTaskMonitor()

RE = re.compile(r"seg(\d+)_([0-9a-f]{8})\.bin")

for path in sorted(glob.glob(os.path.join(SEG_DIR, "seg*.bin"))):
    fname = os.path.basename(path)
    m = RE.match(fname)
    if not m:
        continue
    seg_id = "seg" + m.group(1)
    if seg_id == PRIMARY:
        continue
    load_addr = int(m.group(2), 16)
    data = open(path, "rb").read()
    length = len(data)
    addr = af.getDefaultAddressSpace().getAddress(load_addr)
    print("mapping %s @ 0x%08x len=0x%x" % (seg_id, load_addr, length))
    # writable=True to be permissive; executable only for IRAM/IROM ranges
    is_exec = 0x40000000 <= load_addr < 0x40400000
    from java.io import ByteArrayInputStream
    blk = mem.createInitializedBlock(
        seg_id, addr, ByteArrayInputStream(data), length, mon, False
    )
    blk.setRead(True)
    blk.setWrite(True)
    blk.setExecute(is_exec)

print("segment mapping done")
