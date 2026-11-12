#!/usr/bin/env python3
"""
gen_vectors.py -- ground-truth test vectors for arm64_fault_decode.

Two sources of instruction words, both produced by the Android NDK's own
toolchain rather than written by hand:

  1. A curated list of assembly lines (every addressing form and size the
     decoder claims, plus forms it must refuse), assembled by NDK clang.
  2. accessor_sample.c (the runtime's MEM8/16/32/MEMF/MEMD/... accessor
     shapes) compiled by NDK clang at several -O levels and -march settings;
     every load/store the compiler emits is harvested.

For each word the EXPECTED decode is derived from llvm-objdump's disassembly
text (mnemonic + operands), independently of the decoder under test. The
output, vectors_gen.h, is checked in so the native test builds without the
NDK; rerun this script to regenerate it.

    python3 gen_vectors.py [--ndk DIR] [--out vectors_gen.h]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Curated assembly. Lines prefixed "!" must be REFUSED by the decoder.
# ---------------------------------------------------------------------------
CURATED = r"""
// the four from the task statement
ldr w0, [x1]
str w0, [x1, #4]
ldrb w2, [x3]
ldp w0, w1, [x2]
// unsigned immediate, every size/sign/class
ldr x0, [x1, #32760]
ldr w5, [sp, #16380]
ldrh w6, [x7, #4094]
ldrb w8, [x9, #4095]
ldrsb w10, [x11, #1]
ldrsb x10, [x11, #2]
ldrsh w12, [x13, #2]
ldrsh x12, [x13, #4094]
ldrsw x14, [x15, #16380]
strb w16, [x17, #4095]
strh w18, [x19, #2]
str x20, [x21, #8]
str xzr, [x22]
str wzr, [x23, #4]
strb wzr, [x24]
ldr xzr, [x25]
ldr wzr, [x26, #4]
ldr b0, [x1, #4095]
ldr h1, [x2, #8190]
ldr s2, [x3, #16380]
ldr d3, [x4, #32760]
ldr q4, [x5, #65520]
str b5, [x6]
str h6, [x7, #2]
str s7, [x8, #4]
str d8, [x9, #8]
str q9, [x10, #16]
ldr s31, [sp]
str d31, [sp, #8]
// unscaled
ldur w0, [x1, #-4]
ldur x0, [x1, #255]
ldur x2, [x3, #-256]
ldurb w4, [x5, #-1]
ldurh w6, [x7, #1]
ldursb w8, [x9, #-3]
ldursb x8, [x9, #3]
ldursh w10, [x11, #-5]
ldursh x10, [x11, #5]
ldursw x12, [x13, #-7]
stur w14, [x15, #-8]
stur x16, [x17, #7]
sturb w18, [x19, #-9]
sturh w20, [x21, #9]
ldur s0, [x1, #-4]
ldur d1, [x2, #-8]
ldur q2, [x3, #-16]
ldur b3, [x4, #-1]
ldur h4, [x5, #-2]
stur s5, [x6, #-12]
stur d6, [x7, #3]
stur q7, [x8, #-1]
ldtr w0, [x1, #4]
sttr x2, [x3, #-8]
ldtrsb x4, [x5]
// pre/post index
ldr w0, [x1, #4]!
ldr x2, [x3], #8
ldr w4, [x5], #-4
str x6, [x7, #-16]!
str w8, [x9], #255
strb w10, [x11, #1]!
strh w12, [x13], #-2
ldrb w14, [x15], #1
ldrh w16, [x17, #-2]!
ldrsb x18, [x19], #1
ldrsh w20, [x21, #2]!
ldrsw x22, [x23], #4
ldr s0, [x1, #4]!
ldr d1, [x2], #8
ldr q2, [x3, #16]!
str s3, [x4], #-4
str d4, [sp, #-16]!
str q5, [sp], #32
str x30, [sp, #-16]!
ldr x30, [sp], #16
// register offset
ldr w0, [x1, x2]
ldr w0, [x1, x2, lsl #2]
ldr x0, [x1, x2, lsl #3]
ldr x0, [x1, x2]
ldr w3, [x4, w5, uxtw]
ldr w3, [x4, w5, uxtw #2]
ldr x6, [x7, w8, sxtw]
ldr x6, [x7, w8, sxtw #3]
ldr x9, [x10, x11, sxtx]
ldr x9, [x10, x11, sxtx #3]
ldrb w12, [x13, x14]
ldrb w12, [x13, x14, lsl #0]
ldrb w12, [x13, w14, uxtw]
ldrb w12, [x13, w14, sxtw #0]
ldrh w15, [x16, x17, lsl #1]
ldrh w15, [x16, w17, uxtw #1]
ldrsb w18, [x19, x20]
ldrsb x18, [x19, w20, sxtw]
ldrsh w21, [x22, x23, lsl #1]
ldrsh x21, [x22, w23, uxtw]
ldrsw x24, [x25, x26, lsl #2]
ldrsw x24, [x25, w26, sxtw #2]
str w0, [x1, x2, lsl #2]
str x3, [x4, w5, uxtw #3]
strb w6, [x7, w8, uxtw]
strh w9, [x10, x11, lsl #1]
str wzr, [x12, x13]
ldr w0, [sp, x1]
ldr w0, [x1, xzr]
ldr b0, [x1, x2]
ldr h1, [x2, x3, lsl #1]
ldr s2, [x3, w4, uxtw #2]
ldr d3, [x4, x5, lsl #3]
ldr q4, [x5, x6, lsl #4]
ldr q4, [x5, w6, sxtw]
str s5, [x6, w7, uxtw]
str d6, [x7, x8]
str q7, [x8, x9, lsl #4]
// pairs
ldp x0, x1, [x2]
ldp x0, x1, [x2, #504]
ldp x0, x1, [x2, #-512]
ldp w3, w4, [x5, #252]
ldp w3, w4, [x5, #-256]
stp w6, w7, [x8, #8]
stp x9, x10, [x11, #-8]
stp x29, x30, [sp, #-16]!
ldp x29, x30, [sp], #16
stp x19, x20, [sp, #32]
ldp w0, w1, [x2], #8
ldp w0, w1, [x2, #8]!
stp wzr, wzr, [x3]
stp xzr, xzr, [x4, #16]
ldpsw x5, x6, [x7]
ldpsw x5, x6, [x7, #-8]
ldpsw x5, x6, [x7], #8
ldpsw x5, x6, [x7, #4]!
ldp s0, s1, [x2]
ldp s0, s1, [x2, #-4]
stp s2, s3, [x4, #252]
ldp d4, d5, [x6, #8]
stp d8, d9, [sp, #-16]!
ldp d8, d9, [sp], #16
ldp q10, q11, [x12, #32]
stp q12, q13, [x14, #-1024]
ldp q0, q1, [x0]
stp q0, q1, [x0], #32
ldnp x0, x1, [x2, #16]
stnp w3, w4, [x5, #-4]
ldnp q6, q7, [x8]
stnp d9, d10, [x11, #8]
ldp x1, x0, [x0]
// load-acquire / store-release
ldar w0, [x1]
ldar x2, [x3]
ldarb w4, [x5]
ldarh w6, [x7]
stlr w8, [x9]
stlr x10, [x11]
stlrb w12, [x13]
stlrh w14, [x15]
stlr wzr, [sp]
ldlar w0, [x1]
stllr x2, [x3]
ldapr w0, [x1]
ldapr x2, [sp]
ldaprb w3, [x4]
ldaprh w5, [x6]
ldapur w0, [x1, #-4]
ldapur x2, [x3, #255]
ldapurb w4, [x5, #1]
ldapurh w6, [x7, #-2]
ldapursb w8, [x9]
ldapursb x8, [x9, #-1]
ldapursh x10, [x11, #2]
ldapursw x12, [x13, #-4]
stlur w14, [x15, #-8]
stlur x16, [x17, #8]
stlurb w18, [x19]
stlurh w20, [x21, #-2]
// exclusives
ldxr w0, [x1]
ldxr x2, [x3]
ldxrb w4, [x5]
ldxrh w6, [x7]
ldaxr w8, [x9]
ldaxr x10, [sp]
stxr w11, w12, [x13]
stxr w14, x15, [x16]
stxrb w17, w18, [x19]
stlxr w20, w21, [x22]
stlxrh w23, w24, [x25]
// LSE atomics
ldadd w0, w1, [x2]
ldadda x3, x4, [x5]
ldaddl w6, w7, [x8]
ldaddal w9, w10, [x11]
ldaddb w12, w13, [x14]
ldaddh w15, w16, [x17]
ldaddalb w0, w1, [sp]
stadd w0, [x1]
staddl x2, [x3]
staddb w4, [x5]
staddlh w6, [x7]
ldclr w0, w1, [x2]
ldclral x3, x4, [x5]
ldeor w6, w7, [x8]
ldeorh w9, w10, [x11]
ldset w12, w13, [x14]
ldsetal x15, x16, [x17]
stset w0, [x1]
stclr x2, [x3]
steor w4, [x5]
ldsmax w0, w1, [x2]
ldsmaxb w3, w4, [x5]
ldsmin x6, x7, [x8]
ldsminh w9, w10, [x11]
ldumax w12, w13, [x14]
ldumaxal x15, x16, [x17]
ldumin w18, w19, [x20]
lduminb w21, w22, [x23]
stsmax w0, [x1]
stumin x2, [x3]
swp w0, w1, [x2]
swpa x3, x4, [x5]
swpl w6, w7, [x8]
swpal x9, x10, [x11]
swpb w12, w13, [x14]
swpalh w15, w16, [x17]
cas w0, w1, [x2]
casa x3, x4, [x5]
casl w6, w7, [x8]
casal x9, x10, [x11]
casb w12, w13, [x14]
casalh w15, w16, [x17]
casal wzr, wzr, [sp]
// ---- must be refused ----
! prfm pldl1keep, [x0]
! prfm pstl2strm, [x0, #8]
! prfm pldl1keep, [x0, x1, lsl #3]
! prfum pldl1keep, [x0, #-1]
! ld1 {v0.4s}, [x0]
! st1 {v0.16b, v1.16b}, [x0], #32
! ld1 {v0.s}[1], [x0]
! ld1r {v0.4s}, [x0]
! ldxp x0, x1, [x2]
! stxp w3, x4, x5, [x6]
! ldaxp w0, w1, [x2]
! casp x0, x1, x2, x3, [x4]
! stgp x0, x1, [x2]
! stg x0, [x1]
! ldg x0, [x1]
! ldraa x0, [x1]
! ldrab x0, [x1, #8]!
! ld64b x0, [x1]
! ldr x0, 8
! ldr w0, 8
! ldr q0, 8
! ldrsw x0, 8
! prfm pldl1keep, 8
! add x0, x1, x2
! mov x0, #1
! b .
! bl .
! ret
! nop
! dmb ish
! movk x0, #1, lsl #16
! adrp x0, 0
! fadd s0, s1, s2
! ld1w {z0.s}, p0/z, [x0]
! .inst 0xF8408400
! .inst 0xF8008400
! .inst 0xB8408C21
! .inst 0x29400000
! .inst 0x28C10400
! .inst 0xA9C00400
! .inst 0x08017C41
! .inst 0x88007C20
! .inst 0x00000000
! .inst 0xFFFFFFFF
! .inst 0x7DC00000
! .inst 0x7CE06820
! .inst 0xBC9FF020
! .inst 0xB8A0C020
! .inst 0xB8FFC020
! .inst 0xB87FC020
"""
# The .inst words above, in order (the assembler rejects these spellings):
#   ldr x0, [x0], #8      str x0, [x0], #8      ldr w1, [x1, #8]!
#     -- writeback with base == data register: CONSTRAINED UNPREDICTABLE
#   ldp w0, w0, [x0]      -- load pair with Rt == Rt2
#   ldp w0, w1, [x0], #8  ldp x0, x1, [x0, #0]!  -- pair writeback, base == Rt
#   stxrb w1, w1, [x2]    stxr w0, w0, [x1]      -- status reg == data reg
#   udf #0, and an all-ones word.
#   0x7DC00000 / 0x7CE06820 / 0xBC9FF020: SIMD&FP load/store with opc<1> set
#     and size != 0 (only the B size has the 128-bit Q form) -- unallocated.
#   0xB8A0C020 / 0xB8FFC020 / 0xB87FC020: the LDAPR slot with Rs != 31, with
#     the release bit set, and with acquire clear -- unallocated.

FIELDS = ("kind size count is_load is_write is_fp is_signed reg_bits rt rt2 rn "
          "rm rs ext shift writeback post_index acquire release exclusive "
          "atomic_op imm").split()

KIND = dict(NONE=0, LDST=1, PAIR=2, ORDERED=3, EXCL=4, ATOMIC=5, CAS=6)
EXT = dict(none=0, uxtw=1, lsl=2, sxtw=3, sxtx=4)
ATOP = dict(add=0, clr=1, eor=2, set=3, smax=4, smin=5, umax=6, umin=7,
            swp=8, ldapr=9)
NONE = 0xFF


def reg(tok):
    """'w3' -> ('w', 3); 'sp'/'wsp' -> ('sp', 31); 'xzr' -> ('x', 31)."""
    tok = tok.strip()
    if tok in ("sp", "wsp"):
        return ("sp", 31)
    m = re.fullmatch(r"([wxbhsdq])(\d+|zr)", tok)
    if not m:
        raise ValueError("register? %r" % tok)
    return (m.group(1), 31 if m.group(2) == "zr" else int(m.group(2)))


def imm(tok):
    tok = tok.strip()
    assert tok.startswith("#"), tok
    return int(tok[1:], 0)


FPSIZE = dict(b=1, h=2, s=4, d=8, q=16)


def parse_mem(ops):
    """'[x1, x2, lsl #2]!' etc. -> dict(rn, rm, ext, shift, imm, wb, post)."""
    m = re.fullmatch(r"\[([^\]]*)\](!?)(?:,\s*(#-?(?:0x)?[0-9a-fA-F]+))?", ops.strip())
    if not m:
        raise ValueError("mem? %r" % ops)
    inner = [t.strip() for t in m.group(1).split(",")]
    d = dict(rn=reg(inner[0])[1], rm=NONE, ext=0, shift=0, imm=0, wb=0, post=0)
    if len(inner) >= 2 and not inner[1].startswith("#"):
        d["rm"] = reg(inner[1])[1]
        rmkind = reg(inner[1])[0]
        if len(inner) >= 3:
            ex = inner[2].split()
            d["ext"] = EXT[ex[0]]
            d["shift"] = int(ex[1][1:], 0) if len(ex) > 1 else 0
            d["explicit_amount"] = len(ex) > 1
        else:
            d["ext"] = EXT["lsl"] if rmkind == "x" else EXT["uxtw"]
    elif len(inner) >= 2:
        d["imm"] = imm(inner[1])
    if m.group(2):
        d["wb"] = 1
    if m.group(3):
        d["wb"] = 1
        d["post"] = 1
        d["imm"] = imm(m.group(3))
    return d


def split_ops(s):
    """Split operands at top-level commas (not inside [])."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    # rejoin "[...]!" / "[...], #imm" (post-index)
    res = []
    for t in out:
        if res and res[-1].startswith("[") and t.startswith("#") and "]" in res[-1]:
            res[-1] = res[-1] + ", " + t
        else:
            res.append(t)
    return res


def expect(mn, opstr):
    """Expected a64_access for one disassembled instruction, or None if the
    decoder must refuse it."""
    e = dict.fromkeys(FIELDS, 0)
    e.update(rt2=NONE, rm=NONE, rs=NONE, count=1)
    ops = split_ops(opstr)
    mem = [o for o in ops if o.startswith("[")]
    if not mem:
        return None
    m = parse_mem(mem[0])
    regs = [reg(o) for o in ops if not o.startswith("[")]
    e["rn"] = m["rn"]

    def gpr_load_bits(r):
        return 64 if r[0] == "x" else 32

    # ---- pairs ----
    pm = re.fullmatch(r"(ld|st)(n?)p(sw)?", mn)
    if pm:
        e["kind"] = KIND["PAIR"]
        e["count"] = 2
        load = pm.group(1) == "ld"
        e["is_load"], e["is_write"] = int(load), int(not load)
        e["rt"], e["rt2"] = regs[0][1], regs[1][1]
        k = regs[0][0]
        if pm.group(3):
            e.update(size=4, is_signed=1, reg_bits=64)
        elif k in "wx":
            e["size"] = 4 if k == "w" else 8
            e["reg_bits"] = (32 if k == "w" else 64) if load else 0
        else:
            e.update(size=FPSIZE[k], is_fp=1)
        e["imm"] = m["imm"]
        e["writeback"], e["post_index"] = m["wb"], m["post"]
        return e

    # ---- LSE atomics / swp / cas ----
    am = re.fullmatch(r"(ld|st)(add|clr|eor|set|smax|smin|umax|umin)(a?)(l?)([bh]?)", mn)
    sm = re.fullmatch(r"swp(a?)(l?)([bh]?)", mn)
    cm = re.fullmatch(r"cas(a?)(l?)([bh]?)", mn)
    if am or sm or cm:
        g = am or sm or cm
        sfx = g.group(g.lastindex)
        if am:
            e["kind"] = KIND["ATOMIC"]
            e["atomic_op"] = ATOP[am.group(2)]
            acq, rel = am.group(3), am.group(4)
        elif sm:
            e["kind"] = KIND["ATOMIC"]
            e["atomic_op"] = ATOP["swp"]
            acq, rel = sm.group(1), sm.group(2)
        else:
            e["kind"] = KIND["CAS"]
            acq, rel = cm.group(1), cm.group(2)
        e["acquire"], e["release"] = int(bool(acq)), int(bool(rel))
        e["is_load"] = e["is_write"] = 1
        e["rs"] = regs[0][1]
        e["rt"] = regs[1][1] if len(regs) > 1 else 31     # st* alias
        e["size"] = {"b": 1, "h": 2}.get(sfx) or (8 if regs[0][0] == "x" else 4)
        e["reg_bits"] = 64 if e["size"] == 8 else 32
        return e

    # ---- LDAPR (RCpc, in the atomics group) ----
    lm = re.fullmatch(r"ldapr([bh]?)", mn)
    if lm:
        e.update(kind=KIND["ATOMIC"], atomic_op=ATOP["ldapr"], acquire=1,
                 is_load=1, rt=regs[0][1])
        e["size"] = {"b": 1, "h": 2}.get(lm.group(1)) or (8 if regs[0][0] == "x" else 4)
        e["reg_bits"] = 64 if e["size"] == 8 else 32
        return e

    # ---- exclusives ----
    xm = re.fullmatch(r"(ld|st)(a|l)?xr([bh]?)", mn)
    if xm:
        e.update(kind=KIND["EXCL"], exclusive=1)
        load = xm.group(1) == "ld"
        if load:
            e.update(is_load=1, rt=regs[0][1], acquire=int(xm.group(2) == "a"))
            sz = {"b": 1, "h": 2}.get(xm.group(3)) or (8 if regs[0][0] == "x" else 4)
            e["size"] = sz
            e["reg_bits"] = 64 if sz == 8 else 32
        else:
            e.update(is_write=1, rs=regs[0][1], rt=regs[1][1],
                     release=int(xm.group(2) == "l"))
            e["size"] = {"b": 1, "h": 2}.get(xm.group(3)) or (8 if regs[1][0] == "x" else 4)
        return e

    # ---- LDAR/STLR/LDLAR/STLLR ----
    om = re.fullmatch(r"(ldar|ldlar|stlr|stllr)([bh]?)", mn)
    if om:
        e.update(kind=KIND["ORDERED"], rt=regs[0][1])
        sz = {"b": 1, "h": 2}.get(om.group(2)) or (8 if regs[0][0] == "x" else 4)
        e["size"] = sz
        if om.group(1).startswith("ld"):
            e.update(is_load=1, acquire=1, reg_bits=64 if sz == 8 else 32)
        else:
            e.update(is_write=1, release=1)
        return e

    # ---- single register: LDR/STR/LDUR/STUR/LDTR/STTR/LDAPUR/STLUR ----
    sm2 = re.fullmatch(r"(ld|st)(r|ur|tr|apur|lur)(s?)([bhw]?)", mn)
    if not sm2:
        raise ValueError("mnemonic? %s %s" % (mn, opstr))
    load = sm2.group(1) == "ld"
    form = sm2.group(2)
    signed = bool(sm2.group(3))
    width = sm2.group(4)
    r0 = regs[0]
    e["rt"] = r0[1]
    e["kind"] = KIND["ORDERED"] if form in ("apur", "lur") else KIND["LDST"]
    if form == "apur":
        e["acquire"] = 1
    if form == "lur":
        e["release"] = 1
    if r0[0] in FPSIZE:
        e.update(is_fp=1, size=FPSIZE[r0[0]])
    else:
        e["size"] = {"b": 1, "h": 2, "w": 4}.get(width) or (8 if r0[0] == "x" else 4)
        if load:
            e["reg_bits"] = gpr_load_bits(r0)
            e["is_signed"] = int(signed)
    e["is_load"], e["is_write"] = int(load), int(not load)
    e["rm"], e["ext"] = m["rm"], m["ext"]
    if m["rm"] != NONE:
        # objdump prints the amount it encodes; S=1 with a byte access shows
        # "lsl #0" / "uxtw #0", which the decoder reports as shift 0 anyway.
        e["shift"] = m["shift"]
    e["imm"] = m["imm"]
    e["writeback"], e["post_index"] = m["wb"], m["post"]
    return e


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
        raise SystemExit(1)
    return r.stdout


LINE = re.compile(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(\S+)\s*(.*?)\s*(?://.*)?$")


def disasm(objdump, obj):
    out = []
    for ln in run([objdump, "-d", "--no-print-imm-hex", obj]).splitlines():
        m = LINE.match(ln)
        if m:
            out.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return out


def is_ldst_mnemonic(mn):
    return bool(re.match(r"^(ld|st|cas|swp|prf)", mn))


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME",
                                                    "/root/xtool/android-ndk-r27c"))
    ap.add_argument("--out", default=os.path.join(HERE, "vectors_gen.h"))
    args = ap.parse_args()
    bin_ = os.path.join(args.ndk, "toolchains/llvm/prebuilt/linux-x86_64/bin")
    clang = os.path.join(bin_, "clang")
    objdump = os.path.join(bin_, "llvm-objdump")
    target = "--target=aarch64-linux-android29"
    march = "-march=armv8.7-a+memtag+sve+ls64+rcpc+pauth+lse+rcpc3"

    vectors = []      # (word, text, expected-or-None, origin)
    with tempfile.TemporaryDirectory() as td:
        # ---- curated ----
        lines, refuse = [], []
        for ln in CURATED.splitlines():
            ln = ln.strip()
            if not ln or ln.startswith("//"):
                continue
            neg = ln.startswith("!")
            lines.append(ln.lstrip("! ").strip())
            refuse.append(neg)
        src = os.path.join(td, "curated.S")
        with open(src, "w") as f:
            f.write("\n".join(lines) + "\n")
        obj = os.path.join(td, "curated.o")
        run([clang, target, march, "-c", src, "-o", obj])
        dis = disasm(objdump, obj)
        if len(dis) != len(lines):
            sys.stderr.write("curated: %d lines assembled to %d words\n"
                             % (len(lines), len(dis)))
            raise SystemExit(1)
        for (word, mn, ops), src_line, neg in zip(dis, lines, refuse):
            text = (mn + " " + ops).strip()
            exp = None if neg else expect(mn, ops)
            if not neg and exp is None:
                raise SystemExit("no expectation for %s" % text)
            vectors.append((word, text + "    ; " + src_line, exp, "curated"))

        # ---- compiled accessor shapes ----
        sample = os.path.join(HERE, "accessor_sample.c")
        seen = set()
        configs = []
        for opt in ("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"):
            for extra in ([], ["-march=armv8.2-a"], ["-march=armv8.2-a+rcpc"],
                          ["-mno-outline-atomics"], ["-mcpu=cortex-a78c"]):
                configs.append([opt] + extra)
        harvested = 0
        for cfg in configs:
            obj = os.path.join(td, "s.o")
            run([os.path.join(bin_, "aarch64-linux-android29-clang"), "-c",
                 sample, "-o", obj] + cfg)
            for word, mn, ops in disasm(objdump, obj):
                if not is_ldst_mnemonic(mn):
                    continue
                harvested += 1
                if word in seen:
                    continue
                seen.add(word)
                exp = expect(mn, ops)
                if exp is None:
                    raise SystemExit("compiler emitted %s %s with no expectation"
                                     % (mn, ops))
                vectors.append((word, "%s %s    ; accessor_sample %s"
                                % (mn, ops, " ".join(cfg)), exp, "compiled"))

    with open(args.out, "w") as f:
        f.write("/* Generated by gen_vectors.py from NDK clang + llvm-objdump."
                " Do not edit. */\n")
        f.write("/* %d vectors; %d load/store instructions harvested from "
                "accessor_sample.c across %d compiler configurations. */\n"
                % (len(vectors), harvested, len(configs)))
        f.write("#define VEC_HARVESTED %d\n" % harvested)
        f.write("#define VEC_CONFIGS %d\n" % len(configs))
        f.write("static const struct vec {\n    uint32_t insn;\n"
                "    const char *text;\n    int origin_compiled;\n"
                "    int accept;\n    struct vexp { %s; } e;\n} k_vectors[] = {\n"
                % "; ".join("long long %s" % fl for fl in FIELDS))
        for word, text, exp, origin in vectors:
            if exp is None:
                body = "{0}"
            else:
                body = "{" + ", ".join(str(exp[fl]) for fl in FIELDS) + "}"
            f.write("    { 0x%08Xu, %s, %d, %d, %s },\n"
                    % (word, c_str(text), origin == "compiled",
                       exp is not None, body))
        f.write("};\n")
    print("wrote %s: %d vectors (%d harvested from %d configs)"
          % (args.out, len(vectors), harvested, len(configs)))


if __name__ == "__main__":
    main()
