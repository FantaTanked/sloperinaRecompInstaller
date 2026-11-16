"""(sloperinaRecomp) adds the mod loader to a copy of a recomp builder's tools folder (Hookshot, Longshot and forks with the
same layout): exefs mods as DLLs (loader/smod_api.h), loaded by the game itself.

    python -I patch_loader.py <tools folder copy>

Only the runtime (host/) and the Vulkan back end (gpu/vk/) change; nothing the game's translation depends on (nx2/, the HLE
functions' names) does, so a build with the loader compiles the runtime and takes the game's code from the builder's
cache. Each insertion is checked: a required one that can't be placed stops with a message, an optional one is left out
with a warning (and a stand-in, so the runtime still links).

  required  host/modloader.c    added: loads <game>/exefs_mods/<mod>/mod.dll, the bridge into the game
  required  host/nn_hid.c       the game's controller reads end in the loader (mods' frames)
  required  host/*.c           every HLE asks the loader first (main's imports a mod redirects in its GOT)
  required  host/hos_hostcall.c  the game's calls of a mod's code reach it (a64_call_miss)
  optional  host/hos_mem.c      the address regions, for mods' memory maps
  optional  host/nn_fs.c        the SD card (<game>/user/sdmc), for mods' files
  optional  gpu/vk/vk_menu.cpp  overlays (sloperinaGZ's panels) drawn over the picture

Run on a fresh copy each time (the mod manager does): the tools' own files are never changed.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MARK = "(sloperinaRecomp)"
results = []     # (required, file, what, ok, note)
stand_ins = []   # C appended to host/modloader.c for what couldn't be placed


def read(tools, rel):
    path = os.path.join(tools, *rel.split("/"))
    return open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else None


def write(tools, rel, text):
    with open(os.path.join(tools, *rel.split("/")), "w", encoding="utf-8", newline="") as f:
        f.write(text)


def nl(text):
    return "\r\n" if "\r\n" in text else "\n"


def block_end(text, at):
    """after the `}` at the start of a line that closes the function opening at `at` (top-level, column 0)"""
    m = re.compile(r"^\}[^\n]*\n", re.M).search(text, at)
    return m.end() if m else None


def note(required, rel, what, ok, why=""):
    results.append((required, rel, what, ok, why))


def inline(name, seen=None):
    """files/<name> with the headers of ours it includes put in its place: the builder recompiles every file of a folder
    when a header there changes (all of the runtime for a host/*.h, the whole back end for a gpu/vk/*.h), so the loader adds
    none, and only the files it changes compile"""
    seen = set() if seen is None else seen
    path = os.path.join(HERE, "files", *name.split("/"))
    text = open(path, encoding="utf-8").read().replace("#pragma once\n", "")
    folder = os.path.dirname(name)

    def sub(m):
        inc = (folder + "/" if folder else "") + m.group(1)
        if not os.path.isfile(os.path.join(HERE, "files", *inc.split("/"))):
            return m.group(0)
        if inc in seen:
            return ""
        seen.add(inc)
        return "/* ---- %s (sloperinaRecomp, inlined) ---- */\n%s\n" % (m.group(1), inline(inc, seen))
    return re.sub(r'^#include "([\w.]+)"[^\n]*$', sub, text, flags=re.M)


# ---- the runtime ----

DECL = ("extern volatile int hos_smod_on, hos_smod_calls;   /* %s host/modloader.c */%s"
        "int hos_modloader_redirect(Ctx *c, const char *name);%s"
        "int hos_modloader_call_in(Ctx *c, uint64_t target);%s"
        "void hos_modloader_after_npad(Ctx *c, uint64_t out, uint64_t id_ptr, int redirected);%s")
NPAD = re.compile(r"HLE\((_ZN2nn3hid13GetNpadStatesEPNS0_\w+?StateEiRKj)\)\s*\{\s*(\w+)\(c\);\s*\}")
HLE_START = re.compile(r"^(HLE\((\w+)\)|void imp_(\w+)\(Ctx \*c\))\s*\{", re.M)


def declare(s):
    """the loader's declarations, before the first HLE of the file (Ctx is known by then)"""
    e = nl(s)
    first = HLE_START.search(s) or NPAD.search(s)
    at = first.start() if first else 0
    return s[:at] + DECL % (MARK, e, e, e, e) + s[at:]


def nn_hid_c(tools):
    rel = "host/nn_hid.c"
    s = read(tools, rel)
    what = "the game's controller reads end in the loader (mods' frames)"
    if s is None:
        return note(True, rel, what, False, "no host/nn_hid.c")
    if "hos_modloader_after_npad(c, smod_out" in s:
        return note(True, rel, what, True)
    if not NPAD.search(s):
        return note(True, rel, what, False, "its GetNpadStates weren't found: mods would never run")
    # (a mod's redirect of the read honoured here too, and the frame after it either way)
    s = NPAD.sub(lambda m: "HLE(%s) { const uint64_t smod_out = ARG(0), smod_id = ARG(2); "
                           "const int smod_r = hos_smod_on && hos_modloader_redirect(c, \"%s\"); if (!smod_r) %s(c); "
                           "hos_modloader_after_npad(c, smod_out, smod_id, smod_r); }" % (m.group(1), m.group(1), m.group(2)), s)
    write(tools, rel, s)
    note(True, rel, what, True)


def hle_redirects(tools):
    """every function the runtime implements for the game (an HLE) asks the loader first: a mod may have redirected that
    import in main's GOT, as its hooks do on a Switch. One test of a flag while no mod does. The names stay the same, so
    the game's translation does too"""
    rel = "host/*.c"
    what = "main's imports a mod redirects (its GOT hooks) reach the mod"
    host = os.path.join(tools, "host")
    files = n = 0
    for f in sorted(os.listdir(host)):
        if not f.endswith(".c") or f == "modloader.c":
            continue
        s = read(tools, "host/" + f)
        if MARK in s and "hos_modloader_redirect" in s and f != "nn_hid.c":
            continue
        e = nl(s)
        count = [0]

        def wrap(m):
            name = m.group(2) or m.group(3)
            if NPAD.match(s, m.start()) or name.startswith("_ZN2nn3hid13GetNpadStates") and "smod_out" in s[m.end():m.end() + 200]:
                return m.group(0)
            count[0] += 1
            return m.group(0) + " if (__builtin_expect(hos_smod_on, 0) && hos_modloader_redirect(c, \"%s\")) return;" % name
        t = HLE_START.sub(wrap, s)
        if count[0] or (f == "nn_hid.c" and "smod_out" in s):
            write(tools, "host/" + f, declare(t))
            files += 1
            n += count[0]
    if not n:
        return note(True, rel, what, False, "the runtime's HLE functions weren't found")
    note(True, rel, what + " (%d functions in %d files)" % (n, files), True)


def call_miss(tools):
    rel = "host/hos_hostcall.c"
    s = read(tools, rel)
    what = "the game's calls of a mod's code (pointers it handed out) reach the mod"
    m = re.search(r"^int a64_call_miss\(Ctx \*(\w+), uint64_t (\w+)\) \{[^\n]*\n", s or "", re.M)
    if not m:
        return note(True, rel, what, False, "a64_call_miss wasn't found")
    e = nl(s)
    s = s[:m.end()] + "    if (hos_smod_calls && hos_modloader_call_in(%s, %s)) return 1;   /* %s */%s" % (m.group(1), m.group(2), MARK, e) + s[m.end():]
    s = s[:m.start()] + DECL % (MARK, e, e, e, e) + s[m.start():]
    write(tools, rel, s)
    note(True, rel, what, True)


def hos_mem_c(tools):
    rel = "host/hos_mem.c"
    s = read(tools, rel)
    what = "address regions in mods' memory maps"
    if s is not None and re.search(r"^int hos_region_find\(", s, re.M):
        return note(False, rel, what, True)
    m = re.search(r"^void hos_region_free\(uint64_t addr\) \{", s or "", re.M)
    if s is None or not m or not all(k in s for k in ("regions[", "nregions", "LOCK()", "UNLOCK()")):
        stand_ins.append("int hos_region_find(uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *next) "
                         "{ (void)addr, (void)base, (void)size; *next = ~0ULL; return 0; }")
        return note(False, rel, what, False, "the region list wasn't found: sloperinaGZ's watches on memory there show as unreadable")
    e = nl(s)
    fn = e.join([
        "/* %s the address region holding addr (1, *base, *size), or 0 and in *next the lowest one above it (~0: none) */" % MARK,
        "int hos_region_find(uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *next) {",
        "    int hit = 0;",
        "    *next = ~0ULL;",
        "    LOCK();",
        "    for (int i = 0; i < nregions; i++) {",
        "        if (addr >= regions[i].addr && addr - regions[i].addr < regions[i].size) {",
        "            *base = regions[i].addr, *size = regions[i].size, hit = 1;",
        "            break;",
        "        }",
        "        if (regions[i].addr > addr && regions[i].addr < *next) *next = regions[i].addr;",
        "    }",
        "    UNLOCK();",
        "    return hit;",
        "}", "", ""])
    s = s[:m.start()] + fn + s[m.start():]
    write(tools, rel, s)
    note(False, rel, what, True)


def nn_fs_c(tools):
    rel = "host/nn_fs.c"
    s = read(tools, rel)
    what = "the SD card (<game>/user/sdmc) for mods' files"
    if s is not None and re.search(r"^int hos_modloader_mount_sd\(", s, re.M):
        return note(False, rel, what, True)
    m = re.search(r"^static (int|void) user_mount\(const char \*name, const char \*sub\) \{", s or "", re.M)
    end = block_end(s, m.end()) if m else None
    if not end:
        stand_ins.append("int hos_modloader_mount_sd(const char *name) { (void)name; return -1; }")
        return note(False, rel, what, False, "its mounts weren't found: sloperinaGZ keeps no settings, saves or log")
    e = nl(s)
    body = "return user_mount(name, \"sdmc\") ? 0 : -1;" if m.group(1) == "int" else "user_mount(name, \"sdmc\"); return 0;"
    fn = e.join([
        "/* %s the SD card for mods (host/modloader.c): <user>/sdmc mounted as name, 0 on success. Not an HLE: the game's" % MARK,
        "   translation stays as it was */",
        "int hos_modloader_mount_sd(const char *name) { %s }" % body,
        "", ""])
    s = s[:end] + e + fn + s[end:]
    write(tools, rel, s)
    note(False, rel, what, True)


# ---- the Vulkan back end: overlays ----

def vk_menu_cpp(tools):
    rel = "gpu/vk/vk_menu.cpp"
    s = read(tools, rel)
    what = "sloperinaGZ's panels over the picture"
    if s is None:
        return note(False, rel, what, False, "no gpu/vk/vk_menu.cpp")
    if MARK in s:
        return note(False, rel, what, True)
    need = {"struct Bitmap": r"^struct Bitmap\b", "gdi_draw": r"^void gdi_draw\(Bitmap& out, uint32_t w, uint32_t h, void \(\*paint\)\(HDC, uint32_t, uint32_t, int\), int scale\)",
            "bmp_mu": r"\bstd::mutex bmp_mu\b", "scale_of": r"^int scale_of\(uint32_t h\)", "struct Upload": r"^struct Upload\b",
            "dev.swap_extent": r"\bdev\.swap_extent\b", "keyed bitmaps": r"\bbool keyed\b|\.keyed\b"}
    missing = [k for k, p in need.items() if not re.search(p, s, re.M)]
    cp = re.search(r"^void copy_over\(VkCommandBuffer cmd, VkImage dst, Bitmap& b, Upload& u, int x, int y, uint32_t w, uint32_t h\) \{", s, re.M)
    tick = re.search(r"^void menu_tick\(\) \{[^\n]*\n", s, re.M)
    rec = re.search(r"^void menu_record\(VkCommandBuffer (\w+), VkImage (\w+), int (\w+), uint32_t (\w+), uint32_t (\w+)\) \{[^\n]*\n", s, re.M)
    end = block_end(s, cp.end()) if cp else None
    if (missing or not end or not tick or not rec) and "ImGui::GetForegroundDrawList" in s:
        return vk_menu_imgui(tools, rel, what, s)
    if missing or not end or not tick or not rec:
        why = ", ".join(missing + [n for n, x in (("copy_over", end), ("menu_tick", tick), ("menu_record", rec)) if not x])
        return note(False, rel, what, False, "the menu's drawing has changed (%s): nothing is drawn, the menu still works blind" % why)
    e = nl(s)
    places = sorted([   # (from the end backwards, so the earlier places stay put)
        (rec.end(), "    sloperina_record(%s, %s, %s, %s, %s);   // %s gpu/vk/vk_sloperina.h%s" % (rec.groups() + (MARK, e))),
        (tick.end(), "    sloperina_tick();   // %s gpu/vk/vk_sloperina.h%s" % (MARK, e)),
        (end, "%s/* ---- %s sloperinaGZ's panels (gpu/vk/vk_sloperina.h, inlined) ---- */%s%s%s" % (
            e, MARK, e, inline("gpu/vk/vk_sloperina.h").replace("\n", e), e)),
    ], reverse=True)
    for at, text in places:
        s = s[:at] + text + s[at:]
    inc = re.search(r"^#include [<\"][^\n]*\n", s, re.M)
    s = s[:inc.start()] + "#ifndef _WIN32%s#include <dlfcn.h>   // %s vk_sloperina.h's dlsym%s#endif%s" % (e, MARK, e, e) + s[inc.start():]
    write(tools, rel, s)
    note(False, rel, what, True)


def vk_menu_imgui(tools, rel, what, s):
    """a menu drawn with Dear ImGui (Longshot): the panels drawn in its frame (gpu/vk/vk_sloperina_imgui.h)"""
    rec = re.search(r"^void menu_record\(VkCommandBuffer \w+, uint32_t \w+, uint32_t (\w+), uint32_t (\w+)\) \{[^\n]*\n", s, re.M)
    tick = re.search(r"^void menu_tick\(\) \{", s, re.M)
    end = block_end(s, rec.end()) if rec else None
    if not rec or not tick or not end or tick.start() > rec.start():
        return note(False, rel, what, False, "the menu's drawing has changed (menu_record, menu_tick): nothing is drawn, the menu still works blind")
    w, h = rec.groups()
    body = s[rec.end():end]
    e = nl(s)
    steps = [   # (inside menu_record: the panels count as something shown, and are drawn in its frame, under the menu)
        (r"^([ \t]*)const bool open = [^\n]*\n", lambda m: m.group(0) + "%sconst bool sg = sloperina_imgui_any();   // %s%s" % (m.group(1), MARK, e)),
        (r"if \(!start && !fps && !open\)", lambda m: "if (!start && !fps && !open && !sg)"),
        (r"^([ \t]*)if \(open\) draw_menu\(c\);", lambda m: "%sif (sg) sloperina_imgui_draw(%s, %s);   // %s%s%s" % (m.group(1), w, h, MARK, e, m.group(0))),
    ]
    for pat, fn in steps:
        found = list(re.finditer(pat, body, re.M))
        if len(found) != 1:
            return note(False, rel, what, False, "menu_record has changed (%s): nothing is drawn, the menu still works blind" % pat)
        body = body[:found[0].start()] + fn(found[0]) + body[found[0].end():]
    s = s[:rec.end()] + body + s[end:]
    s = s[:tick.start()] + "/* ---- %s sloperinaGZ's panels (gpu/vk/vk_sloperina_imgui.h, inlined) ---- */%s%s%s" % (
        MARK, e, inline("gpu/vk/vk_sloperina_imgui.h").replace("\n", e), e) + s[tick.start():]
    inc = re.search(r"^#include [<\"][^\n]*\n", s, re.M)
    s = s[:inc.start()] + "#ifndef _WIN32%s#include <dlfcn.h>   // %s vk_sloperina_common.h's dlsym%s#endif%s" % (e, MARK, e, e) + s[inc.start():]
    write(tools, rel, s)
    note(False, rel, what + " (Dear ImGui)", True)


def main(tools):
    if not os.path.isfile(os.path.join(tools, "nx2", "make_game.py")) or not os.path.isdir(os.path.join(tools, "host")):
        raise SystemExit("patch_loader: %s isn't a recomp builder's tools folder (nx2/make_game.py, host/)" % tools)
    # (an added file that defines an HLE would change the translation's key: the game would be translated again)
    for d, _, files in os.walk(os.path.join(HERE, "files")):
        for f in files:
            if f.endswith(".c") and re.search(r"^(HLE\(|void imp_\w+\(Ctx)", open(os.path.join(d, f), encoding="utf-8").read(), re.M):
                raise SystemExit("patch_loader: %s defines an HLE: the game would be translated again" % f)
    # (the one file added, its header inlined: a new host/*.h would recompile the whole runtime)
    with open(os.path.join(tools, "host", "modloader.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(inline("host/modloader.c"))
    note(True, "host/modloader.c", "added: loads <game>/exefs_mods/<mod>/mod.dll", True)
    nn_hid_c(tools)
    hle_redirects(tools)
    call_miss(tools)
    hos_mem_c(tools)
    nn_fs_c(tools)
    vk_menu_cpp(tools)
    if stand_ins:
        with open(os.path.join(tools, "host", "modloader.c"), "a", encoding="utf-8", newline="\n") as f:
            f.write("\n/* %s stand-ins for what patch_loader.py couldn't add to this runtime */\n" % MARK)
            f.write("\n".join(stand_ins) + "\n")
    failed = False
    for required, rel, what, ok, why in results:
        tag = "ok  " if ok else ("FAIL" if required else "skip")
        print("  %s %-22s %s%s" % (tag, rel, what, (": " + why) if why else ""))
        failed |= required and not ok
    if failed:
        print("patch_loader: this release's runtime is too different for the mod loader; nothing was built")
        raise SystemExit(1)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        raise SystemExit(2)
    main(os.path.abspath(sys.argv[1]))
