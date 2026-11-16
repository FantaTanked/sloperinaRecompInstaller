/* (sloperinaRecomp) the mod loader: stock Switch exefs mods as DLLs (translated on the player's PC by the mod manager,
   loader/smod_api.h), with what a Switch gives them.

   Added to the runtime once by the mod manager (loader/patch_loader.py). It changes nothing the game's translation
   depends on (no new HLE: the translator hooks those, and a new one would translate the game again), so adding it compiles
   only the runtime and the game's code comes from the builder's cache.

   - <game>/exefs_mods/<mod>/mod.dll are loaded, and their init run, the first time the game calls a runtime function (an
     HLE: each starts with hos_modloader_redirect, patch_loader.py), on that thread: before the game sets up NVN, as rtld
     runs a module's DT_INIT before main.
   - main's imports a mod redirected in main's GOT: the runtime's function for it calls the mod's instead
     (hos_modloader_redirect); the mod's own call of the original, made while it runs, goes to the runtime's.
   - The game's calls of a mod's code (pointers it handed out) come through the runtime's a64_call_miss (call_in).
   - Each of the game's controller reads (nn_hid.c's GetNpadStates) ends in hos_modloader_after_npad: the controller is noted
     (for mods' own reads), and on the first controller the game reads in a frame each mod's frame runs (its .ips hooks).
   - A mod calls the game's functions and system calls through the loader, on the calling thread's own context: its
     registers are swapped in for the call and the game's put back after.

   Without mods nothing here runs after the first call: the game is as it was. */
#include "hos.h"
#include "smod_api.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define LOADER_EXPORT __declspec(dllexport)
#else
#include <dlfcn.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <windows.h>   /* (host/posix's: SRWLOCK) */
#define LOADER_EXPORT __attribute__((visibility("default")))
#endif

#define MAX_MODS 8
#define SCREEN_MAX 0x10000
#define STATE_SIZE 0x28
#define ATTR_CONNECTED 1u
#define HOSTCALL_LO 0x06000000ULL   /* (hos_hostcall.c: no memory behind the host calls' addresses) */

#ifndef HOS_GAME_TITLE
#define HOS_GAME_TITLE "recomp"
#endif

/* (patch_loader.py adds these to the runtime, or stand-ins when it can't) */
int hos_modloader_mount_sd(const char *name);   /* nn_fs.c: <user>/sdmc mounted as name, 0 on success */
int hos_region_find(uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *next);   /* hos_mem.c */
/* (the game's public controller read, the runtime's own; declared extern so the translator's list of the runtime's
   functions doesn't take it for a new one) */
extern void imp__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(Ctx *c);
int a64_call_miss(Ctx *c, uint64_t target);   /* (rt/a64_rt.c; the runtime's host calls) */

static const SmodMod *mods[MAX_MODS];
static int nmods, loaded;
static char game_dir[1024];
static __thread Ctx *game_ctx;       /* the game thread's context while a mod runs (its calls go there) */
static __thread int in_mod;          /* a mod is running on this thread: the game's reads it makes aren't mod frames */

/* ---- the game's registers <-> a mod's ---- */
typedef struct Saved {
    uint64_t x[32], v[32][2], fpcr, fpsr, tpidr, tpidrro;
    uint8_t n, z, c, vf;
} Saved;
static void save(const Ctx *g, Saved *s) {
    memcpy(s->x, g->x, sizeof s->x);
    memcpy(s->v, g->vreg, sizeof s->v);
    s->fpcr = g->fpcr, s->fpsr = g->fpsr, s->tpidr = g->tpidr_el0, s->tpidrro = g->tpidrro_el0;
    s->n = g->n, s->z = g->z, s->c = g->c, s->vf = g->v;
}
static void restore(Ctx *g, const Saved *s) {
    memcpy(g->x, s->x, sizeof s->x);
    memcpy(g->vreg, s->v, sizeof s->v);
    g->fpcr = s->fpcr, g->fpsr = s->fpsr, g->tpidr_el0 = s->tpidr, g->tpidrro_el0 = s->tpidrro;
    g->n = s->n, g->z = s->z, g->c = s->c, g->v = s->vf;
}
static void regs_in(Ctx *g, const SmodRegs *r) {
    memcpy(g->x, r->x, sizeof r->x);
    memcpy(g->vreg, r->v, sizeof r->v);
    g->n = r->n, g->z = r->z, g->c = r->c, g->v = r->v_flag;
    g->fpcr = r->fpcr, g->fpsr = r->fpsr;
}
static void regs_out(const Ctx *g, SmodRegs *r) {
    memcpy(r->x, g->x, sizeof r->x);
    memcpy(r->v, g->vreg, sizeof r->v);
    r->n = g->n, r->z = g->z, r->c = g->c, r->v_flag = g->v;
    r->fpcr = g->fpcr, r->fpsr = g->fpsr, r->tpidr_el0 = g->tpidr_el0, r->tpidrro_el0 = g->tpidrro_el0;
}

/* ---- what a mod can ask for (SmodHost) ---- */
static void h_log(const char *text) { hos_log("mods: %s", text); }

static int h_call(SmodRegs *r, uint64_t target) {
    Ctx *g = game_ctx;
    if (!g) return -1;
    A64Fn f = a64_lookup(target);
    Saved s;
    save(g, &s);
    regs_in(g, r);
    int ok = 1;
    if (f) f(g);
    else ok = a64_call_miss(g, target);   /* (an address the runtime made: its host calls) */
    regs_out(g, r);
    restore(g, &s);
    return ok ? 0 : -1;
}

static void h_svc(SmodRegs *r, uint32_t imm) {
    Ctx *g = game_ctx;
    if (!g) return;
    Saved s;
    save(g, &s);
    regs_in(g, r);
    a64_svc(g, imm);
    regs_out(g, r);
    restore(g, &s);
}

static int h_commit(uint64_t addr, uint64_t size) {
    if (!size || addr >= HOS_SPACE_END || size > HOS_SPACE_END - addr) return -1;
#ifdef _WIN32
    return VirtualAlloc(GP(addr), (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
#else
    return 0;   /* (Linux: the whole window is mapped) */
#endif
}

/* svcQueryMemory with the whole map: the image readable, writable and executable, the heap and the address regions
   read-write, each gap one free region up to the next mapping (the mods' own ranges are theirs to answer) */
static void h_query(uint64_t addr, SmodMemInfo *out) {
    const struct { uint64_t lo, hi; uint32_t state, perm; } known[3] = {
        {HOS_SYSTEM_BASE, HOSTCALL_LO, 0x05, 3},
        {hos_image_base, hos_image_end, 0x03, 7},
        {HOS_HEAP_BASE, HOS_HEAP_BASE + hos_heap_size, 0x05, 3},
    };
    uint64_t base = 0, size = 0, next = HOS_SPACE_END;
    uint32_t state = 0, perm = 0;
    int hit = 0;
    for (int i = 0; i < 3 && !hit; i++) {
        if (addr >= known[i].lo && addr < known[i].hi)
            base = known[i].lo, size = known[i].hi - known[i].lo, state = known[i].state, perm = known[i].perm, hit = 1;
        else if (known[i].lo > addr && known[i].lo < next && known[i].hi > known[i].lo)
            next = known[i].lo;
    }
    for (int i = 0; i < nmods && !hit; i++)   /* (a gap ends at a mod) */
        if (mods[i]->lo > addr && mods[i]->lo < next) next = mods[i]->lo;
    if (!hit) {
        uint64_t rb, rs, rn;
        if (hos_region_find(addr, &rb, &rs, &rn)) base = rb, size = rs, state = 0x05, perm = 3, hit = 1;
        else if (rn < next) next = rn;
    }
    if (!hit) {   /* free, up to whatever is mapped next; 0 past the window: a walk ends */
        base = addr & ~0xFFFULL;
        size = base < HOS_SPACE_END ? (next > base ? next : HOS_SPACE_END) - base : 0;
    }
    memset(out, 0, sizeof *out);
    out->base = base, out->size = size, out->state = state, out->perm = perm;
}

/* the controller, as the game last read it */
static SRWLOCK pad_lock = SRWLOCK_INIT;
static SmodPad pad_last;
static uint64_t scratch;   /* guest: a state and an npad id, for a read of our own */

static void note_pad(uint64_t out, int n) {
    if (n <= 0 || !(G32(out + 32) & ATTR_CONNECTED)) return;
    AcquireSRWLockExclusive(&pad_lock);
    pad_last.buttons = G64(out + 8);
    for (int i = 0; i < 4; i++) pad_last.stick[i] = (int32_t)G32(out + 16 + 4 * i);
    pad_last.when_ns = (int64_t)hos_now_ns();
    pad_last.connected = 1;
    ReleaseSRWLockExclusive(&pad_lock);
}

static void h_pad(SmodPad *out, int fresh) {
    Ctx *g = game_ctx;
    if (fresh && g && (int64_t)hos_now_ns() - pad_last.when_ns > 50000000LL) {   /* (the game is held: read it here) */
        if (!scratch) scratch = hos_sys_alloc(0x40, 8);
        Saved s;
        save(g, &s);
        G32(scratch + STATE_SIZE) = 0;
        g->x[0] = scratch, g->x[1] = 1, g->x[2] = scratch + STATE_SIZE;
        imp__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(g);   /* (in_mod: not a mod frame) */
        const int n = (int)g->x[0];
        restore(g, &s);
        note_pad(scratch, n);
    }
    AcquireSRWLockShared(&pad_lock);
    *out = pad_last;
    ReleaseSRWLockShared(&pad_lock);
}

static int h_mount_sd(const char *name) { return hos_modloader_mount_sd(name); }

/* overlays: kind 1, gz's text panels, for the Vulkan back end */
static SRWLOCK screen_lock = SRWLOCK_INIT;
static uint8_t screen[SCREEN_MAX];
static uint32_t screen_len, screen_serial;

static void h_screen(uint32_t kind, const void *data, uint32_t size) {
    if (kind != 1) return;
    AcquireSRWLockExclusive(&screen_lock);
    if (data && size && size <= SCREEN_MAX) {
        memcpy(screen, data, size);
        screen_len = size;
    } else {
        screen_len = 0;
    }
    screen_serial++;
    ReleaseSRWLockExclusive(&screen_lock);
}

/* (the Vulkan back end, found by name in the exe) the latest panels into dst: their size (0: nothing to show, or more than
   cap), and in *serial a number that changes each frame */
LOADER_EXPORT int hos_sloperina_screen(void *dst, uint32_t cap, uint32_t *serial) {
    AcquireSRWLockShared(&screen_lock);
    const uint32_t n = screen_len <= cap ? screen_len : 0;
    if (n) memcpy(dst, screen, n);
    if (serial) *serial = screen_serial;
    ReleaseSRWLockShared(&screen_lock);
    return (int)n;
}

static void h_written(uint64_t addr, uint64_t size) {
#ifdef HOS_WRITTEN
    HOS_WRITTEN(GP(addr), (size_t)size);
#else
    (void)addr, (void)size;
#endif
}

#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__) && defined(A64_DIRTY_PAGES)
#define LOADER_DIRTY a64_dirty, (uint64_t)A64_DIRTY_PAGES   /* (rt/a64.h: the marks the renderer reads) */
#else
#define LOADER_DIRTY NULL, 0
#endif
static const SmodHost host = {
    SMOD_HOST_VERSION, sizeof(SmodHost), HOS_GAME_TITLE, game_dir, h_log, h_call, h_svc, h_commit, h_query, h_pad,
    h_mount_sd, h_screen, h_written, LOADER_DIRTY,
};

/* ---- finding and loading the mods ---- */
static int exists_dir(const char *p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    DIR *d = opendir(p);
    if (d) closedir(d);
    return d != NULL;
#endif
}

static char *last_sep(char *p) {
    char *a = strrchr(p, '/');
#ifdef _WIN32
    char *b = strrchr(p, '\\');
    if (b && (!a || b > a)) a = b;
#endif
    return a;
}

/* the game folder: the exe's own, or the one above it (the launcher layout: the game in bin\), whichever has exefs_mods */
static void find_game_dir(void) {
    char dir[1024] = "", probe[1100];
#ifdef _WIN32
    GetModuleFileNameA(NULL, dir, sizeof dir);
#else
    const ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[n > 0 ? n : 0] = 0;
#endif
    char *s = last_sep(dir);
    if (s) *s = 0;
    snprintf(game_dir, sizeof game_dir, "%s", dir);
    snprintf(probe, sizeof probe, "%s/exefs_mods", dir);
    if (exists_dir(probe)) return;
    s = last_sep(dir);
    if (!s) return;
    *s = 0;
    snprintf(probe, sizeof probe, "%s/exefs_mods", dir);
    if (exists_dir(probe)) snprintf(game_dir, sizeof game_dir, "%s", dir);
}

static void load_one(const char *dir, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/exefs_mods/%s/mod.dll", dir, name);
#ifdef _WIN32
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    HMODULE lib = LoadLibraryA(path);
    SmodAttachFn attach = lib ? (SmodAttachFn)(void *)GetProcAddress(lib, SMOD_ATTACH) : NULL;
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    SmodAttachFn attach = lib ? (SmodAttachFn)dlsym(lib, SMOD_ATTACH) : NULL;
#endif
    if (!attach) {
        hos_log("mods: %s/mod.dll isn't a mod for this loader (no %s): left out", name, SMOD_ATTACH);
        return;
    }
    const SmodMod *m = attach(&host);
    if (!m || m->version < 1 || m->size < sizeof(SmodMod) || !m->init || !m->frame || !m->call_in || !m->redirect) {
        hos_log("mods: %s didn't start (see above)", name);
        return;
    }
    for (int i = 0; i < nmods; i++)
        if (m->lo < mods[i]->hi && mods[i]->lo < m->hi) {
            hos_log("mods: %s uses the same memory as %s: left out", m->name, mods[i]->name);
            return;
        }
    if (nmods < MAX_MODS) mods[nmods++] = m;
    hos_log("mods: %s %s loaded (%#llx-%#llx)", m->name, m->version_text ? m->version_text : "",
            (unsigned long long)m->lo, (unsigned long long)m->hi);
}

static void load_mods(void) {
    loaded = 1;
    find_game_dir();
    char root[1100];
    snprintf(root, sizeof root, "%s/exefs_mods", game_dir);
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') load_one(game_dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(root);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') load_one(game_dir, e->d_name);
    closedir(d);
#endif
    hos_log("mods: %d loaded from %s", nmods, root);
}

/* (the mod manager looks for this in the exe: the loader is in, and its ABI) */
LOADER_EXPORT const uint32_t hos_modloader_abi = SMOD_HOST_VERSION;

/* (patch_loader.py's insertions test these first: nothing else runs while they're 0) */
volatile int hos_smod_on = 1;      /* the first runtime call loads the mods; then: some mod redirects an import */
volatile int hos_smod_calls;       /* mods are loaded: the game may call their code */

static SRWLOCK load_lock = SRWLOCK_INIT;

static void update_on(void) {
    int any = 0;
    for (int i = 0; i < nmods; i++) any |= mods[i]->redirects != 0;
    hos_smod_on = any;
}

/* run a mod's code for the game, on this thread's context c (its registers in, its results back in c) */
static int enter(Ctx *c, const SmodMod *m, uint64_t target) {
    SmodRegs r;
    regs_out(c, &r);
    Ctx *outer = game_ctx;
    const int was = in_mod;
    game_ctx = c, in_mod = 1;
    const int ok = m->call_in(&r, target) == 0;
    if (ok) regs_in(c, &r);
    game_ctx = outer, in_mod = was;
    return ok;   /* (0: not the mod's code after all: the caller does what it would have without the mod) */
}

static void load_and_init(Ctx *c) {
    AcquireSRWLockExclusive(&load_lock);
    if (!loaded) {
        load_mods();
        if (nmods) {
            SmodRegs r;
            regs_out(c, &r);
            Ctx *outer = game_ctx;
            game_ctx = c, in_mod = 1;
            for (int i = 0; i < nmods; i++) mods[i]->init(&r);
            game_ctx = outer, in_mod = 0;
            hos_smod_calls = 1;
        }
        update_on();
    }
    ReleaseSRWLockExclusive(&load_lock);
}

/* (each of the runtime's HLE functions starts with this, patch_loader.py) main's import `name`: 1 when a mod's function
   ran in its place (its results in c), 0 to run the runtime's */
int hos_modloader_redirect(Ctx *c, const char *name) {
    if (!loaded) load_and_init(c);
    if (in_mod) return 0;   /* (a mod calling the original) */
    for (int i = 0; i < nmods; i++) {
        uint64_t target;
        if (mods[i]->redirects && mods[i]->redirect(name, &target) && target >= mods[i]->lo && target < mods[i]->hi)
            return enter(c, mods[i], target);
    }
    return 0;
}

/* (the runtime's a64_call_miss starts with this, patch_loader.py) the game calling a mod's code: 1 when it ran */
int hos_modloader_call_in(Ctx *c, uint64_t target) {
    for (int i = 0; i < nmods; i++)
        if (target >= mods[i]->lo && target < mods[i]->hi)
            return enter(c, mods[i], target);
    return 0;
}

/* ---- the game's controller reads (nn_hid.c): after GetNpadStates(out, count, id), x0 the count. redirected: a mod's
   function ran in its place (the states are what it made of them; the runtime's own, read inside it, were noted then) ---- */
static uint64_t anchor_out, anchor_id = ~0ULL;
static int64_t last_frame_ns;

void hos_modloader_after_npad(Ctx *c, uint64_t out, uint64_t id_ptr, int redirected) {
    const int n = (int)c->x[0];
    if (!redirected) note_pad(out, n);
    if (in_mod || !nmods) return;   /* (a mod's own read) */
    const uint64_t id = id_ptr ? G32(id_ptr) : 0;
    const int64_t now = (int64_t)hos_now_ns();
    /* a frame: the first controller the game read (its id and buffer), each time it reads it again; and whatever it reads
       when that hasn't come round for a tenth of a second (the game reads another one now) */
    if (anchor_id == ~0ULL || now - last_frame_ns > 100000000LL) anchor_id = id, anchor_out = out;
    if (id != anchor_id || out != anchor_out) return;
    last_frame_ns = now;
    SmodRegs r;
    regs_out(c, &r);
    game_ctx = c, in_mod = 1;
    for (int i = 0; i < nmods; i++) mods[i]->frame(&r);
    game_ctx = NULL, in_mod = 0;
    c->x[0] = (uint64_t)n;
    update_on();
}
