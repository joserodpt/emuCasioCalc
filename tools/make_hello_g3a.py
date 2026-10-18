#!/usr/bin/env python3
"""Builds tests/hello.g3a: a tiny hand-assembled add-in for smoke-testing the
emulator before a real SH toolchain is installed.

It enables colour, draws a pattern straight into VRAM, prints two lines with
PrintXY and loops on GetKey until EXIT. Not a valid add-in for the real
calculator (no checksum, name or icons), only for emuCasioFxCG.

usage: make_hello_g3a.py OUT.g3a
"""
import struct
import sys

HEADER_SIZE = 0x7000
MAGIC = bytes(b ^ 0xFF for b in b"USBPower")
BASE = 0x00300000


class Asm:
    """Two-pass assembler: every item knows its size, ops may use labels."""

    def __init__(self):
        self.items = []

    def label(self, name):
        self.items.append(("label", name))

    def op(self, encode):
        self.items.append(("op", encode))

    def raw(self, *halfwords):
        for h in halfwords:
            self.op(lambda a, L, h=h: h)

    def long(self, value):
        self.items.append(("data", struct.pack(">I", value)))

    def word(self, value):
        self.items.append(("data", struct.pack(">h", value)))

    def asciz(self, text):
        self.items.append(("data", text.encode("ascii") + b"\0"))

    def align(self, n):
        self.items.append(("align", n))

    def assemble(self, base):
        labels, addr = {}, base
        for kind, v in self.items:
            if kind == "label":
                labels[v] = addr
            elif kind == "op":
                addr += 2
            elif kind == "data":
                addr += len(v)
            else:
                addr += -addr % v
        out, addr = bytearray(), base
        for kind, v in self.items:
            if kind == "op":
                out += struct.pack(">H", v(addr, labels) & 0xFFFF)
                addr += 2
            elif kind == "data":
                out += v
                addr += len(v)
            elif kind == "align":
                pad = -addr % v
                out += b"\x00\x09" * (pad // 2) if pad % 2 == 0 else b"\0" * pad
                addr += pad
        return bytes(out)


def disp(value, bits):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    assert lo <= value <= hi, f"branch/literal out of range: {value}"
    return value & ((1 << bits) - 1)


def udisp(value, bits):
    assert 0 <= value < (1 << bits), f"literal out of range (must be after the instruction): {value}"
    return value


# Encoders for the instructions used below (SH-4A manual, chapter 10).
def mov_imm(imm, n): return lambda a, L: 0xE000 | n << 8 | (imm & 0xFF)
def add_imm(imm, n): return lambda a, L: 0x7000 | n << 8 | (imm & 0xFF)
def mov(m, n):       return lambda a, L: 0x6003 | n << 8 | m << 4
def jsr(n):          return lambda a, L: 0x400B | n << 8
def bf(lbl):         return lambda a, L: 0x8B00 | disp((L[lbl] - (a + 4)) // 2, 8)
def mova(lbl):       return lambda a, L: 0xC700 | udisp((L[lbl] - ((a & ~3) + 4)) // 4, 8)
def movl_pc(lbl, n): return lambda a, L: 0xD000 | n << 8 | udisp((L[lbl] - ((a & ~3) + 4)) // 4, 8)
def movw_pc(lbl, n): return lambda a, L: 0x9000 | n << 8 | udisp((L[lbl] - (a + 4)) // 2, 8)
def push(m):         return lambda a, L: 0x2F06 | m << 4         # mov.l Rm,@-r15
def pop(n):          return lambda a, L: 0x60F6 | n << 8         # mov.l @r15+,Rn
def cmp_eq(m, n):    return lambda a, L: 0x3000 | n << 8 | m << 4
NOP, RTS = 0x0009, 0x000B


def syscall(asm, const_label):
    asm.op(movl_pc(const_label, 0))
    asm.op(jsr(8))  # r8 holds SYSCALL_ENTRY
    asm.raw(NOP)


def print_xy(asm, x, y, msg_label, color):
    asm.op(mov_imm(x, 4))
    asm.op(mov_imm(y, 5))
    asm.op(mova(msg_label))
    asm.op(mov(0, 6))
    asm.op(mov_imm(0, 7))      # TEXT_MODE_NORMAL
    asm.op(mov_imm(color, 1))
    asm.op(push(1))            # 5th arg on the stack
    syscall(asm, "SC_PRINTXY")
    asm.op(add_imm(4, 15))


def build():
    asm = Asm()
    asm.label("start")
    asm.raw(0x4F22)            # sts.l pr,@-r15
    for r in (8, 9, 10, 11):
        asm.op(push(r))
    asm.op(movl_pc("K_SYSCALL", 8))

    asm.op(mov_imm(1, 4))
    syscall(asm, "SC_ENABLECOLOR")
    syscall(asm, "SC_GETVRAM")
    asm.op(mov(0, 9))          # r9 = VRAM pointer

    # for y in 0..215: for x in 0..383: *vram++ = x * y
    asm.op(mov_imm(0, 10))
    asm.label("yloop")
    asm.op(mov_imm(0, 11))
    asm.label("xloop")
    asm.raw(0x0BA7)            # mul.l r10,r11
    asm.raw(0x001A)            # sts macl,r0
    asm.raw(0x2901)            # mov.w r0,@r9
    asm.op(add_imm(2, 9))
    asm.op(add_imm(1, 11))
    asm.op(movw_pc("K_WIDTH", 1))
    asm.op(cmp_eq(1, 11))
    asm.op(bf("xloop"))
    asm.op(add_imm(1, 10))
    asm.op(movw_pc("K_HEIGHT", 1))
    asm.op(cmp_eq(1, 10))
    asm.op(bf("yloop"))

    print_xy(asm, 1, 1, "MSG_HELLO", 1)   # TEXT_COLOR_BLUE
    print_xy(asm, 1, 8, "MSG_EXIT", 4)    # TEXT_COLOR_RED

    asm.label("keyloop")
    asm.op(add_imm(-4, 15))
    asm.op(mov(15, 4))
    syscall(asm, "SC_GETKEY")
    asm.op(pop(1))
    asm.op(movw_pc("K_EXIT", 2))
    asm.op(cmp_eq(2, 1))
    asm.op(bf("keyloop"))

    asm.op(mov_imm(0, 0))
    for r in (11, 10, 9, 8):
        asm.op(pop(r))
    asm.raw(0x4F26, RTS, NOP)  # lds.l @r15+,pr; rts; nop

    asm.align(4)
    for name, value in [("K_SYSCALL", 0x80020070), ("SC_ENABLECOLOR", 0x0921),
                        ("SC_GETVRAM", 0x01E6), ("SC_PRINTXY", 0x18F9), ("SC_GETKEY", 0x0EAB)]:
        asm.label(name)
        asm.long(value)
    for name, value in [("K_WIDTH", 384), ("K_HEIGHT", 216), ("K_EXIT", 30002)]:
        asm.label(name)
        asm.word(value)
    for name, text in [("MSG_HELLO", "  Hi from emuCasioFxCG!"), ("MSG_EXIT", "  Press EXIT to quit")]:
        asm.align(4)
        asm.label(name)
        asm.asciz(text)
    return asm.assemble(BASE)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    header[8] = 0x2C  # g3a file type
    with open(sys.argv[1], "wb") as f:
        f.write(header + build())


if __name__ == "__main__":
    main()
