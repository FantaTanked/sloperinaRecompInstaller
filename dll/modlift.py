"""(sloperinaRecomp) Translates a stock Switch exefs mod (a subsdk module and its .ips for main) to C for a mod DLL, as it is:
nothing in the mod has to know about recomps. Run by the mod manager on the player's PC, with their builder's tools and
their game's exefs (which give the addresses the mod's imports bind to; nothing of the game ends up in the output).

    python -I modlift.py <tools folder> <game exefs folder> <module file> <out folder> [<.ips for main> ...]

What the DLL then does in the game, in place of what a Switch does (dll/smod_rt.c, with the recomp's mod loader):
- The module's bytes go to its guest address, after sdk (where rtld would put it here: no game address moves), its
  DT_INIT runs.
- .ips records: the bytes go into main's guest memory (so a mod's own checks of its patch pass), and each hook on a call
  of the game's (a B / BL at a site, to code in a cave) becomes a call of the cave code once a frame, in which the call
  it replaced is skipped (the translated game still makes it). A hook into the middle of a function can't be kept in a
  recomp (a translated function is entered only at its start): its records are left out, so the mod sees it missing.
- main's imports a mod redirects in main's GOT: the loader calls the mod's function in place of the runtime's (the
  translated main calls its imports directly), and the game's calls of the mod's functions reach the DLL.
"""
import json
import os
import re
import shutil
import struct
import sys
import tempfile

tools, exefs_src, module, out = (os.path.abspath(a) for a in sys.argv[1:5])
ips_files = [os.path.abspath(a) for a in sys.argv[5:]]
sys.path.insert(0, os.path.join(tools, "nx2"))
os.chdir(tools)
import kit  # noqa: E402,F401  (a64-kit on the path)
import importlib  # noqa: E402
import image as eden_image  # noqa: E402
import a64_lift  # noqa: E402
import a64_rtgen  # noqa: E402
import rtgen  # noqa: E402

for fix in ("nso2", "insn", "lift_fixes", "suyu_rt_fixes"):   # (the translator's fixes, as nx2/lift.py installs them)
    try:
        m = importlib.import_module(fix)
    except ImportError:
        print("modlift: this translator has no %s" % fix)
        continue
    if hasattr(m, "install"):
        m.install()
nso = eden_image.nso
MAIN_BASE = lambda: eden_image.nx_image.MAIN_BASE

# Instructions the game's code doesn't use, so the translator may not have them, but a mod does: C for them here.
import a64emit  # noqa: E402


def extra_insn(w):
    """C for an instruction the translator left out, or None"""
    # SCVTF / UCVTF (scalar, fixed-point, SIMD&FP register): 01 U 11111 0 immh immb 11100 1 Rn Rd
    if (w & 0xDF80FC00) == 0x5F00E400 and (w >> 19) & 0xF:
        u, immh_immb, rn, rd = (w >> 29) & 1, (w >> 16) & 0x7F, (w >> 5) & 31, w & 31
        if immh_immb >> 6:   # (64-bit)
            fb = 128 - immh_immb
            src = "(double)(uint64_t)c->vreg[%d][0]" % rn if u else "(double)(int64_t)c->vreg[%d][0]" % rn
            div = "18446744073709551616.0" if fb == 64 else "(double)(1ULL << %d)" % fb
            return ("{ double a64x_ = %s / %s; memcpy(&c->vreg[%d][0], &a64x_, 8); c->vreg[%d][1] = 0; }"
                    % (src, div, rd, rd))
        if (immh_immb >> 5) == 1:   # (32-bit)
            fb = 64 - immh_immb
            src = "(float)(uint32_t)c->vreg[%d][0]" % rn if u else "(float)(int32_t)(uint32_t)c->vreg[%d][0]" % rn
            return ("{ float a64x_ = %s / (float)(1ULL << %d); uint32_t a64u_; memcpy(&a64u_, &a64x_, 4); "
                    "c->vreg[%d][0] = a64u_; c->vreg[%d][1] = 0; }" % (src, fb, rd, rd))
    return None


_translate = a64emit.translate


def _translate_more(items):
    tx, fl = _translate(items)
    tx, fl = list(tx), list(fl)
    for i, (pc, w) in enumerate(items):
        if fl[i] & a64emit.UNHANDLED:
            t = extra_insn(w)
            if t:
                tx[i], fl[i] = t, fl[i] & ~a64emit.UNHANDLED
    return tx, fl


a64emit.translate = _translate_more


def read_ips(path):
    data = open(path, "rb").read()
    width, end = (4, b"EEOF") if data[:5] == b"IPS32" else (3, b"EOF")
    if data[:5] not in (b"IPS32", b"PATCH"):
        raise SystemExit("modlift: %s is not an IPS patch" % path)
    pos, records = 5, []
    while data[pos:pos + len(end)] != end:
        off = int.from_bytes(data[pos:pos + width], "big")
        size = int.from_bytes(data[pos + width:pos + width + 2], "big")
        pos += width + 2
        if size == 0:
            run = int.from_bytes(data[pos:pos + 2], "big")
            records.append((off - 0x100, data[pos + 2:pos + 3] * run))
            pos += 3
        else:
            records.append((off - 0x100, data[pos:pos + size]))
            pos += size
    return records


def branch(word, at):
    """(target, is_bl) of a B / BL at `at`, or None"""
    if (word & 0x7C000000) != 0x14000000:
        return None
    imm = word & 0x3FFFFFF
    return at + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm), bool(word & 0x80000000)


name = os.path.basename(module)
with tempfile.TemporaryDirectory() as tmp:
    exefs = os.path.join(tmp, "exefs")
    os.makedirs(exefs)
    for f in os.listdir(exefs_src):
        if f in ("main", "main.npdm", "rtld", "sdk") or (f.startswith("subsdk") and f != name):
            shutil.copyfile(os.path.join(exefs_src, f), os.path.join(exefs, f))
    shutil.copyfile(module, os.path.join(exefs, name))
    # (the module after sdk, as rtld would place it in the recomp's layout: before it, it would move sdk and every address
    # the game's translation names. The builder's own image.py loads the exefs in name order)
    load = nso.load_exefs
    nso.load_exefs = lambda d: ([m for m in load(d) if m.name != name] + [m for m in load(d) if m.name == name])
    img = eden_image.EdenImage(exefs)
    nso.load_exefs = load
order = [m.name for m in img.modules]
if order[-1] != name or order[-2] != "sdk":
    raise SystemExit("modlift: %s should follow sdk, the image has %s" % (name, order))
mod = img.modules[-1]
main = next(m for m in img.modules if m.name == "main")
lo, hi = mod.base, mod.base + eden_image.eden_size(mod)
print("image: %s" % ", ".join("%s@%#x+%#x" % (m.name, m.base, eden_image.eden_size(m)) for m in img.modules))

# ---- the .ips records for main: which can be kept ----
main_id = main.build_id.hex().rstrip("0").upper() if isinstance(main.build_id, (bytes, bytearray)) else str(main.build_id).rstrip("0").upper()
records = []
for p in ips_files:
    stem = os.path.splitext(os.path.basename(p))[0].upper().rstrip("0")
    if stem and (main_id.startswith(stem) or stem.startswith(main_id)):   # (the name: main's build id, 40 or 64 digits)
        records += read_ips(p)
        print("ips: %s (%d records)" % (os.path.basename(p), len(read_ips(p))))
    else:
        print("ips: %s is for another build of the game: left out" % os.path.basename(p))
word_at = lambda a: struct.unpack_from("<I", img.mem, a - MAIN_BASE())[0]
span = [(main.base + off, main.base + off + len(d)) for off, d in records]
record_of = lambda a: next((i for i, (s, e) in enumerate(span) if s <= a < e), None)
hooks, unkept = [], set()
for i, (off, d) in enumerate(records):
    if len(d) != 4:
        continue
    site = main.base + off
    b = branch(struct.unpack("<I", d)[0], site)
    if not b or record_of(b[0]) in (None, i):
        continue
    was = branch(word_at(site), site)   # (what the game had there)
    if was:
        hooks.append({"site": site, "cave": b[0], "replaced": was[0]})
    else:
        unkept.update((i, record_of(b[0])))
        print("ips: the hook at main+%#x goes into the middle of a game function: a recomp can't keep it (left out)" % off)
kept = [(off, d) for i, (off, d) in enumerate(records) if i not in unkept]
for off, d in kept:   # (the image the cave code is lifted from)
    a = main.base + off - MAIN_BASE()
    img.mem[a:a + len(d)] = d
print("ips: %d hook(s) on game calls kept, %d record(s) written, %d left out" % (len(hooks), len(kept), len(records) - len(kept)))

# (the kept records are code too: a cave is usually past main's .text, in its padding, where the image sees no code)
cave_spans = [(main.base + off, main.base + off + len(d)) for off, d in kept]
in_code = img.in_code
img.in_code = lambda a: in_code(a) or any(s <= a < e for s, e in cave_spans)

# ---- the module's functions ----
L = a64_lift.Lifter(img)
mine = {e for e in L.entries if lo <= e < hi}
# (no unwind table in a stock module: every instruction after an unconditional branch or return starts a function too. An
# extra entry is always correct in this translation: code reaching it tail-calls it)
text_lo, text_hi = mod.base + mod.segments[0][0], mod.base + mod.segments[0][0] + mod.segments[0][1]
in_text = lambda a: text_lo <= a < text_hi
sweep = adr = 0
for a in range(text_lo, text_hi - 4, 4):
    w = word_at(a)
    # (after a RET: the next function. After every branch would cut functions into pieces that each walk the code they share)
    if (w & 0xFFFFFC1F) == 0xD65F0000 and word_at(a + 4) and a + 4 not in mine:
        mine.add(a + 4)
        sweep += 1
    # code addresses the module builds (callbacks it hands out): ADR, and ADRP + ADD
    if (w & 0x9F000000) == 0x10000000:   # ADR
        t = a + ((((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3) ^ (1 << 20)) - (1 << 20)
        if in_text(t) and t % 4 == 0 and t not in mine:
            mine.add(t)
            adr += 1
    elif (w & 0x9F000000) == 0x90000000:   # ADRP, then an ADD of the same register
        page = (a & ~0xFFF) + (((((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3) ^ (1 << 20)) - (1 << 20) << 12)
        rd = w & 31
        for k in range(1, 25):   # (the compiler schedules them apart: an ADD from that register, until it's written again)
            n = word_at(a + 4 * k) if a + 4 * k < text_hi else 0
            if (n & 0xFFC00000) == 0x91000000 and (n >> 5) & 31 == rd:
                t = page + ((n >> 10) & 0xFFF)
                if in_text(t) and t % 4 == 0 and t not in mine:
                    mine.add(t)
                    adr += 1
                break
            if n & 31 == rd and (n & 0x1F000000) in (0x11000000, 0x12000000, 0x13000000, 0x0A000000, 0x0B000000, 0x18000000,
                                                       0x19000000, 0x10000000) or (n & 0x3B000000) == 0x39000000 and n & 31 == rd \
                    or (n & 0x7C000000) == 0x14000000 or (n & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000):
                break   # (written by another instruction, or the code went elsewhere)
# code addresses the module keeps in its data (its GOT, callback tables): its RELATIVE / ABS64 relocations, and any word of
# .rodata / .data that points into its code (a callback it hands to the game is often only there)
ptr = 0
for r in mod.relocs:
    t = mod.base + r.get("addend", 0) if r["type"] == "RELATIVE" else None
    if t is not None and in_text(t) and t % 4 == 0 and t not in mine:
        mine.add(t)
        ptr += 1
for seg in mod.segments[1:3]:
    s_lo, s_hi = mod.base + seg[0], mod.base + seg[0] + seg[1]
    for a in range(s_lo, s_hi - 7, 8):
        t = struct.unpack_from("<Q", img.mem, a - MAIN_BASE())[0]
        if in_text(t) and t % 4 == 0 and t not in mine and word_at(t):
            mine.add(t)
            ptr += 1
print("entries: %d after returns, %d code addresses built with ADR / ADRP+ADD, %d kept in its data" % (sweep, adr, ptr))
mine.add(mod.base + mod.dynamic["INIT"][0]) if "INIT" in mod.dynamic else None
mine |= {h["cave"] for h in hooks}
L.entries |= mine
for e in sorted(mine):
    body = L.walk(e)
    if body[0]:
        L.funcs[e] = body
    elif e not in {h["cave"] for h in hooks}:
        L.entries.discard(e)   # (no code there: padding after the last function)
for e in list(L.funcs):
    t = L.plt_target(e)
    if t is not None:
        L.plt[e] = t
L.find_setjmp_owners()
print("module %s at %#x-%#x: %d functions (%d cave), %d PLT stubs" % (name, lo, hi, len(L.funcs), len(hooks), len(L.plt)))

gen = os.path.join(out, "gen")
shutil.rmtree(gen, ignore_errors=True)
os.makedirs(gen)
files = L.write(gen, chunk_bytes=1 << 40)
a64_rtgen.write(gen)
rt = rtgen.write(os.path.join(out, "rt64"))
L.report()

# calls out of the DLL (the game's functions, the SDK's): f_<address> stubs into the loader (dll/smod_rt.c smod_out)
code = "".join(open(f, encoding="utf-8").read() for f in files)
defined = set(re.findall(r"^void (f_[0-9a-f]{8})\(Ctx[^;\n]*\{", code, re.M))   # (definitions, not the declarations)
used = set(re.findall(r"\b(f_[0-9a-f]{8})\b", code)) - defined
with open(os.path.join(gen, "smod_out.c"), "w", newline="\n") as f:
    f.write('/* Generated by dll/modlift.py: the game\'s functions the module calls directly, through the loader. */\n#include "a64.h"\n')
    f.write("void smod_out(Ctx *A64_NOESC c, uint64_t target);\n")
    f.write("".join("void %s(Ctx *A64_NOESC c) { smod_out(c, 0x%sULL); }\n" % (n, n[2:]) for n in sorted(used)))

# ---- smod_gen.c: the module's bytes, its init, the hooks, the .ips bytes, the imports by name, main's GOT ----
data = bytes(img.mem[lo - MAIN_BASE():hi - MAIN_BASE()])
size = len(data.rstrip(b"\0"))
imports = {}
for s in mod.symbols:
    if not s["defined"] and s["name"] and s["name"] in img.exports:
        m, es = img.exports[s["name"]]
        imports[m.base + es["value"]] = s["name"]
got = {}
for r in main.relocs:
    if r["type"] in ("JUMP_SLOT", "GLOB_DAT") and r.get("sym"):
        got.setdefault(main.base + r["offset"], r["sym"])
cstr = lambda s: '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')
with open(os.path.join(gen, "smod_gen.c"), "w", newline="\n") as f:
    f.write("/* Generated by dll/modlift.py from %s. Do not edit. */\n#include <stdint.h>\n" % os.path.basename(module))
    f.write("const uint64_t smod_lo = 0x%x, smod_hi = 0x%x;\nconst uint32_t smod_image_size = %d;\n" % (lo, hi, size))
    f.write("const uint8_t smod_image[] = {\n")
    for i in range(0, size, 40):
        f.write(",".join(str(b) for b in data[i:i + 40]) + ",\n")
    f.write("};\n")
    f.write("const uint64_t smod_init = 0x%x;\n" % (mod.base + mod.dynamic["INIT"][0] if "INIT" in mod.dynamic else 0))
    f.write("const uint32_t smod_hook_count = %d;\n" % len(hooks))
    f.write("const uint64_t smod_hook_cave[] = {%s0};\n" % "".join("0x%x," % h["cave"] for h in hooks))
    f.write("const uint64_t smod_hook_replaced[] = {%s0};\n" % "".join("0x%x," % h["replaced"] for h in hooks))
    blob = b"".join(struct.pack("<QI", main.base + off, len(d)) + d for off, d in kept)
    f.write("const uint32_t smod_patch_size = %d;\nconst uint8_t smod_patch[] = {%s0};\n" % (len(blob), ",".join(str(b) for b in blob) + ("," if blob else "")))
    f.write("const uint32_t smod_import_count = %d;\n" % len(imports))
    f.write("const uint64_t smod_import_addr[] = {%s0};\n" % "".join("0x%x," % a for a in sorted(imports)))
    f.write("const char *const smod_import_name[] = {%s0};\n" % "".join(cstr(imports[a]) + "," for a in sorted(imports)))
    f.write("const uint32_t smod_got_count = %d;\n" % len(got))
    f.write("const uint64_t smod_got_slot[] = {%s0};\n" % "".join("0x%x," % a for a in sorted(got)))
    f.write("const char *const smod_got_name[] = {%s0};\n" % "".join(cstr(got[a]) + "," for a in sorted(got)))
with open(os.path.join(out, "module.json"), "w") as f:
    json.dump({"module": name, "lo": lo, "hi": hi, "functions": len(L.funcs), "hooks": hooks,
               "records_kept": len(kept), "records_left_out": len(records) - len(kept), "imports": len(imports),
               "main_got": len(got), "external_calls": len(used)}, f, indent=1)
print("wrote %s: %d functions, module image %d of %d bytes, %d imports, %d main GOT slots, %d direct calls out" % (
    gen, len(L.funcs), size, hi - lo, len(imports), len(got), len(used)))
