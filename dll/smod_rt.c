/* (sloperinaRecomp) a mod DLL's runtime: a stock Switch exefs mod (a subsdk module and its .ips), translated to C on its own
   (dll/modlift.py, gen/), the translator's runtime (rt64/a64_rt.c) and this file, which does for the module what a Switch
   would, with the recomp's mod loader (loader/files/host/smod_api.h).

   - init: the module's bytes to its guest address, the kept .ips records into main's memory, then its DT_INIT.
   - frame: each .ips hook on a game call: the cave code it branches to runs, and the call it replaced is skipped (the
     translated game still makes it where it always did).
   - main's GOT: the slots' values before init are kept; a slot the module changes is a redirect the loader honours.
   - The module's calls out (the game's functions, the SDK's): to the game through the loader, on the calling thread's
     context, except a few imports answered here: the controller reads (the loader's copy of the game's controller, stick
     directions as buttons 16-23 as the Switch reports them), the SD card's mount, plain C ones on guest memory.
   - svcQueryMemory for the module's own range; the rest of the map from the loader. Other system calls: the game's. */
#include "a64.h"
#include "smod_api.h"
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include("smod_version.h")
#include "smod_version.h"   /* (written by the mod manager: SMOD_NAME, SMOD_VERSION_TEXT) */
#endif
#ifndef SMOD_NAME
#define SMOD_NAME "mod"
#endif
#ifndef SMOD_VERSION_TEXT
#define SMOD_VERSION_TEXT "dev"
#endif

extern const uint64_t smod_lo, smod_hi, smod_init;
extern const uint32_t smod_image_size, smod_patch_size;
extern const uint8_t smod_image[], smod_patch[];
extern const uint32_t smod_hook_count, smod_import_count, smod_got_count;
extern const uint64_t smod_hook_cave[], smod_hook_replaced[], smod_import_addr[], smod_got_slot[];
extern const char *const smod_import_name[];
extern const char *const smod_got_name[];

static const SmodHost *host;
static int started, broken;
static __thread uint64_t skip_call;   /* (during a hook's cave code: the game call it replaced) */

/* Written guest memory is marked, as the game's translated code marks it: the runtime's renderer sends only marked pages
   to the GPU again (a mod drawing through the game's NVN writes its textures and buffers in guest memory). build_dll.ps1
   builds the translated code with A64_WRITE_TRACK and a64_dirty as (*smod_dirty): the runtime's own marks (host->dirty),
   or, from a loader without them, a table of our own nothing reads. */
#if !defined(A64_WRITE_TRACK)
#error "build_dll.ps1 builds the mod with A64_WRITE_TRACK"
#endif
static void mark(uint64_t addr, uint64_t n) {
    if (!n) return;
    for (uint64_t p = addr >> 12, last = (addr + n - 1) >> 12; p <= last; p++)
        __atomic_store_n(&a64_dirty[p & (A64_DIRTY_PAGES - 1)], 1, __ATOMIC_RELEASE);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (host && host->log) host->log(line);
}

/* ---- registers between the DLL's context and the loader's ---- */
static void to_regs(const Ctx *c, SmodRegs *r) {
    memcpy(r->x, c->x, sizeof r->x);
    memcpy(r->v, c->vreg, sizeof r->v);
    r->n = c->n, r->z = c->z, r->c = c->c, r->v_flag = c->v;
    r->fpcr = c->fpcr, r->fpsr = c->fpsr, r->tpidr_el0 = c->tpidr_el0, r->tpidrro_el0 = c->tpidrro_el0;
}
static void from_regs(Ctx *c, const SmodRegs *r) {
    memcpy(c->x, r->x, sizeof r->x);
    memcpy(c->vreg, r->v, sizeof r->v);
    c->n = r->n, c->z = r->z, c->c = r->c, c->v = r->v_flag;
    c->fpcr = r->fpcr, c->fpsr = r->fpsr, c->tpidr_el0 = r->tpidr_el0, c->tpidrro_el0 = r->tpidrro_el0;
}

static void game_call(Ctx *c, uint64_t target) {
    SmodRegs r;
    to_regs(c, &r);
    if (host->call(&r, target)) {
        say("%s: no game function at %#llx (lr %#llx): the mod stops", SMOD_NAME, (unsigned long long)target,
            (unsigned long long)c->x[30]);
        c->halted = 1;
        broken = 1;
        return;
    }
    from_regs(c, &r);
}

/* ---- the runtime's hooks (rt64/a64_rt.c's weak ones) ---- */
#define SVC_QUERY_MEMORY 0x6
void a64_svc(Ctx *c, uint32_t imm) {
    if (imm == SVC_QUERY_MEMORY) {   /* (MemoryInfo *out, PageInfo *page, address) */
        const uint64_t out = c->x[0], addr = c->x[2];
        SmodMemInfo m = {0};
        if (addr >= smod_lo && addr < smod_hi) {
            m.base = smod_lo, m.size = smod_hi - smod_lo, m.state = 0x03, m.perm = 7;   /* (as the game's image) */
        } else {
            host->query(addr, &m);
            if (m.state == 0 && m.base < smod_lo && m.base + m.size > smod_lo) m.size = smod_lo - m.base;   /* (free up to us) */
        }
        uint8_t *o = A64_PTR(out);
        memset(o, 0, 0x28);
        memcpy(o, &m.base, 8);
        memcpy(o + 8, &m.size, 8);
        memcpy(o + 16, &m.state, 4);
        memcpy(o + 20, &m.attr, 4);
        memcpy(o + 24, &m.perm, 4);
        mark(out, 0x28);
        c->x[0] = 0, c->x[1] = 0;
        return;
    }
    SmodRegs r;
    to_regs(c, &r);
    host->svc(&r, imm);
    from_regs(c, &r);
}

void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    say("%s: %s at %#llx (insn %08x, lr %#llx): the mod stops", SMOD_NAME, why, (unsigned long long)pc, insn,
        (unsigned long long)c->x[30]);
    broken = 1;
}

uint64_t a64_cntpct_host(void) {
    SmodRegs r = {0};
    host->svc(&r, 0x1E);   /* (svcGetSystemTick) */
    return r.x[0];
}

void a64_poll(Ctx *c) { (void)c; }
void a64_poll_cur(void) {}

static float vf(const Ctx *c, int i) { float f; memcpy(&f, &c->vreg[i][0], 4); return f; }
static void set_vf(Ctx *c, float f) { uint32_t u; memcpy(&u, &f, 4); c->vreg[0][0] = u, c->vreg[0][1] = 0; }

/* ---- the controller: nn::hid::detail::GetNpadStates(int *out_count, State *out, int count, unsigned int const& npadId) ---- */
#define STATE_SIZE 0x28
static void npad_detail(Ctx *c) {
    const uint64_t pcount = c->x[0], out = c->x[1];
    const int count = (int)c->x[2] > 0 ? 1 : 0;
    const int connected = *(const uint32_t *)A64_PTR(c->x[3]) == 0;   /* (No.1, the one the runtime connects) */
    if (count) {
        SmodPad p = {0};
        host->pad(&p, 1);
        uint64_t b = p.buttons;
        const int32_t past = 0x4000;   /* (half a stick's travel) */
        for (int s = 0; s < 2; s++) {
            const int32_t x = p.stick[2 * s], y = p.stick[2 * s + 1];
            const int at = 16 + 4 * s;   /* left, up, right, down */
            if (x < -past) b |= 1ULL << at;
            if (y > past) b |= 1ULL << (at + 1);
            if (x > past) b |= 1ULL << (at + 2);
            if (y < -past) b |= 1ULL << (at + 3);
        }
        uint8_t *st = A64_PTR(out);
        memset(st, 0, STATE_SIZE);
        const uint64_t ms = (uint64_t)(p.when_ns / 1000000);
        memcpy(st, &ms, 8);
        if (connected && p.connected) {
            memcpy(st + 8, &b, 8);
            memcpy(st + 16, p.stick, 16);
            const uint32_t attr = 1;
            memcpy(st + 32, &attr, 4);
        }
        mark(out, STATE_SIZE);
    }
    if (pcount) *(uint32_t *)A64_PTR(pcount) = (uint32_t)count, mark(pcount, 4);
}

/* an import the module calls: answered here (1) or not (0: the game's) */
static int import_here(Ctx *c, const char *name) {
    if (!strcmp(name, "memcpy") || !strcmp(name, "memmove")) {
        if (c->x[2]) memmove(A64_PTR(c->x[0]), A64_PTR(c->x[1]), (size_t)c->x[2]), mark(c->x[0], c->x[2]);
        return 1;
    }
    if (!strcmp(name, "memset")) {
        if (c->x[2]) memset(A64_PTR(c->x[0]), (int)c->x[1], (size_t)c->x[2]), mark(c->x[0], c->x[2]);
        return 1;
    }
    if (!strcmp(name, "strlen")) { c->x[0] = strlen((const char *)A64_PTR(c->x[0])); return 1; }
    if (!strcmp(name, "sinf")) { set_vf(c, sinf(vf(c, 0))); return 1; }
    if (!strcmp(name, "cosf")) { set_vf(c, cosf(vf(c, 0))); return 1; }
    if (!strcmp(name, "tanf")) { set_vf(c, tanf(vf(c, 0))); return 1; }
    if (!strcmp(name, "atan2f")) { set_vf(c, atan2f(vf(c, 0), vf(c, 1))); return 1; }
    if (!strcmp(name, "hypotf")) { set_vf(c, hypotf(vf(c, 0), vf(c, 1))); return 1; }
    if (!strncmp(name, "_ZN2nn2fs11MountSdCard", 22) || !strncmp(name, "_ZN2nn2fs19MountSdCardForDebug", 30)) {
        const int r = host->mount_sd((const char *)A64_PTR(c->x[0]));
        say("%s: SD card as %s: %s", SMOD_NAME, (const char *)A64_PTR(c->x[0]), r ? "failed" : "mounted");
        c->x[0] = r ? 0x2EE202 : 0;   /* (fs's "not mounted" on failure) */
        return 1;
    }
    if (!strncmp(name, "_ZN2nn3hid6detail13GetNpadStates", 32)) {
        npad_detail(c);
        return 1;
    }
    return 0;
}

static const char *import_name(uint64_t target) {
    uint32_t lo = 0, hi = smod_import_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (smod_import_addr[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    return lo < smod_import_count && smod_import_addr[lo] == target ? smod_import_name[lo] : NULL;
}

/* main's imports by what their GOT slots held before the module ran: the originals a hook calls on to */
static uint64_t *got_before;
static const char *original_name(uint64_t target) {
    for (uint32_t i = 0; got_before && i < smod_got_count; i++)
        if (got_before[i] == target) return smod_got_name[i];
    return NULL;
}

/* nn::hid::GetNpadStates(State *out, int count, npadId): the Switch's hid reports the sticks' directions as buttons too
   (16-23: left stick left, up, right, down, then the right stick's); a recomp's runtime may not. Added to the states the
   game's function gave (count in x0), as a Switch would have them: mods' combos use them (sloperinaGZ: R + right stick down) */
#define NPAD_STATE 0x28
static void stick_buttons(uint64_t out, int n) {
    for (int k = 0; k < n && k < 16; k++) {
        uint8_t *st = A64_PTR(out + (uint64_t)k * NPAD_STATE);
        uint64_t b;
        int32_t s[4];
        memcpy(&b, st + 8, 8);
        memcpy(s, st + 16, 16);
        const int32_t past = 0x4000;   /* (half a stick's travel) */
        for (int i = 0; i < 2; i++) {
            const int at = 16 + 4 * i;
            if (s[2 * i] < -past) b |= 1ULL << at;
            if (s[2 * i + 1] > past) b |= 1ULL << (at + 1);
            if (s[2 * i] > past) b |= 1ULL << (at + 2);
            if (s[2 * i + 1] < -past) b |= 1ULL << (at + 3);
        }
        memcpy(st + 8, &b, 8);
    }
    if (n > 0) mark(out, (uint64_t)(n < 16 ? n : 16) * NPAD_STATE);
}

/* a call or jump out of the module: an import, a hook's replaced call, or the game's code */
void smod_out(Ctx *c, uint64_t target) {
    if (broken) return;
    if (target && target == skip_call) { c->x[0] = 0; return; }   /* (the game made this call itself) */
    const char *name = import_name(target);
    if (name && import_here(c, name)) return;
    if (!name) name = original_name(target);
    const uint64_t out = c->x[0];
    game_call(c, target);
    if (name && !strncmp(name, "_ZN2nn3hid13GetNpadStates", 25) && !broken) stick_buttons(out, (int)c->x[0]);
}
int a64_call_miss(Ctx *c, uint64_t target) { smod_out(c, target); return 1; }

/* ---- running the module's code ---- */
static int run(SmodRegs *r, uint64_t fn) {
    A64Fn f = a64_lookup(fn);
    if (!f) return -1;
    Ctx c;
    memset(&c, 0, sizeof c);
    from_regs(&c, r);
    f(&c);
    to_regs(&c, r);
    return 0;
}

/* a call of the module's code from the game's (in the game's own frame: its stack, its x30) */
static int call_in(SmodRegs *r, uint64_t target) {
    if (broken || target < smod_lo || target >= smod_hi) return -1;
    if (run(r, target)) {
        say("%s: the game called %#llx, which isn't one of the mod's functions", SMOD_NAME, (unsigned long long)target);
        return -1;
    }
    return 0;
}

/* main's GOT: the values before init, and what the module put there since */
static uint32_t got_changed[64], n_changed;
static SmodMod mod;

static void got_scan(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < smod_got_count && n < 64; i++)
        if (*(const uint64_t *)A64_PTR(smod_got_slot[i]) != got_before[i]) got_changed[n++] = i;
    if (n != n_changed) {
        for (uint32_t k = 0; k < n; k++)
            say("%s redirects main's %s", SMOD_NAME, smod_got_name[got_changed[k]]);
    }
    n_changed = n;
    __atomic_store_n(&mod.redirects, n, __ATOMIC_RELEASE);
}

static int redirect(const char *name, uint64_t *target) {
    for (uint32_t k = 0; k < n_changed; k++) {
        const uint32_t i = got_changed[k];
        if (!strcmp(smod_got_name[i], name)) {
            *target = *(const uint64_t *)A64_PTR(smod_got_slot[i]);
            return 1;
        }
    }
    return 0;
}

static void init(const SmodRegs *game) {
    if (started) return;
    started = 1;
    if (host->commit(smod_lo, smod_hi - smod_lo)) {
        say("%s: guest memory at %#llx-%#llx isn't available: not started", SMOD_NAME, (unsigned long long)smod_lo,
            (unsigned long long)smod_hi);
        broken = 1;
        return;
    }
    uint8_t *m = A64_PTR(smod_lo);
    memcpy(m, smod_image, smod_image_size);
    memset(m + smod_image_size, 0, (size_t)(smod_hi - smod_lo - smod_image_size));
    host->written(smod_lo, smod_hi - smod_lo);
    mark(smod_lo, smod_hi - smod_lo);
    /* the .ips records kept, into main's memory: the mod's checks of its patch read them (the translated game doesn't) */
    for (uint32_t at = 0; at + 12 <= smod_patch_size;) {
        uint64_t addr;
        uint32_t len;
        memcpy(&addr, smod_patch + at, 8);
        memcpy(&len, smod_patch + at + 8, 4);
        memcpy(A64_PTR(addr), smod_patch + at + 12, len);
        mark(addr, len);
        at += 12 + len;
    }
    static uint64_t before[4096];
    got_before = before;
    for (uint32_t i = 0; i < smod_got_count && i < 4096; i++) got_before[i] = *(const uint64_t *)A64_PTR(smod_got_slot[i]);
    if (smod_init) {
        SmodRegs r = *game;
        r.x[31] = (game->x[31] - 0x200) & ~0xFULL;   /* (below the game's frame) */
        r.x[30] = 0;
        if (run(&r, smod_init)) say("%s: its init at %#llx wasn't translated", SMOD_NAME, (unsigned long long)smod_init);
    }
    got_scan();
    say("%s %s started: %u hook(s) on game calls, %u redirect(s) of main's imports", SMOD_NAME, SMOD_VERSION_TEXT,
        smod_hook_count, n_changed);
}

static void frame(const SmodRegs *game) {
    if (!started || broken) return;
    for (uint32_t i = 0; i < smod_hook_count && !broken; i++) {
        SmodRegs r = *game;
        r.x[31] = (game->x[31] - 0x200) & ~0xFULL;
        r.x[0] = 0, r.x[30] = 0;
        skip_call = smod_hook_replaced[i];
        run(&r, smod_hook_cave[i]);
        skip_call = 0;
    }
    got_scan();
}

SMOD_EXPORT const SmodMod *smod_attach(const SmodHost *h) {
    if (!h || h->version < SMOD_HOST_VERSION || h->size < sizeof(SmodHost)) {
        if (h && h->log) h->log(SMOD_NAME ": this game's mod loader is older than the mod: install the loader again");
        return NULL;
    }
    host = h;
    /* the runtime's marks of written pages, when the loader passes them and they're laid out as ours */
    const size_t with_dirty = offsetof(SmodHost, dirty_pages) + sizeof h->dirty_pages;
    if (h->size >= with_dirty && h->dirty && h->dirty_pages == (uint64_t)A64_DIRTY_PAGES) {
        smod_dirty = (uint8_t (*)[A64_DIRTY_PAGES])(void *)h->dirty;
    } else {
        smod_dirty = (uint8_t (*)[A64_DIRTY_PAGES])calloc(1, A64_DIRTY_PAGES);
        say("%s: this game's mod loader doesn't share the runtime's written-page marks: what the mod draws may not show"
            " (install the loader again)", SMOD_NAME);
    }
    mod.version = SMOD_MOD_VERSION, mod.size = sizeof(SmodMod);
    mod.name = SMOD_NAME, mod.version_text = SMOD_VERSION_TEXT;
    mod.lo = smod_lo, mod.hi = smod_hi;
    mod.init = init, mod.frame = frame, mod.call_in = call_in, mod.redirect = redirect;
    return &mod;
}
