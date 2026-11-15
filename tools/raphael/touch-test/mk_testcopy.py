"""Make a host-testable copy of sweet_touch.c.

Only three changes, all mechanical:
  * ARM barrier asm statements become empty statements (host is x86_64);
  * the definitions of touch_rd, touch_wr and touch_msg are renamed to *_hw
    so the harness can supply fake MMIO and a message-level I2C device.
Everything else -- including all code under test -- is compiled verbatim.
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()

n_asm = len(re.findall(r'__asm__ volatile\("[^"]*" ::: "memory"\);', text))
text = re.sub(r'__asm__ volatile\("[^"]*" ::: "memory"\);', ';', text)

renames = [
    (r'^touch_rd\(vm_offset_t base, uint32_t off\)$', 'touch_rd_hw(vm_offset_t base, uint32_t off)'),
    (r'^touch_wr\(vm_offset_t base, uint32_t off, uint32_t val\)$', 'touch_wr_hw(vm_offset_t base, uint32_t off, uint32_t val)'),
    (r'^touch_msg\(uint8_t addr, const uint8_t \*tx, unsigned nt, uint8_t \*rx,$', 'touch_msg_hw(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,'),
]
for pat, rep in renames:
    text, k = re.subn(pat, rep, text, flags=re.M)
    if k != 1:
        sys.exit("rename failed for %s (matches=%d)" % (pat, k))

open(dst, "w", encoding="utf-8", newline="\n").write(text)
print("asm statements stubbed: %d; definitions renamed: %d" % (n_asm, len(renames)))
