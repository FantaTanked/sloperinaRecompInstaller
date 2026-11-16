/* sloperinaRecomp mod loader ABI: between a recomp runtime that has the loader (host/modloader.c, added by the mod manager)
   and a mod DLL (a stock Switch exefs mod translated on the player's PC by the mod manager, dll/).

   The loader is part of the game's exe, made with that runtime's headers; a mod DLL has its own copy of the translator's
   runtime. Nothing in here depends on either's internals: guest registers travel as SmodRegs, the guest window is at a
   fixed host address (a64-kit's A64_HOST_BASE, which every recomp of this family uses), and each side checks the other's
   version and struct sizes. Fields are only ever added at the end.

   What the loader does for a mod, in place of a Switch:
   - loads <game>/exefs_mods/<mod>/mod.dll the first time the game calls a function the runtime implements (an HLE), and
     runs the mod's init (its DT_INIT, as rtld would) on that thread;
   - main's imports: when a mod has redirected one in main's GOT (its hooks), the runtime's function for it calls the mod's
     instead (the translated main calls its imports directly, so the GOT itself is never read); the mod's own call of the
     original goes to the runtime's;
   - calls of the mod's code from the game (pointers a mod handed out: wrapped NVN procs ...) reach the DLL (call_in);
   - once a frame, on the game's thread (its controller read), the mod's frame: its .ips hooks on game calls. */
#ifndef SMOD_API_H
#define SMOD_API_H
#include <stdint.h>

#define SMOD_HOST_VERSION 1
#define SMOD_MOD_VERSION 1
#define SMOD_GUEST_BASE 0x100000000000ULL   /* (a64-kit's A64_HOST_BASE: guest address a is host address base + a) */

#ifdef _WIN32
#define SMOD_EXPORT __declspec(dllexport)
#else
#define SMOD_EXPORT __attribute__((visibility("default")))
#endif

/* A guest thread's registers, as a call into the game or out of it sees them */
typedef struct SmodRegs {
    uint64_t x[32];          /* x[31]: SP */
    uint64_t v[32][2];       /* Q0-Q31 */
    uint8_t n, z, c, v_flag; /* NZCV */
    uint32_t pad;
    uint64_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0;
} SmodRegs;

/* The controller as the game last read it (No.1 / handheld), before any mod's filter */
typedef struct SmodPad {
    uint64_t buttons;        /* nn::hid NpadButton bits (no stick directions: a mod adds them if it wants them) */
    int32_t stick[4];        /* left x, y, right x, y */
    int64_t when_ns;         /* when the game read it (0: never) */
    uint32_t connected;
    uint32_t pad;
} SmodPad;

/* A memory map entry, svcQueryMemory's shape */
typedef struct SmodMemInfo {
    uint64_t base, size;
    uint32_t state, attr, perm, pad;
} SmodMemInfo;

typedef struct SmodHost {
    uint32_t version, size;                        /* SMOD_HOST_VERSION, sizeof(SmodHost) */
    const char *runtime;                           /* "Longshot", for logs */
    const char *game_dir;                          /* the game folder (UTF-8) */
    void (*log)(const char *text);                 /* a line in the game's log */
    /* call the game's function at a guest address with r (x0-x7, v0-v7 its arguments, r->x[31] its stack), on the calling
       thread's context; its results come back in r. 0, or -1: no function there */
    int (*call)(SmodRegs *r, uint64_t target);
    void (*svc)(SmodRegs *r, uint32_t imm);         /* the game's system call, on r */
    int (*commit)(uint64_t addr, uint64_t size);    /* guest memory readable and writable there: 0, or -1 */
    void (*query)(uint64_t addr, SmodMemInfo *out); /* the game's memory map at addr (mods' own ranges excluded) */
    void (*pad)(SmodPad *out, int fresh);           /* the controller; fresh: read it now through the game first */
    int (*mount_sd)(const char *name);              /* mount <game>/user/sdmc as name: (0: done) */
    void (*screen)(uint32_t kind, const void *data, uint32_t size);   /* an overlay to draw (kind 1: gz text panels) */
    void (*written)(uint64_t addr, uint64_t size);  /* guest memory the GPU may read was written (a no-op if not needed) */
    /* the runtime's marks of written guest pages (a64-kit's a64_dirty, A64_WRITE_TRACK: a byte per 4 KB page, index
       (address >> 12) & (pages - 1)), which its renderer reads to send changed memory to the GPU again: a mod's stores
       mark them as the game's translated code does. NULL: the runtime doesn't track writes that way */
    uint8_t *dirty;
    uint64_t dirty_pages;
} SmodHost;

typedef struct SmodMod {
    uint32_t version, size;                        /* SMOD_MOD_VERSION, sizeof(SmodMod) */
    const char *name, *version_text;
    uint64_t lo, hi;                               /* the guest range its code and data occupy */
    void (*init)(const SmodRegs *game);            /* once, on the thread of the game's first runtime call */
    void (*frame)(const SmodRegs *game);           /* once a frame, on the game's thread (game: its registers then) */
    /* run the mod's code at target with r (a call the game made of it): 0, or -1 when target isn't the mod's */
    int (*call_in)(SmodRegs *r, uint64_t target);
    /* main's import `name` redirected by the mod (in main's GOT): 1 and its function in *target, else 0 */
    int (*redirect)(const char *name, uint64_t *target);
    volatile uint32_t redirects;                   /* how many it has redirected (0: redirect isn't asked) */
} SmodMod;

/* A mod DLL exports this: NULL if it can't run with this host (it logged why) */
typedef const SmodMod *(*SmodAttachFn)(const SmodHost *host);
#define SMOD_ATTACH "smod_attach"

#endif
