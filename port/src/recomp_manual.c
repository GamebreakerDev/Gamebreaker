/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <windows.h>  /* QueryPerformanceCounter, for the sound thread's clock */
#include <tlhelp32.h> /* the thread list, for the stall dump */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv */
#include <string.h>   /* memset, strrchr */
#include <math.h>     /* powf, log10f: the fold-down of mix-bin volumes */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#include <xbox/xboxrecomp.h>   /* RECOMP_TLS */
#include "gb_overlay.h"            /* the movie being played, the start screen */
#include "gb_log.h"                /* the log for bug reports */
#include "gb_mods.h"               /* the player's replacement files */
int gb_d3d11_reflip(void);           /* gb_d3d11.c: show the start screen while the boot waits */

/* Thread-local in the runtime; a plain extern would read the wrong copy. */
extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define GB_MEM32(va) \
    (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/* ── Contiguous memory: place the big pools from the top ─────
 *
 * The title takes two 24 MB contiguous pools at start-up (guest 0x001AB5D0
 * and 0x001588D0), both "anywhere" requests, and does not check the second
 * for failure: a NULL there becomes a heap based at address 0, which later
 * overwrites the thread block and faults far from the cause.
 *
 * A console's kernel fills contiguous memory from the top down, so both fit.
 * The toolkit's arena fills from the bottom and keeps the megabyte below
 * 16 MB in reserve, which leaves the second pool 128 KB short. Rather than
 * change the toolkit, large unconstrained requests are turned into pinned
 * ones (lowest/highest naming one exact block), stacked down from the top
 * of the arena. The toolkit's own bridge honours a pinned request, zeroes it
 * and records it.
 *
 * Small requests are left alone, so D3D's buffers stay low as before.
 */
#define GB_KTHUNK_MMALLOC_CONTIG_EX 0x0022AD5Cu      /* import slot, .rdata */
#define GB_CONTIG_BASE              0x80000000u
#define GB_CONTIG_TOP               (64u * 1024u * 1024u - 128u * 1024u)
#define GB_BIG_REQUEST              (8u * 1024u * 1024u)

extern const volatile uint8_t *xbox_ContiguousPageMap(void);

static void gb_MmAllocateContiguousMemoryEx(void)
{
    /* Entered like any lifted callee: return address at [esp], then the
     * five stdcall arguments. */
    uint32_t size = GB_MEM32(g_esp + 4);
    uint32_t low  = GB_MEM32(g_esp + 8);
    uint32_t high = GB_MEM32(g_esp + 12);
    recomp_func_t real =
        recomp_lookup_kernel(GB_MEM32(GB_KTHUNK_MMALLOC_CONTIG_EX));

    if (size >= GB_BIG_REQUEST && low == 0 && high == 0xFFFFFFFFu) {
        /* Not thread-safe by itself; both pools are taken by the start-up
         * thread before any other thread allocates. */
        static uint32_t next_top = GB_CONTIG_TOP;
        uint32_t bytes = (size + 0xFFFu) & ~0xFFFu;
        const volatile uint8_t *used = xbox_ContiguousPageMap();

        if (bytes <= next_top - 0x1000u && used) {
            uint32_t base = next_top - bytes, q, clear = 1;

            for (q = base >> 12; q < next_top >> 12; q++)
                if (used[q]) { clear = 0; break; }
            if (clear) {
                GB_MEM32(g_esp + 8)  = base;              /* lowest  */
                GB_MEM32(g_esp + 12) = base + bytes - 1;  /* highest */
                next_top = base;
            }
        }
    }
    if (real)
        real();
}

/* ── Pushbuffer ring wrap: publish the tail before D3D jumps ──
 *
 * D3D writes GPU commands into a 512 KB ring. When it reaches the end, its
 * make-space routine (guest 0x00205940) writes a JUMP back to the start at
 * the current write position, moves to the start, and only then tells the
 * GPU how far it may read (the kick-off, which sets the DMA_PUT register).
 * So the last commands before the JUMP are never announced on their own: the
 * first PUT the GPU sees after them is already back at the start of the ring.
 *
 * A real GPU does not care; it follows the JUMP. The toolkit's default
 * pushbuffer walk (the "segment scan" in nv2a_pb_scan.c) does not follow it.
 * On a wrap it reads from the last PUT up to the highest PUT it has ever
 * seen, then carries on from the lowest. D3D never kicks off from the last
 * 516 bytes of the ring, so whatever it wrote between the highest PUT and the
 * JUMP was dropped: usually one quad, sometimes a whole batch. In the menus
 * that is one frame in eleven with a piece missing (a line of text, a tile of
 * the background movie; 375 of 375 odd frames in 83.6 s were at a wrap,
 * none in over 8,000 frames with this hook). The toolkit's faithful walk (RECOMP_PB_WALK=dma) follows the JUMP
 * but freezes this title in D3D's fence wait, so it is not an option yet.
 *
 * The fix here: when make-space is entered and is about to wrap, kick off
 * first. That publishes a PUT at the exact position where the JUMP will go,
 * the walk executes everything up to it, and nothing is left behind. An extra
 * kick-off is harmless: it is what make-space does at its end anyway.
 *
 * This is an entry hook: the lifter calls sub_XXXXXXXX_enter at the top of
 * the generated function when it is run with --exclude-manual on this file
 * (port/regen.ps1 does).
 *
 * Addresses and offsets are for the pinned executable (XDK 5233 D3D):
 *   device pointer     global at 0x0020FCC8
 *   device+0x00        write position (guest address)
 *   device+0x08        flags; bit 2 = recording into a pushbuffer of the title's own
 *   device+0x28        end of the ring
 *   kick-off           0x00205600, device in ecx
 */
#define GB_D3D_DEVICE_PTR_VA 0x0020FCC8u

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebp, g_seh_ebp;
extern void sub_00205600(void);                 /* D3D kick-off */

static int gb_wrap_kick_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = 1;
    }
    return on;
}

void sub_00205940_enter(void)
{
    /* Entered like the function itself: return address at [esp], then the two
     * stdcall arguments (the least and the most space wanted, in bytes). */
    uint32_t dev = GB_MEM32(GB_D3D_DEVICE_PTR_VA);
    uint32_t put, end, least, most;

    if (!dev || (GB_MEM32(dev + 0x08) & 4u))
        return;
    put   = GB_MEM32(dev + 0x00);
    end   = GB_MEM32(dev + 0x28);
    least = GB_MEM32(g_esp + 4);
    most  = GB_MEM32(g_esp + 8);
    /* The same two tests the function makes (0x002059BF to 0x002059D1): it
     * wraps only when the larger request plus 16 KB does not fit before the
     * end, and the smaller request does not fit either. */
    if (put + most + 0x4000u < end || put + least <= end)
        return;
    if (!gb_wrap_kick_enabled())
        return;
    {
        /* Everything the kick-off touches is put back, so the function
         * starts exactly as it would have without the hook. */
        uint32_t a = g_eax, c = g_ecx, d = g_edx, bp = g_ebp, sbp = g_seh_ebp;

        g_ecx = dev;
        g_esp -= 4;
        GB_MEM32(g_esp) = 0x00205940u;          /* return address, popped by the callee */
        sub_00205600();
        g_eax = a; g_ecx = c; g_edx = d; g_ebp = bp; g_seh_ebp = sbp;
    }
}

/* ── D3D's kick-off: wait instead of spin ──────────────────────
 *
 * D3D's kick-off (guest 0x00205600, device in ecx) publishes the write
 * position to the GPU. Before it does, it sets the write-back flush bit
 * (bit 16 of GPU register 0x100410) and loops on that bit, with no call in
 * the loop, until the "GPU" clears it. Here the GPU is the toolkit's executor
 * thread, which clears the bit between commands and once per pass, and its
 * pass includes the 60 Hz frame limit. So the game's main thread spins
 * through most of every frame: the one processor the game's threads are
 * pinned to runs at 100% though the game's own work is about 1 ms a frame.
 * On a desktop that costs nothing visible; on a laptop it is heat and power
 * taken from the budget the executor needs. A second routine (0x002056E0)
 * makes the same wait before it publishes a position.
 *
 * Both are written here by hand, the rest of each as the game has it, with the
 * wait changed. Three ways to wait:
 *   spin   the game's own loop.
 *   timer  spin on the bit (with the processor's pause hint) for up to 2 ms,
 *          then sleep on a high-resolution waitable timer in 250 us steps. A
 *          match makes about 40 kick-offs a frame and the executor answers
 *          most of them during its walk, within the 2 ms; only the one long
 *          wait a frame, while the executor sits in its frame limit, is slept
 *          through. (Sleeping after 50 us cost a quarter of the frame rate.)
 *   event  spin briefly, then wait on the event the executor sets when it
 *          clears the bit (the toolkit patch's xbox_Nv2aAckedEvent); the
 *          executor itself sleeps when it has nothing to do and the kick-off
 *          wakes it (xbox_Nv2aKick, xbox_Nv2aSetIdleWait). Every wait has a
 *          time limit, so a missed signal costs a millisecond, never a hang.
 *          The default, at every rate: in a match the main thread uses about
 *          5% of a core instead of 99%, at the same frame rate.
 * Kick-offs made while the game records into a pushbuffer of its own (device
 * flags bit 13) do not wait; they go to the lifted function unchanged.
 *
 * Addresses are for the pinned executable (XDK 5233 D3D):
 *   device+0x00     write position          device+0x08  flags
 *   device+0x358    write position while recording (flags bit 2)
 *   device+0x17C0   the GPU's DMA channel   [0x0020FCC8]+0x17C8  GPU registers
 *   0x0020F820      non-zero: call 0x002007E0 until it returns 0, then set
 *                   flags bit 13 (0x00205600 only; 0x002056E0 always calls it)
 */
#define GB_KICK_SPIN   0
#define GB_KICK_TIMER  1
#define GB_KICK_EVENT  2
#define GB_KICK_WAIT_DEFAULT  GB_KICK_EVENT
#define GB_KICK_POLL_US       2000     /* timer: spin this long before the first sleep */
#define GB_KICK_EVENT_US      20       /* event: spin this long before waiting */
#define GB_KICK_STEP_US       250      /* timer: sleep in steps of this */
#define GB_NV2A_IDLE_MS       1        /* event: the executor's longest idle sleep */

extern void sub_00205600_gen(void);
extern void sub_002056E0_gen(void);
extern void sub_002007E0(void);
extern void xbox_Nv2aSetIdleWait(int ms);  /* the toolkit patch */
extern void xbox_Nv2aKick(void);
extern void *xbox_Nv2aAckedEvent(void);

static struct {
    volatile LONG state;               /* 0 not read, 1 reading, 2 ready */
    int mode;
    LONG poll_ticks, step_100ns;
    HANDLE acked;
    LARGE_INTEGER freq;
} s_kick;

static RECOMP_TLS HANDLE t_kick_timer;

static void gb_kick_start(void)
{
    int idle_ms;

    for (;;) {
        LONG s = InterlockedCompareExchange(&s_kick.state, 1, 0);
        if (s == 2) return;
        if (s == 0) break;
        YieldProcessor();
    }
    QueryPerformanceFrequency(&s_kick.freq);
    s_kick.mode = GB_KICK_WAIT_DEFAULT;
    idle_ms = GB_NV2A_IDLE_MS;
    s_kick.poll_ticks = (LONG)(s_kick.freq.QuadPart
                               * (s_kick.mode == GB_KICK_TIMER ? GB_KICK_POLL_US
                                                               : GB_KICK_EVENT_US) / 1000000);
    s_kick.step_100ns = GB_KICK_STEP_US * 10;
    s_kick.acked = (HANDLE)xbox_Nv2aAckedEvent();
    if (s_kick.mode == GB_KICK_EVENT && !s_kick.acked)
        s_kick.mode = GB_KICK_TIMER;
    /* The executor sleeps when idle only when the kick-off wakes it. */
    xbox_Nv2aSetIdleWait(s_kick.mode == GB_KICK_EVENT ? idle_ms : 0);
    InterlockedExchange(&s_kick.state, 2);
}

/* Set the flush bit, tell the executor, and wait until it is cleared. */
static void gb_kick_flush(void)
{
    uint32_t regs = GB_MEM32(GB_MEM32(GB_D3D_DEVICE_PTR_VA) + 0x17C8);
    volatile uint32_t *bit = &GB_MEM32(regs + 0x100410);
    LARGE_INTEGER t0, t, due;

    *bit = *bit | 0x10000u;
    xbox_Nv2aKick();
    if (s_kick.mode == GB_KICK_SPIN) {
        while (*bit & 0x10000u)
            ;
        return;
    }
    QueryPerformanceCounter(&t0);
    do {
        if (!(*bit & 0x10000u)) {
            return;
        }
        YieldProcessor();
        QueryPerformanceCounter(&t);
    } while (t.QuadPart - t0.QuadPart < s_kick.poll_ticks);
    if (s_kick.mode == GB_KICK_EVENT) {
        while (*bit & 0x10000u) {
            WaitForSingleObject(s_kick.acked, 1);  /* the limit: a missed set costs 1 ms */
        }
    } else {
        if (!t_kick_timer) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
            t_kick_timer = CreateWaitableTimerExW(NULL, NULL,
                                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
            if (!t_kick_timer) {            /* before Windows 10 1803: spin as the game does */
                while (*bit & 0x10000u)
                    YieldProcessor();
                return;
            }
        }
        due.QuadPart = -(LONGLONG)s_kick.step_100ns;
        while (*bit & 0x10000u) {
            SetWaitableTimer(t_kick_timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(t_kick_timer, 50);
        }
    }
}


/* Calls 0x002007E0 until it returns 0, as both functions do (return address
 * 0x0020566D or 0x00205745). */
static void gb_kick_drain(uint32_t ret, uint32_t sbp)
{
    do {
        g_ebp = sbp;
        g_seh_ebp = sbp;
        g_esp -= 4;
        GB_MEM32(g_esp) = ret;              /* return address, popped by the callee */
        sub_002007E0();
    } while (g_eax);
}

void sub_00205600(void)
{
    uint32_t dev = g_ecx, flags, put, sbp = g_seh_ebp;

    if (s_kick.state != 2)
        gb_kick_start();
    flags = GB_MEM32(dev + 0x08);
    if (flags & 0x2000u) {
        sub_00205600_gen();
        xbox_Nv2aKick();
        return;
    }
    put = (flags & 4u) ? GB_MEM32(dev + 0x358) : GB_MEM32(dev);
    gb_kick_flush();
    /* From here as the function goes on (0x0020564C): the device is kept on
     * the stack across the call, as the function keeps ecx there. */
    g_esp -= 4;
    GB_MEM32(g_esp) = dev;
    put &= 0x0FFFFFFFu;
    GB_MEM32(GB_MEM32(dev + 0x17C0) + 0x40) = put;
    xbox_Nv2aKick();
    g_edx = put;
    g_eax = GB_MEM32(0x0020F820u);
    if (g_eax) {
        gb_kick_drain(0x0020566Du, sbp);
        GB_MEM32(dev + 0x08) = GB_MEM32(dev + 0x08) | 0x2000u;
        g_eax = dev;
    }
    g_ecx = dev;
    g_esp += 4;                             /* the device */
    g_ebp = sbp;
    g_seh_ebp = sbp;
    g_esp += 4;                             /* ret */
}

void sub_002056E0(void)
{
    uint32_t dev = g_ecx, flags, put, sbp = g_seh_ebp;

    if (s_kick.state != 2)
        gb_kick_start();
    flags = GB_MEM32(dev + 0x08);
    if (flags & 0x2000u) {
        sub_002056E0_gen();
        return;
    }
    put = (flags & 4u) ? GB_MEM32(dev + 0x358) : GB_MEM32(dev);
    gb_kick_flush();
    /* From here as the function goes on (0x0020572C). */
    g_esp -= 4;
    GB_MEM32(g_esp) = dev;
    put &= 0x0FFFFFFFu;
    GB_MEM32(GB_MEM32(dev + 0x17C0) + 0x40) = put;
    xbox_Nv2aKick();
    g_edx = put;
    gb_kick_drain(0x00205745u, sbp);
    g_ecx = GB_MEM32(g_esp);
    g_esp += 4;                             /* the device */
    g_ebp = sbp;
    g_seh_ebp = sbp;
    g_esp += 4;                             /* ret */
}

#define GB_MEM8(va) \
    (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* -1 not read, 0 off or boot over, 2 GB_SKIP_INTRO (the START press below);
 * 1 the experiment above, in a working build only. */
static int gb_skip_intro_state = -1;

/* ── Skip the intro film: GB_SKIP_INTRO=1 (works; off by default) ───────────
 *
 * Presses START for the player while the intro film (xmt0018) plays, which
 * is what a player would do: the game's own skip, nothing set from outside. The title
 * screen then comes up about 4 s into the film, about 24 s from start,
 * instead of 63.5 s. The EA logo and the NBA legal notice still play: the game
 * ignores START in both (measured), and ending them any other way (setting
 * the movie player's own skip flag) made the game's video codec fail.
 *
 * When: 4 s after the movie player is first seen on the film with its stream
 * ready. Measured before choosing: a START
 * 3.6 s into the film gave the title 0.1 s later, and the scripted laps have
 * pressed it about 8.5 s in hundreds of times without fault. If the film is
 * still current 3 s after a press (a press the title did not see), it presses
 * again, three presses at most. Only while the film is the current entry:
 * never on the logo, the legal notice, the title screen or later, because the
 * hook stops for the rest of the run at the first movie that is not a boot
 * movie (the title screen's xmt0015).
 *
 * How: the press goes through the toolkit's scripted pad, the same path the
 * test scripts drive (RECOMP_PAD_LIVE, a file of button lines read by the USB
 * pad model and merged with the real pad or keyboard on pad 1). When no
 * script has set that file, main.c points it at one in %TEMP% at start-up
 * (gb_skip_intro_pad_setup). Held 100 ms.
 *
 * Entry hook on 0x000DA010. */
static int gb_skip_intro_press(void)
{
    const char *path = getenv("RECOMP_PAD_LIVE");
    static const char line[] = "start:100\n";
    HANDLE h;
    DWORD put = 0;
    if (!path || !path[0])
        return 0;
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    WriteFile(h, line, (DWORD)(sizeof line - 1), &put, NULL);
    CloseHandle(h);
    return put == sizeof line - 1;
}

/* 1 = a boot movie that is skipped, 2 = a boot movie that plays (the boot
 * window stays open through it), 0 = not a boot movie (the boot is over). */
static int gb_boot_movie_kind(const char *base)
{
    static const char *const skipped[] = {
        "xmt0018.nfx", "xmtjp18.nfx", "xmtfr18.nfx",   /* intro film */
    };
    static const char *const played[] = {
        "xmt0017.nfx", "xmtjp17.nfx", "xmtfr17.nfx",   /* EA logo */
        "xmt0016.nfx", "xmtjp16.nfx", "xmtfr16.nfx",   /* legal notice, team logos */
    };
    size_t i;
    for (i = 0; i < sizeof skipped / sizeof skipped[0]; i++)
        if (_stricmp(base, skipped[i]) == 0)
            return 1;
    for (i = 0; i < sizeof played / sizeof played[0]; i++)
        if (_stricmp(base, played[i]) == 0)
            return 2;
    return 0;
}

void sub_000DA010_enter(void)
{
    uint32_t player = g_ecx, name_va;
    int8_t entry;
    char name[64], *base;
    size_t n;
    int kind;

    static ULONGLONG press_ms = 4000;


    if (gb_skip_intro_state < 0) {
        const char *p = getenv("GB_SKIP_INTRO");
        gb_skip_intro_state = (p && p[0] && p[0] != '0') ? 2 : 0;
    }
    if (!player)
        return;
    entry = (int8_t)GB_MEM8(player + 0xFE);
    if (entry < 0 || entry > 4)
        return;
    name_va = GB_MEM32(player + 0xE0 + 4u * (uint32_t)entry);
    if (!name_va)
        return;
    for (n = 0; n < sizeof name - 1; n++) {
        name[n] = (char)GB_MEM8(name_va + n);
        if (!name[n])
            break;
    }
    name[n] = 0;
    base = strrchr(name, '/');
    if (!base) base = strrchr(name, '\\');
    base = base ? base + 1 : name;

    /* The overlay (gb_overlay.c) shows the project's corner mark on the
     * screens whose background movie it is told about here. */
    gb_overlay_note_movie(base);
    {
        /* The project's start screen comes before the publisher's logo. If
         * the boot loading is over before the start screen is, the first
         * movie waits for it here, on its first update (before its stream
         * starts), while the back end keeps showing the start screen; at most
         * 4 s, so nothing can wait for ever. */
        static int held;
        if (!held) {
            ULONGLONG from = GetTickCount64();
            unsigned left;
            held = 1;
            while ((left = gb_overlay_start_left_ms()) > 0 && GetTickCount64() - from < 4000) {
                gb_d3d11_reflip();
                Sleep(left < 15 ? left : 15);
            }
        }
    }
    if (gb_skip_intro_state <= 0)
        return;

    kind = gb_boot_movie_kind(base);
    if (kind == 2)
        return;                 /* the logo and the legal notice play: see the top */
    if (kind == 1) {
        static uint32_t seen_player, seen_name;
        static int8_t seen_entry = -2;
        static ULONGLONG seen_at;
        ULONGLONG now = GetTickCount64();

        if (player != seen_player || name_va != seen_name || entry != seen_entry) {
            seen_player = player; seen_name = name_va; seen_entry = entry;
            seen_at = 0;
        }
        if (gb_skip_intro_state == 2) {
            /* GB_SKIP_INTRO: press START, as a player would (see above). The
             * clock starts when the stream is first seen ready. */
            static int presses;
            static ULONGLONG last_press;
            if (GB_MEM8(player + 0xFC) != 0)
                return;
            if (!seen_at)
                seen_at = now;
            if (presses >= 3 || now - seen_at < press_ms ||
                (presses && now - last_press < 3000))
                return;
            presses++;
            last_press = now;
            if (gb_skip_intro_press()) {
            } else {
                presses = 3;
                gb_log("[GB] skip intro: cannot press START (no RECOMP_PAD_LIVE file); the film plays\n");
            }
            return;
        }
    } else {
        gb_skip_intro_state = 0;
    }
}

/* ── Sound thread: a 1 ms clock, and a longer lead ───────────
 *
 * The title's sound system mixes music, speech and crowd on the CPU and
 * streams the result through six looping 50 ms buffers. Its own thread (guest
 * 0x001B2AF0) runs 100 times a second and each time mixes up to 20 ms ahead of
 * the audio chip's play cursor. When the cursor gets past what was written,
 * the chip plays what it played 50 ms earlier: a click. Measured as "replays"
 * of voice 252. The thread wakes late because the title's clock
 * (KeTickCount) is coarse, so its 10 ms sleeps land unevenly; the emulated
 * chip's cursor also moves in bursts.
 *
 * Two changes, both for this title only. BOTH ARE OFF UNLESS SWITCHED ON:
 * together they remove the replays (9 a second to none on Direct3D 11, 25 to
 * none on the CPU rasteriser), but with them on the sound thread stalled or
 * stopped right after the second intro movie was skipped in 3 of 91
 * boots, and in none of 71 without them. They are not the default; the
 * launcher's Crackle fix setting switches them on.
 *
 * 1. The clock. The thread keeps its 10 ms schedule against XAPI
 *    GetTickCount (guest 0x001CAB6A, three instructions: read the kernel's
 *    tick word). On a console that word moves every millisecond. The toolkit
 *    fills it from the host's GetTickCount(), which moves in steps of 15 to
 *    16 ms, and it does so from the thread that also executes the pushbuffer,
 *    so it stands still while a frame is drawn. A stale clock makes the
 *    thread sleep too long by exactly the staleness. The function is defined
 *    by hand here and returns a millisecond counter built on the host's
 *    performance counter, starting from the kernel word's value so the epoch
 *    is the same. Its only callers are the sound start-up and the sound
 *    thread, and nothing else in the title reads the kernel word by a direct
 *    address, so nothing else changes.
 *      GB_AUDIO_TICK=1          the 1 ms clock (unset or 0: the kernel's tick
 *                               word, as the title was written)
 *
 * 2. The lead. The emulated chip does not read at a steady rate: it renders
 *    256 samples at a time and catches up in bursts of up to 27 ms after its
 *    thread has been held up. 20 ms of lead does not cover that. The sound
 *    set-up (guest 0x001B2750) computes the lead in frames and keeps it in
 *    one word that the writer re-reads on every call; it is wrapped here and
 *    the word is raised afterwards. The ring stays 50 ms, so the lead is
 *    capped below it. The cost is latency: sound mixed by the title (not the
 *    sound effects on their own voices) is heard later by the difference.
 *      GB_AUDIO_LEAD_MS=<n>     lead in milliseconds (40 is the measured
 *                               value); unset, 0 or 20 leaves the title's
 *                               own 20 ms
 *
 * Both need the lifter run with --exclude-manual on this file (port/regen.ps1
 * does): the first is then not generated at all, and the second is generated
 * under the name with _gen appended for the wrapper to call.
 *
 * Addresses are for the pinned executable.
 */
#define GB_KTHUNK_KETICKCOUNT 0x0022AD54u   /* import slot: guest address of the kernel's tick word */
#define GB_SND_NEXT_VA        0x002ED108u   /* the sound thread's next wake time, in ticks */
#define GB_SND_RATE_VA        0x002ECE3Au   /* word: the mixer's sample rate */
#define GB_SND_RING_VA        0x002ED188u   /* length of each stream buffer, in frames */
#define GB_SND_LEAD_VA        0x002ED18Cu   /* how far ahead of the cursor the writer mixes, in frames */

#define GB_AUDIO_LEAD_MS_DEFAULT 0u         /* 0: leave the title's own lead */

#define GB_SND_LAST_VA        0x002ED190u   /* where the writer's last mix ended, in frames */
#define GB_DS_VOICES_PTR_VA   0x00224A68u   /* DirectSound: pointer to the chip's voice array */
#define GB_SND_FIRST_VOICE    248u          /* the six stream buffers are voices 248 to 253 */

typedef struct {
    int64_t us;
    uint32_t ret, kernel, next, tid;
    uint32_t last, cur0, cur4;              /* writer position; play cursors of voices 248 and 252 */
} GbTickRec;
#define GB_TICK_RECS (1u << 19)             /* 87 minutes at 100 calls a second */

static struct {
    volatile LONG state;                    /* 0 not started, 1 starting, 2 ready */
    int fine;                               /* the 1 ms clock is in use */
    LARGE_INTEGER freq, t0;
    uint32_t base;                          /* the kernel word at the first call */
    GbTickRec *rec;                         /* calls recorded so far */
    volatile LONG rec_n;
    FILE *rec_file;
    DWORD first_tid;                        /* the start-up thread makes the first call */
    int watch;                              /* passes per 250 ms below which to report; 0 = off */
    HANDLE thread;                          /* the sound thread, once it has called */
    volatile LONG64 last_qpc;               /* when it last called */
    volatile LONG calls;
    LONG test_at;                           /* the call at which to hold the thread; 0 = never */
    DWORD64 exe_lo, exe_hi;                 /* this executable's address range */
} s_snd_clock;


static void gb_snd_clock_start(uint32_t kernel_now)
{
    const char *e;

    for (;;) {
        LONG s = InterlockedCompareExchange(&s_snd_clock.state, 1, 0);
        if (s == 2) return;                 /* another thread finished it */
        if (s == 0) break;                  /* ours to do */
        YieldProcessor();
    }
    e = getenv("GB_AUDIO_TICK");
    s_snd_clock.fine = e && e[0] && e[0] != '0';
    QueryPerformanceFrequency(&s_snd_clock.freq);
    QueryPerformanceCounter(&s_snd_clock.t0);
    s_snd_clock.base = kernel_now;
    s_snd_clock.first_tid = GetCurrentThreadId();
    InterlockedExchange(&s_snd_clock.state, 2);
}


/* XAPI GetTickCount. Entered like the function itself: return address at
 * [esp], no arguments; the result goes in eax. */
void sub_001CAB6A(void)
{
    uint32_t kernel = GB_MEM32(GB_MEM32(GB_KTHUNK_KETICKCOUNT));
    LARGE_INTEGER t;
    int64_t ticks;
    uint32_t ret;

    if (s_snd_clock.state != 2)
        gb_snd_clock_start(kernel);
    QueryPerformanceCounter(&t);
    ticks = t.QuadPart - s_snd_clock.t0.QuadPart;
    /* One base, captured once: the counter cannot step back or jump. */
    ret = s_snd_clock.fine
        ? s_snd_clock.base + (uint32_t)(ticks * 1000 / s_snd_clock.freq.QuadPart)
        : kernel;
    g_eax = ret;
    g_esp += 4;                             /* ret */
}

extern void sub_001B2750_gen(void);         /* the title's stream set-up, as lifted */

void sub_001B2750(void)
{
    const char *e = getenv("GB_AUDIO_LEAD_MS");
    uint32_t ms = e && e[0] ? (uint32_t)strtoul(e, NULL, 10) : GB_AUDIO_LEAD_MS_DEFAULT;
    uint32_t rate, ring, lead, want, cap;

    sub_001B2750_gen();

    rate = GB_MEM32(GB_SND_RATE_VA) & 0xFFFFu;
    ring = GB_MEM32(GB_SND_RING_VA);
    lead = GB_MEM32(GB_SND_LEAD_VA);
    /* The writer adds the lead to a cursor rounded down to 16 frames and
     * rounds again, and DirectSound's write cursor is 32 frames past the
     * play cursor; 288 frames keeps the newest data clear of the cursor
     * coming round, with room to spare. */
    cap  = ring > 288u ? (ring - 288u) & ~15u : 0;
    want = (rate * ms / 1000u) & ~15u;
    if (want > cap)
        want = cap;
    if (rate < 8000u || rate > 48000u || lead == 0 || lead >= ring) {
        gb_log("[GB] sound lead: unexpected set-up (rate %u, ring %u, "
               "lead %u); left alone\n", rate, ring, lead);
    } else if (want > lead) {
        GB_MEM32(GB_SND_LEAD_VA) = want;
    }
}

/* ── The sound thread's sleep: a wait the port owns, and a guard ──
 *
 * The sound thread's loop (guest 0x001B2AF0) ends with Sleep(wait), wait
 * being the milliseconds to its next 10 ms slot. Sleep is guest 0x001CAD77;
 * through SleepEx (0x001CAD13) and the kernel's KeDelayExecutionThread it
 * reaches a plain Windows Sleep in the toolkit. About once in a hundred boots
 * that sleep does not come back for 16 to 17 seconds (asked for 9 ms; seen
 * with the 1 ms clock on and, once, with it off), and for that time the game
 * has no sound service. The cause, below the toolkit's sleep, is not known.
 *
 * This wrapper acts only on the call from that loop (return address
 * 0x001B2B56); every other caller goes to the lifted function untouched.
 *
 *   GB_AUDIO_WAIT=1   the wait is the port's: a high-resolution waitable
 *                     timer (it does not depend on the process's timer
 *                     resolution), an event the guard can set, and a timeout
 *                     20 ms past the request: three independent ways out.
 *   GB_AUDIO_WAIT=0   the title's own sleep, timed.
 *   GB_AUDIO_GUARD=1  a guard thread: when a wait has run 100 ms past its
 *                     request, set the event (which ends a port-owned wait);
 *                     at 200 ms let the thread run on any core until it is
 *                     back.
 */
/*
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable.
 */
#define GB_AUDIO_WAIT_DEFAULT  0
#define GB_AUDIO_GUARD_DEFAULT 0            /* part of the crackle fix, which is off by default */
#define GB_SND_SLEEP_RET       0x001B2B56u  /* the loop's return address from Sleep */

extern void sub_001CAD77_gen(void);         /* Sleep(ms): ms on the stack, removes 4 bytes */

static struct {
    volatile LONG state;                    /* 0 not started, 1 starting, 2 ready */
    int on, guard;
    HANDLE timer, kick, thread;
    DWORD tid;
    LARGE_INTEGER freq;
    volatile LONG64 wait_start;             /* when the wait in progress began; 0 = none */
    volatile LONG wait_ms;                  /* what it asked for */
    volatile LONG waits, by_timer, by_kick, by_timeout;
    volatile LONG late[6];                  /* late by under 1, 2, 5, 20, 100 ms, and more */
    volatile LONG kicks, widened, lines;
    double worst_ms;
    LONG test_at;
    volatile LONG widen_flag;               /* the guard widened the thread's cores */
    DWORD_PTR cores;                        /* what they were */
    volatile LONG64 last_edge;              /* when a wait last began or ended */
    volatile LONG hold_left;                /* passes left on any core */
    LONG hold;                              /* how many the last widening asked for */
    volatile LONG64 restored_at;            /* when the thread last went back to its core */
} s_snd_wait;


static DWORD WINAPI gb_snd_guard(LPVOID unused)
{
    LONG64 cur = 0;
    int stage = 0;

    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        LONG64 ws, edge;
        LARGE_INTEGER now;
        double over;
        LONG asked;

        Sleep(20);
        /* Watched from the last edge of a wait (its start or its end), so a
         * thread held between two waits is seen too: that is where the first
         * version of this guard lost it (see the hold below). */
        edge = s_snd_wait.last_edge;
        if (edge != cur) {
            cur = edge;
            stage = 0;
        }
        if (!edge)
            continue;
        ws = s_snd_wait.wait_start;
        asked = ws ? s_snd_wait.wait_ms : 0;
        QueryPerformanceCounter(&now);
        over = (double)(now.QuadPart - (ws ? ws : edge)) * 1000.0
             / (double)s_snd_wait.freq.QuadPart - (double)asked;
        if (stage == 0 && over > 100.0) {
            DWORD_PTR all = 0, sys_mask = 0;
            GROUP_AFFINITY ga;
            int widened = 0;

            /* Act first, report after. In every stall caught so far the
             * thread's wait had ended and Windows had it READY, but kept it
             * off the one core it is pinned to while the game's main thread
             * ran there. The event ends a wait the timer missed; letting the
             * thread run on any core ends the other case.
             *
             * The thread puts its own cores back, and only after it has run
             * a number of passes on any core (hold_left). Put back at once,
             * as the first version did, it pinned itself to the held core the
             * moment its wait ended and was stuck again outside any wait,
             * where that version did not look. If it has to be widened again
             * within 5 s of going back, the hold doubles (100 passes, about
             * a second, up to 6,400). */
            stage = 2;
            InterlockedIncrement(&s_snd_wait.kicks);
            SetEvent(s_snd_wait.kick);
            memset(&ga, 0, sizeof(ga));
            GetProcessAffinityMask(GetCurrentProcess(), &all, &sys_mask);
            if (all && !s_snd_wait.widen_flag
                    && GetThreadGroupAffinity(s_snd_wait.thread, &ga) && ga.Mask != all) {
                LONG64 back = s_snd_wait.restored_at;

                s_snd_wait.hold = back && now.QuadPart - back < 5 * s_snd_wait.freq.QuadPart
                                ? (s_snd_wait.hold < 3200 ? s_snd_wait.hold * 2 : 6400) : 100;
                s_snd_wait.cores = ga.Mask;
                InterlockedExchange(&s_snd_wait.hold_left, s_snd_wait.hold);
                InterlockedExchange(&s_snd_wait.widen_flag, 1);
                if (SetThreadAffinityMask(s_snd_wait.thread, all)) {
                    widened = 1;
                    InterlockedIncrement(&s_snd_wait.widened);
                } else {
                    InterlockedExchange(&s_snd_wait.widen_flag, 0);
                }
            }
            (void)widened;
        } else if (stage == 2 && over > 2000.0) {
            stage = 3;
        }
    }
    return 0;
}

static void gb_snd_wait_start(void)
{
    const char *e;

    for (;;) {
        LONG s = InterlockedCompareExchange(&s_snd_wait.state, 1, 0);
        if (s == 2) return;
        if (s == 0) break;
        YieldProcessor();
    }
    e = getenv("GB_AUDIO_WAIT");
    s_snd_wait.on = e && e[0] ? e[0] != '0' : GB_AUDIO_WAIT_DEFAULT;
    e = getenv("GB_AUDIO_GUARD");
    s_snd_wait.guard = e && e[0] ? e[0] != '0' : GB_AUDIO_GUARD_DEFAULT;
    QueryPerformanceFrequency(&s_snd_wait.freq);
    s_snd_wait.tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_snd_wait.thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (s_snd_wait.on) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        s_snd_wait.timer = CreateWaitableTimerExW(NULL, NULL,
                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!s_snd_wait.timer)              /* before Windows 10 1803 */
            s_snd_wait.timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        s_snd_wait.kick = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s_snd_wait.timer || !s_snd_wait.kick)
            s_snd_wait.on = 0;
    }
    if (!s_snd_wait.kick)
        s_snd_wait.kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s_snd_wait.guard)
        CreateThread(NULL, 0, gb_snd_guard, NULL, 0, NULL);
    InterlockedExchange(&s_snd_wait.state, 2);
}

static void gb_timer_watch(void);   /* below, with gb_KeSetTimerEx */

void sub_001CAD77(void)
{
    uint32_t ms;
    LARGE_INTEGER t0;
    const char *by = "the title's sleep";  /* for the working build's log */
    LONG n;

    if (GB_MEM32(g_esp) != GB_SND_SLEEP_RET) {
        sub_001CAD77_gen();
        return;
    }
    gb_timer_watch();           /* the title's timer service: see gb_KeSetTimerEx */
    if (s_snd_wait.state != 2)
        gb_snd_wait_start();
    ms = GB_MEM32(g_esp + 4);
    n = InterlockedIncrement(&s_snd_wait.waits);
    QueryPerformanceCounter(&t0);
    InterlockedExchange(&s_snd_wait.wait_ms, (LONG)ms);
    InterlockedExchange64(&s_snd_wait.wait_start, t0.QuadPart);
    InterlockedExchange64(&s_snd_wait.last_edge, t0.QuadPart);
    if (s_snd_wait.on && ms != 0u && ms < 60000u) {
        HANDLE h[2] = { s_snd_wait.timer, s_snd_wait.kick };
        LARGE_INTEGER due;
        DWORD limit = ms + 20u, r;

        {
            due.QuadPart = -(LONGLONG)ms * 10000ll;
        }
        ResetEvent(s_snd_wait.kick);
        SetWaitableTimer(s_snd_wait.timer, &due, 0, NULL, NULL, FALSE);
        r = WaitForMultipleObjects(2, h, FALSE, limit);
        if (r == WAIT_OBJECT_0) {
            InterlockedIncrement(&s_snd_wait.by_timer);
            by = "the timer";
        } else if (r == WAIT_OBJECT_0 + 1) {
            InterlockedIncrement(&s_snd_wait.by_kick);
            CancelWaitableTimer(s_snd_wait.timer);
            by = "the GUARD's event";
        } else {
            InterlockedIncrement(&s_snd_wait.by_timeout);
            CancelWaitableTimer(s_snd_wait.timer);
            by = "the TIMEOUT";
        }
        InterlockedExchange64(&s_snd_wait.wait_start, 0);
        g_eax = 0;
        g_esp += 8;                         /* ret 4 */
    } else {
        sub_001CAD77_gen();
        InterlockedExchange64(&s_snd_wait.wait_start, 0);
    }
    {
        LARGE_INTEGER te;

        QueryPerformanceCounter(&te);
        InterlockedExchange64(&s_snd_wait.last_edge, te.QuadPart);
        /* Widened by the guard: back to its own cores only after the passes
         * the guard asked for (see gb_snd_guard). */
        if (s_snd_wait.widen_flag && InterlockedDecrement(&s_snd_wait.hold_left) < 0) {
            /* Recorded before the cores go back: if the core is still held,
             * the thread stops right after SetThreadAffinityMask. */
            LONG ran = s_snd_wait.hold;     /* the guard may change it once the flag is clear */

            InterlockedExchange64(&s_snd_wait.restored_at, te.QuadPart);
            InterlockedExchange(&s_snd_wait.widen_flag, 0);
            SetThreadAffinityMask(GetCurrentThread(), s_snd_wait.cores);
            (void)ran;
        }
    }
    (void)by;
    (void)n;
}

extern void sub_0021C0D0_gen(void);         /* raise to DISPATCH_LEVEL if below it; object in ecx */
extern void sub_0021C103_gen(void);         /* lower again if that object raised */
extern void sub_0021E7FB_gen(void);         /* DPC: the chip's interrupt */
extern void sub_0021E67B_gen(void);         /* DPC: commands with a due time */
extern void sub_0021E9DB_gen(void);



/* DirectSound's raise (0x0021C0D0), lower (0x0021C103), the chip's interrupt
 * routine (0x0021E9DB) and its two DPCs (0x0021E7FB, 0x0021E67B): plain calls
 * to the lifted functions in a player's build. A working build gates and
 * traces them (above). */
void sub_0021C0D0(void)
{
    sub_0021C0D0_gen();
}

void sub_0021C103(void)
{
    sub_0021C103_gen();
}

void sub_0021E9DB(void)
{
    sub_0021E9DB_gen();
}

void sub_0021E7FB(void)
{
    sub_0021E7FB_gen();
}

void sub_0021E67B(void)
{
    sub_0021E67B_gen();
}


/* ── DirectSound: an "idle voice" report for a voice that is still playing ──
 *
 * When a voice finishes, the chip stops its front end ("traps") and raises
 * an interrupt. DirectSound's interrupt routine then reads two chip
 * registers to learn which voice: the method word (FEDECMETH, 0xFE801300)
 * and its parameter (FEDECPARAM, 0xFE801304), in guest 0x0021E822, and for
 * method 0x8000 calls the idle-voice handler, guest 0x0021E32F, with the
 * parameter as the voice number. The handler retires that voice: it unlinks
 * it from the chip's list of voices to play and marks its buffer finished.
 * It does not look at whether the chip agrees that the voice is idle. On a
 * console it has no need to: a trapped front end takes no further commands,
 * so the two registers hold until they have been read.
 *
 * In the toolkit every register write by a title thread goes through the
 * same front-end routine (fe_method, xboxrecomp/src/apu/apu_vp.c), which
 * stores the written method and value in those two registers first. The
 * title's sound thread asks for the play position of its first stream
 * buffer a hundred times a second, and that request begins with "select
 * voice 248". If the write lands between the interrupt routine's two reads,
 * the routine is told that voice 248 went idle. On a console the game's
 * threads cannot write the chip while that routine runs, so this cannot
 * happen there.
 *
 * The chip's own record settles it: it raises this trap only for a linked
 * voice whose active bit (bit 21 of the state word at +0x54 of its entry in
 * the voice table) is clear. A report naming a voice whose bit is set was
 * garbled on the way. This wrapper leaves such reports out. Nothing is lost
 * by that: the voice that really went idle is still linked and inactive, and
 * the chip traps for it again on its next frame.
 */
/*
 * Measured: without it the music and speech stream
 * stopped in 5 of 7 laps to a match, each time on a false report for voice
 * 248; with it, 5 laps to a match and no stop, two false reports ignored on
 * the way, and with false reports provoked on purpose 145 ignored in two
 * laps and no stop.
 *
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable.
 */
#define GB_IDLE_FILTER_DEFAULT 1
#define GB_VOICE_STATE_ACTIVE  0x00200000u  /* bit 21 of the voice's state word */

extern void sub_0021E32F_gen(void);         /* the idle-voice handler: driver in ecx, voice number on the stack */

static struct {
    int on;                                 /* -1 not read yet */
    volatile LONG calls, false_calls, ignored;
    int lines;
    int fake_voice, fake_after, fake_done;  /* 0 in a public build */
    int fake_any;                           /* ...,any: also a voice that is not active */
    volatile LONG no_object;                /* reports for a voice DirectSound has no object for */
} s_idle = { -1, 0, 0, 0, 0, -1, 28, 0, 0, 0 };


void sub_0021E32F(void)
{
    uint32_t drv = g_ecx;
    uint32_t v = GB_MEM32(g_esp + 4);
    uint32_t table = GB_MEM32(GB_DS_VOICES_PTR_VA);
    uint32_t state = 0;
    int is_false, no_object;

    if (s_idle.on < 0) {
        s_idle.on = GB_IDLE_FILTER_DEFAULT;
    }
    if (table && v < 256u)
        state = GB_MEM32(table + v * 0x80u + 0x54u);
    is_false = (state & GB_VOICE_STATE_ACTIVE) != 0;
    if (is_false && s_idle.on) {
        g_esp += 8;                         /* ret 4 */
        return;
    }
    /* A report for a voice DirectSound has no object for. The handler reads
     * through the object pointer without testing it; under the toolkit guest
     * address 0 reads as zeros, so for voice 0 its own "is this the object's
     * voice" test passes and it retires a null object (a crash, seen once,
     * at 0x0021F1FD). Such a report can only be a garbled one: the
     * chip traps for linked voices, and a linked voice has an object. */
    no_object = v < 256u && GB_MEM32(drv + 0x2C0u + v * 4u) == 0u;
    if (no_object) {
        if (s_idle.on) {
            g_esp += 8;                     /* ret 4 */
            return;
        }
    }
    sub_0021E32F_gen();
}

/* Stereo fold-down of the centre and LFE mix bins.
 *
 * The title gives every voice the six speaker bins (0 front left, 1 front
 * right, 2 centre, 3 LFE, 4 and 5 surround) and pans by their volumes; its
 * music is six mono streams, one per speaker, and menu sounds go to the
 * centre alone. On a console the audio chip's DSP folds those down for a
 * stereo output. The toolkit has no DSP: it adds the even bins into the left
 * channel and the odd bins into the right (mcpx_apu_dsp_frame in
 * xboxrecomp/src/apu/apu_dsp.c), so the centre is heard in the left ear only
 * and the LFE in the right only. Measured: menu sounds were -32.8 dB left
 * and silent right; with this, -35.8 dB on both.
 *
 * DirectSound packs a voice's eight volumes for the chip in one routine
 * (guest 0x0021EF0D; `this` in ecx, the output on the stack: three words for
 * each of up to three hardware voices). This wrapper lets it run, and where
 * the centre or the LFE is audible it rewrites the result so the same sound
 * reaches both sides: left = FL + SL + (C + LFE) x 0.707, right = FR + SR +
 * (C + LFE) x 0.707 (3 dB down each side, the usual fold-down; surrounds as
 * before). Because the toolkit only looks at whether a bin is even or odd,
 * the two sums go into the voice's own slots for bins 0 and 1 (and 4/5, 2/3
 * if a sum is above full level) and the other speaker slots are silenced.
 * Effect-send bins (6 and up) are left as they are. A voice with neither
 * centre nor LFE is not touched.
 *
 * Left alone: 3D voices (the chip overrides their first four bins), buffers
 * spread over several hardware voices, and stereo hardware voices (their
 * slots alternate between the two channels). None was seen in this title.
 */
/*
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable.
 */
#define GB_DOWNMIX_DEFAULT 1

extern void sub_0021EF0D_gen(void);         /* DirectSound: pack a voice's mix-bin volumes */

static float gb_vol_gain(uint32_t vol)      /* the chip's units: 0x40 a dB, FFF silent */
{
    vol &= 0xFFFu;
    return vol == 0xFFFu ? 0.0f : powf(10.0f, (float)vol / -1280.0f);
}

static uint32_t gb_gain_vol(float g)
{
    float v;

    if (g <= 0.0f)
        return 0xFFFu;
    if (g >= 1.0f)
        return 0;
    v = -1280.0f * log10f(g) + 0.5f;
    return v >= 4094.0f ? 0xFFFu : (uint32_t)v;
}

static void gb_fold_down_call(void)         /* entered as 0x0021EF0D itself */
{
    static int on = -1;
    uint32_t self = g_ecx;
    uint32_t out = GB_MEM32(g_esp + 4);
    uint32_t set, count, table, hw, a, b, c, k, n;
    uint32_t bin[8], vol[8];
    int slot[6];
    float g[6], mid, side[2];

    sub_0021EF0D_gen();

    if (on < 0) {
        on = GB_DOWNMIX_DEFAULT;
    }
    if (!on || !out)
        return;
    set = GB_MEM32(self + 0x78u);
    if (!set)
        return;
    count = GB_MEM32(set + 0x24u);
    hw = GB_MEM32(self + 0x0Cu) & 0xFFFFu;          /* the first hardware voice */
    table = GB_MEM32(GB_DS_VOICES_PTR_VA);
    if ((GB_MEM32(set + 0x08u) & 0x10u)             /* 3D */
            || (GB_MEM32(self + 0x64u) & 0xFFu) != 1u
            || count > 8u || hw > 255u || !table
            || (GB_MEM32(table + hw * 0x80u + 0x04u) & 0x08000000u)) {  /* stereo */
        return;
    }
    /* The slot order, as DirectSound's own routine (0x0021EE38) builds it:
     * the buffer's bins, then the lowest unused bin numbers up to eight. */
    for (n = 0; n < count; n++)
        bin[n] = GB_MEM32(set + 0x28u + n) & 0xFFu;
    for (c = 0; n < 8u; c++) {
        for (k = 0; k < n && bin[k] != c; k++)
            ;
        if (k == n)
            bin[n++] = c;
    }
    a = GB_MEM32(out);
    b = GB_MEM32(out + 0x0Cu);
    c = GB_MEM32(out + 0x18u);
    vol[0] = (a >> 4) & 0xFFFu;  vol[1] = a >> 20;
    vol[2] = (b >> 4) & 0xFFFu;  vol[3] = b >> 20;
    vol[4] = (c >> 4) & 0xFFFu;  vol[5] = c >> 20;
    vol[6] = ((c & 15u) << 8) | ((b & 15u) << 4) | (a & 15u);
    vol[7] = (((c >> 16) & 15u) << 8) | (((b >> 16) & 15u) << 4) | ((a >> 16) & 15u);
    for (k = 0; k < 6u; k++) {
        slot[k] = -1;
        g[k] = 0.0f;
    }
    for (k = 0; k < 8u; k++)
        if (bin[k] < 6u) {
            slot[bin[k]] = (int)k;
            g[bin[k]] = gb_vol_gain(vol[k]);
        }
    mid = g[2] + g[3];
    if (mid <= 0.0f || slot[0] < 0 || slot[1] < 0)
        return;
    side[0] = g[0] + g[4] + mid * 0.70710678f;
    side[1] = g[1] + g[5] + mid * 0.70710678f;
    for (k = 0; k < 2u; k++) {
        /* Slots of this side's parity, in the order they are filled. */
        static const uint8_t order[2][3] = { { 0, 4, 2 }, { 1, 5, 3 } };
        float left = side[k];

        for (n = 0; n < 3u; n++) {
            int s = slot[order[k][n]];
            float part = left > 1.0f ? 1.0f : left;

            if (s < 0)
                continue;
            vol[s] = gb_gain_vol(part);
            left -= part;
        }
    }
    a = (vol[0] << 4) | (vol[1] << 20) | (vol[6] & 15u) | ((vol[7] & 15u) << 16);
    b = (vol[2] << 4) | (vol[3] << 20) | ((vol[6] >> 4) & 15u) | (((vol[7] >> 4) & 15u) << 16);
    c = (vol[4] << 4) | (vol[5] << 20) | ((vol[6] >> 8) & 15u) | (((vol[7] >> 8) & 15u) << 16);
    GB_MEM32(out) = a;
    GB_MEM32(out + 0x0Cu) = b;
    GB_MEM32(out + 0x18u) = c;
}

/* Master volume and mono (the audio page), in the same place as the
 * fold-down: every volume DirectSound sends to the chip passes this routine,
 * and the toolkit has no volume of its own on the host output.
 *
 *   GB_AUDIO_VOLUME=0..100  master volume, percent of full level in
 *                           amplitude (50 is 6.0 dB down, 0 is silence).
 *                           Unset or 100: nothing is changed.
 *   GB_AUDIO_MONO=1         both ears get the same sound. Each voice is a
 *                           mono source with a left level (the sum of its
 *                           even-numbered bins) and a right level (the odd
 *                           ones: the toolkit's mix rule, effect sends
 *                           included); both sides are given their average,
 *                           so the result is (left + right) / 2 of the
 *                           stereo mix, which cannot clip.
 *
 * Mono leaves alone what the fold-down leaves alone (3D voices, buffers on
 * several hardware voices, stereo hardware voices; none seen in this title).
 * Master volume applies to every voice. */
static void gb_master_mono(uint32_t self, uint32_t out)
{
    static int ready, mono;
    static uint32_t att;                    /* added to every volume; 0xFFF: silence */
    uint32_t nv, k, i;

    if (!ready) {
        const char *e = getenv("GB_AUDIO_VOLUME");
        const char *m = getenv("GB_AUDIO_MONO");
        int pct = e && e[0] ? atoi(e) : 100;

        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        att = pct == 0 ? 0xFFFu
            : (uint32_t)(-1280.0 * log10((double)pct / 100.0) + 0.5);
        mono = m && m[0] && m[0] != '0';
        ready = 1;
    }
    if (!out || (!att && !mono))
        return;
    nv = GB_MEM32(self + 0x64u) & 0xFFu;
    if (nv > 3u)
        nv = 3u;
    for (k = 0; k < nv; k++) {
        uint32_t a = GB_MEM32(out + k * 4u);
        uint32_t b = GB_MEM32(out + 0x0Cu + k * 4u);
        uint32_t c = GB_MEM32(out + 0x18u + k * 4u);
        uint32_t vol[8];

        vol[0] = (a >> 4) & 0xFFFu;  vol[1] = a >> 20;
        vol[2] = (b >> 4) & 0xFFFu;  vol[3] = b >> 20;
        vol[4] = (c >> 4) & 0xFFFu;  vol[5] = c >> 20;
        vol[6] = ((c & 15u) << 8) | ((b & 15u) << 4) | (a & 15u);
        vol[7] = (((c >> 16) & 15u) << 8) | (((b >> 16) & 15u) << 4) | ((a >> 16) & 15u);
        if (mono) {
            uint32_t set = GB_MEM32(self + 0x78u);
            uint32_t count = set ? GB_MEM32(set + 0x24u) : 99u;
            uint32_t hw = GB_MEM32(self + 0x0Cu) & 0xFFFFu;
            uint32_t table = GB_MEM32(GB_DS_VOICES_PTR_VA);

            if (nv != 1u || count > 8u || hw > 255u || !table
                    || (GB_MEM32(set + 0x08u) & 0x10u)
                    || (GB_MEM32(table + hw * 0x80u + 0x04u) & 0x08000000u)) {
            } else {
                uint32_t bin[8], n, cc;
                float side[2] = { 0.0f, 0.0f }, mid;
                int to[2] = { -1, -1 };

                for (n = 0; n < count; n++)
                    bin[n] = GB_MEM32(set + 0x28u + n) & 0xFFu;
                for (cc = 0; n < 8u; cc++) {        /* as DirectSound fills the rest */
                    for (i = 0; i < n && bin[i] != cc; i++)
                        ;
                    if (i == n)
                        bin[n++] = cc;
                }
                for (i = 0; i < 8u; i++) {
                    side[bin[i] & 1u] += gb_vol_gain(vol[i]);
                    if (bin[i] < 2u)
                        to[bin[i]] = (int)i;        /* front left, front right */
                }
                for (i = 0; i < 8u; i++)            /* else the first slot of each parity */
                    if (to[bin[i] & 1u] < 0)
                        to[bin[i] & 1u] = (int)i;
                if (to[0] >= 0 && to[1] >= 0) {
                    uint32_t p;

                    mid = (side[0] + side[1]) * 0.5f;
                    /* One slot holds at most full level (the title sends its
                     * streams at full level to two bins of a side), so the
                     * average is spread over the slots of each parity, the
                     * front one first. */
                    for (p = 0; p < 2u; p++) {
                        float left = mid, part = left > 1.0f ? 1.0f : left;

                        vol[to[p]] = gb_gain_vol(part);
                        left -= part;
                        for (i = 0; i < 8u; i++) {
                            if ((bin[i] & 1u) != p || (int)i == to[p])
                                continue;
                            part = left > 1.0f ? 1.0f : left;
                            vol[i] = gb_gain_vol(part);
                            left -= part;
                        }
                    }
                }
            }
        }
        if (att)
            for (i = 0; i < 8u; i++)
                vol[i] = vol[i] + att >= 0xFFFu ? 0xFFFu : vol[i] + att;
        a = (vol[0] << 4) | (vol[1] << 20) | (vol[6] & 15u) | ((vol[7] & 15u) << 16);
        b = (vol[2] << 4) | (vol[3] << 20) | ((vol[6] >> 4) & 15u) | (((vol[7] >> 4) & 15u) << 16);
        c = (vol[4] << 4) | (vol[5] << 20) | ((vol[6] >> 8) & 15u) | (((vol[7] >> 8) & 15u) << 16);
        GB_MEM32(out + k * 4u) = a;
        GB_MEM32(out + 0x0Cu + k * 4u) = b;
        GB_MEM32(out + 0x18u + k * 4u) = c;
    }
}

void sub_0021EF0D(void)
{
    uint32_t self = g_ecx;
    uint32_t out = GB_MEM32(g_esp + 4);

    gb_fold_down_call();
    gb_master_mono(self, out);
}

/* Volume by sound category (the audio page).
 *
 * The title keeps a mix balance of eleven levels (0 to 100) in named tuning
 * variables and copies them once into a working table, in the audio
 * manager's constructor (guest 0x0007F840 calls 0x0007F760, its only
 * caller), before the start-up script runs. Every volume the game applies is
 * the player's own slider (effects, speech or music, 0 to 10, from the
 * Options screen or the save) times a word of this table. Nothing else
 * writes the table's volume words, so scaling them here lasts for the run
 * and stacks with the game's sliders.
 *
 *   GB_VOL_MUSIC=0..100       game music, menu music
 *   GB_VOL_SFX=0..100         game effects, menu effects, Gamebreaker
 *                             sounds, environment
 *   GB_VOL_CROWD=0..100       crowd (both kinds), random crowd reactions
 *   GB_VOL_PLAYERS=0..100     the players' speech
 *   GB_VOL_COMMENTARY=0..100  the commentator
 *   GB_COMMENTARY=0           the game's own "no commentary" switch (the
 *                             variable audio_no_commentary_speech, read each
 *                             time a line is chosen)
 *
 * Each is a percentage of the game's own level; unset or 100 changes
 * nothing. With none set the wrapper only calls the original.
 *
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable.
 */
extern void sub_0007F760_gen(void);

static const struct {
    uint32_t va;
    int group;                              /* index into s_vol_env */
    const char *what;
} s_vol_words[] = {
    { 0x002C7F14u, 0, "game music" },   { 0x002C7EE8u, 0, "menu music" },
    { 0x002C7EFCu, 1, "game effects" }, { 0x002C7F0Cu, 1, "menu effects" },
    { 0x002C7F08u, 1, "Gamebreaker" },  { 0x002C7EE4u, 1, "environment" },
    { 0x002C7EE0u, 2, "crowd" },        { 0x002C7EF4u, 2, "crowd stream" },
    { 0x002C7F10u, 2, "crowd reactions" },
    { 0x002C7F04u, 3, "player speech" },
    { 0x002C7EECu, 4, "commentary" },
};
static const char *const s_vol_env[5] = { "GB_VOL_MUSIC", "GB_VOL_SFX", "GB_VOL_CROWD",
                                          "GB_VOL_PLAYERS", "GB_VOL_COMMENTARY" };
#define GB_VOL_WORDS (sizeof(s_vol_words) / sizeof(s_vol_words[0]))

static uint32_t s_vol_set[GB_VOL_WORDS];    /* what the table held after the copy and our scaling */
static volatile LONG s_vol_latched;

/* The game's named tuning variables, set by name.
 *
 * The title registers 50 variables with its console at start-up (static
 * initialisers; constructors 0x00013150 on/off, 0x0007EC80 whole number,
 * 0x0007ECD0 fraction). Each is an object whose value is at +0x18: one byte
 * for on/off, a dword otherwise. The game's code reads the value straight
 * from the object, so writing it is all "setting" one takes; the console's
 * own "name value" command does the same and nothing more.
 *
 * WHEN: the game options below are set once, at the top of the wrapper of
 * 0x0007F760, which is inside the audio manager's constructor: after every
 * variable has been built, before the first frame, before the audio volumes
 * are copied (so the eleven volume variables work too), and before the
 * start-up script. The disc's cscripts\autoexec.cfg runs later and sets
 * demo_delay to 90: that one variable cannot be set from here.
 *
 * The table is the one place that knows the names and addresses (all 50 are
 * in it; produced from the game's registration calls in the disassembly).
 * Whether a variable is read every time it is used or copied once is noted
 * per row where it was checked: "live" was read in the listing; the volume
 * variables are copied once, right after this runs. */
static const struct {
    const char *name;
    uint32_t object;
    char kind;                              /* 'b' on/off byte, 'i' whole number, 'f' fraction */
} s_cvars[] = {
    { "blocks_on",                            0x002C2268u, 'b' },
    { "steals_on",                            0x002C229Cu, 'b' },
    { "def_switch_cycle",                     0x002C3AB4u, 'b' },
    { "drones_on",                            0x002C3FD8u, 'b' },
    { "debug_passoutOneTurbo",                0x002C44C8u, 'b' },
    { "debug_passoutOnGround",                0x002C44ECu, 'b' },
    { "audio_game_music_track",               0x002C7F90u, 'i' },   /* the game overwrites it */
    { "audio_dunk_dry_level",                 0x002C7FACu, 'i' },
    { "audio_no_sound",                       0x002C7FCCu, 'b' },   /* live, 47 places */
    { "audio_path_music_ducking",             0x002C7FE8u, 'i' },
    { "audio_FE_music_track",                 0x002C8004u, 'i' },   /* copied once */
    { "audio_dunk_master_fx_level",           0x002C8020u, 'i' },
    { "audio_aems_crowd_ducking",             0x002C803Cu, 'i' },
    { "audio_rotate_game_songs",              0x002C805Cu, 'b' },   /* live */
    { "audio_no_game_sfx",                    0x002C807Cu, 'b' },   /* live */
    { "audio_rucker_classic_track",           0x002C8098u, 'i' },
    { "au_no_player_speech",                  0x002C80B4u, 'b' },   /* live */
    { "audio_demo_cd",                        0x002C80D0u, 'b' },
    { "audio_speech_evlogs_on",               0x002C80ECu, 'b' },   /* never read */
    { "audio_no_pause_menu_music",            0x002C8108u, 'b' },   /* live */
    { "audio_internal_game_mus_vol",          0x002C8124u, 'i' },   /* the eleven volumes: copied once */
    { "audio_internal_crowd_ram_vol",         0x002C8140u, 'i' },
    { "audio_no_fe_music",                    0x002C8160u, 'b' },   /* never read */
    { "audio_aems_global_random_reac_vol",    0x002C817Cu, 'i' },
    { "audio_internal_game_sfx_vol",          0x002C8198u, 'i' },
    { "audio_song_change_on_gb",              0x002C81B4u, 'b' },   /* live */
    { "audio_internal_player_speech_vol",     0x002C81D0u, 'i' },
    { "audio_no_crowd",                       0x002C81ECu, 'b' },   /* live */
    { "audio_dunk_reverb_holdtime",           0x002C8208u, 'f' },
    { "audio_internal_fe_mus_vol",            0x002C8224u, 'i' },
    { "audio_internal_commentary_speech_vol", 0x002C8244u, 'i' },
    { "audio_gb_end_wait",                    0x002C8264u, 'f' },   /* never read */
    { "audio_aems_global_gamebreaker_vol",    0x002C8290u, 'i' },
    { "audio_path_offset",                    0x002C82ACu, 'i' },
    { "audio_aems_global_environment_vol",    0x002C82C8u, 'i' },
    { "audio_no_fe_sfx",                      0x002C82E4u, 'b' },   /* live */
    { "audio_aems_plyr_spch_ducking",         0x002C8304u, 'i' },
    { "audio_internal_fe_sfx_vol",            0x002C8320u, 'i' },
    { "audio_aems_sfx_ducking",               0x002C833Cu, 'i' },
    { "audio_internal_crowd_stream_vol",      0x002C8360u, 'i' },
    { "audio_no_commentary_speech",           0x002C837Cu, 'b' },   /* live */
    { "audio_dunk_fx_level",                  0x002C839Cu, 'i' },
    { "audio_no_game_music",                  0x002C83B8u, 'b' },   /* live, 23 places */
    { "create_team_add_generic",              0x002D9AB0u, 'b' },
    { "demo_delay",                           0x002DA758u, 'i' },   /* the disc's script sets it afterwards */
    { "g_repeatSpeed",                        0x002DB7C8u, 'i' },
    { "g_repeatSpeedMax",                     0x002DB7F4u, 'i' },
    { "g_repeatStep",                         0x002DB814u, 'i' },
    { "dual_shock_on",                        0x002DCD80u, 'b' },   /* live, 8 places */
    { "auto_replay",                          0x002E83E4u, 'b' },   /* live */
};

/* Sets one variable; returns 0 when the name is not in the table. */
static int gb_cvar_set(const char *name, size_t name_len, const char *value)
{
    size_t i;

    for (i = 0; i < sizeof(s_cvars) / sizeof(s_cvars[0]); i++) {
        uint32_t va = s_cvars[i].object + 0x18u;
        char *end = NULL;

        if (strlen(s_cvars[i].name) != name_len || memcmp(s_cvars[i].name, name, name_len))
            continue;
        if (s_cvars[i].kind == 'f') {
            float f = strtof(value, &end);
            uint32_t was = GB_MEM32(va);

            if (end == value) return -1;
            memcpy((void *)((uintptr_t)va + g_xbox_mem_offset), &f, sizeof(f));
            (void)was;
        } else {
            long v = strtol(value, &end, 10);

            if (end == value) return -1;
            if (s_cvars[i].kind == 'b') {
                volatile uint8_t *p = (volatile uint8_t *)((uintptr_t)va + g_xbox_mem_offset);

                *p = v != 0;
            } else {
                GB_MEM32(va) = (uint32_t)v;
            }
        }
        return 1;
    }
    return 0;
}


/* Game options: the player-facing settings that are one of the game's own
 * variables. Each switch, when set, writes its variable through
 * gb_cvar_set; unset leaves the game's own value. The game's code never writes any of these
 * variables, and its Options screen and save file keep their own settings
 * elsewhere, so nothing puts them back. Each is read every time it is used.
 *
 *   GB_RUMBLE=0         no controller vibration (dual_shock_on; every
 *                       rumble the game sends checks it; the game's Options
 *                       screen has no vibration item)
 *   GB_SONG_ROTATION=0  keep the same song from game to game
 *                       (audio_rotate_game_songs: at a new game the next
 *                       song in the player's EA Trax list is chosen)
 *   GB_AUTO_REPLAY=0    no automatic replays after big plays (auto_replay)
 *   GB_DEMO_DELAY=<s>   seconds on the title screen before the attract demo
 *                       (demo_delay, default 90; 0 = never). Written later,
 *                       see sub_00120790_enter: the disc's start-up script
 *                       sets this one after this point. */
static void gb_game_option(const char *env, const char *var)
{
    const char *e = getenv(env);

    if (!e || !e[0])
        return;
    if (gb_cvar_set(var, strlen(var), e[0] == '0' ? "0" : "1") <= 0)
        gb_log("[GB] game option %s: the game's variable %s was not found\n", env, var);
}

static void gb_game_options_apply(void)
{
    gb_game_option("GB_RUMBLE", "dual_shock_on");
    gb_game_option("GB_SONG_ROTATION", "audio_rotate_game_songs");
    gb_game_option("GB_AUTO_REPLAY", "auto_replay");
}

void sub_0007F760(void)
{
    int pct[5], any = 0, i;
    const char *e;
    e = getenv("GB_COMMENTARY");
    if (e && e[0] == '0') {
        gb_cvar_set("audio_no_commentary_speech", 26, "1");
    }
    gb_game_options_apply();

    sub_0007F760_gen();

    for (i = 0; i < 5; i++) {
        e = getenv(s_vol_env[i]);
        pct[i] = e && e[0] ? atoi(e) : 100;
        if (pct[i] < 0) pct[i] = 0;
        if (pct[i] > 100) pct[i] = 100;
        any |= pct[i] != 100;
    }
    for (i = 0; i < (int)GB_VOL_WORDS; i++) {
        uint32_t was = GB_MEM32(s_vol_words[i].va);
        uint32_t now = (was * (uint32_t)pct[s_vol_words[i].group] + 50u) / 100u;

        if (any) {
            GB_MEM32(s_vol_words[i].va) = now;
        }
        s_vol_set[i] = any ? now : was;
    }
    InterlockedExchange(&s_vol_latched, 1);
}


/* KeSetTimer with an absolute due time. The toolkit's bridge honours only
 * relative ones and fires an absolute one at once ("has not come up in
 * practice"). DirectSound's timer DPC arms its timer with the system time of
 * its next command, so on this toolkit it would run again on every pass of
 * the timer thread until the command is due. Counted here, to know whether
 * the title ever does it; the call itself is passed on unchanged. */
#define GB_KTHUNK_KESETTIMER 0x0022ADF4u    /* import slot, .rdata */

static void gb_KeSetTimer(void)
{
    recomp_func_t real = recomp_lookup_kernel(GB_MEM32(GB_KTHUNK_KESETTIMER));
    if (real)
        real();
}

/* KeSetTimerEx: a timer that fires while it is being set loses its signal.
 *
 * The title's XAPI timer service (the thread at guest 0x001CAE9D) keeps up
 * to 63 one-shot kernel timers and sleeps in KeWaitForMultipleObjects on all
 * of them. After each tick it re-arms the timer that fired, with the time
 * left to the next tick, or with 0 when it is already late. Its 100 Hz tick
 * wakes the title's stream worker (the thread under 0x001D88E0), which feeds
 * the movie player and, with it, the menus.
 *
 * The toolkit's KeSetTimerEx arms the timer in its table, leaves the table's
 * lock and only then resets the timer's host event. Its timer thread runs on
 * another core and fires a due timer by clearing the one-shot slot, setting
 * the event and writing 1 to the guest SignalState. If that lands between
 * the arming and the reset, the reset erases the firing: the slot is empty,
 * the event is clear, and the service thread waits for ever. The picture
 * goes on (the main thread draws on the vblank), the movie and the input
 * stop: the main-menu freeze. A busy core makes it likelier, because the
 * setting thread is then more often held between the two steps, and the
 * service re-arms with 0 more often when it runs late.
 *
 * The fix: clear SignalState before the call; if it reads 1 after the call,
 * a firing overlapped it. Most overlaps are harmless, so the case is told
 * apart (below); only when the reset erased the firing is the timer's event
 * set (the toolkit's KeSetEvent on a timer sets its host event), delivering
 * the tick that was due. Nothing else reads a timer's SignalState on this
 * toolkit (its waits on timers use the host event). Measured: one firing dropped on purpose
 * stops the menus every time with the fix off and never with it on. */
#define GB_KTHUNK_KESETTIMEREX  0x0022AD9Cu  /* import slot, .rdata */
#define GB_KTHUNK_KESETEVENT    0x0022ADA8u
#define GB_KTHUNK_KECANCELTIMER 0x0022ADB4u
#define GB_KTHUNK_KEWAITSINGLE  0x0022ADB0u

static int s_timer_fix = -1;
static volatile LONG s_timer_refired;

extern int xbox_kernel_cancel_timer(uint32_t timer_va);  /* toolkit: drops it, says if it was armed */

/* Call a kernel import from C on the current guest stack: arguments pushed
 * right to left, then a return address; the bridge pops both (stdcall). */
static void gb_kernel_call(uint32_t slot_va, const uint32_t *args, int n)
{
    recomp_func_t fn = recomp_lookup_kernel(GB_MEM32(slot_va));
    int i;

    if (!fn)
        return;
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        GB_MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    GB_MEM32(g_esp) = 0;
    fn();
}

/* Whether the timer's event is clear: a zero-timeout wait, which takes the
 * event if it is set (the caller then sets it again). */
static int gb_timer_event_clear(uint32_t timer)
{
    uint32_t args[5], to;

    g_esp -= 16;
    to = g_esp;
    GB_MEM32(to) = 0;
    GB_MEM32(to + 4) = 0;
    args[0] = timer;
    args[1] = 0;                /* Executive */
    args[2] = 0;                /* KernelMode */
    args[3] = 0;                /* not alertable */
    args[4] = to;               /* timeout 0 */
    gb_kernel_call(GB_KTHUNK_KEWAITSINGLE, args, 5);
    g_esp += 16;
    return g_eax == 0x102u;     /* STATUS_TIMEOUT */
}


/* The watch on the timer service. The fix above covers the one way of
 * losing a tick that has been seen. Anything else that leaves the service
 * asleep on its timer (a setting the toolkit dropped, a firing that never
 * comes, a wake-up that is never scheduled) has the same effect, so the
 * sound thread, which runs 100 times a second through the freeze, checks
 * that the service's last setting has been followed by a new one within
 * 300 ms of its due time. If not, it records the timer's state, which tells
 * those causes apart, and sets the timer's event to wake the service (with
 * the fix on). At worst one early tick, as above.
 *   event set     the firing came; the service was not woken or not run
 *   armed         the toolkit still holds the setting and has not fired it
 *   dead          neither: the firing or the setting was lost
 * The service's two KeSetTimerEx call sites, by return address: */
#define GB_XAPI_REARM_RET  0x001CB045u
#define GB_XAPI_CREATE_RET 0x001CB179u

static struct {
    CRITICAL_SECTION lock;
    volatile LONG init;
    uint32_t timer;
    LONGLONG armed_at, due_ticks, freq;   /* QueryPerformanceCounter */
    int handled;
    LONG woken;
} s_tw;

static void gb_timer_watch_init(void)
{
    if (InterlockedCompareExchange(&s_tw.init, 1, 0) == 0) {
        InitializeCriticalSection(&s_tw.lock);
        InterlockedExchange(&s_tw.init, 2);
    }
    while (s_tw.init != 2)
        Sleep(0);
}

static void gb_timer_watch_armed(uint32_t timer, uint32_t due_lo, uint32_t due_hi)
{
    LARGE_INTEGER now, f;
    long long due = (long long)(((uint64_t)due_hi << 32) | due_lo);
    long long ms = due < 0 ? -due / 10000 : 0;

    gb_timer_watch_init();
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    EnterCriticalSection(&s_tw.lock);
    s_tw.timer = timer;
    s_tw.armed_at = now.QuadPart;
    s_tw.freq = f.QuadPart;
    s_tw.due_ticks = (ms + 300) * f.QuadPart / 1000;
    s_tw.handled = 0;
    LeaveCriticalSection(&s_tw.lock);
}

static void gb_timer_watch(void)
{
    LARGE_INTEGER now;
    uint32_t timer;
    int sig, ev_set, armed;
    uint32_t ev_args[3];
    LONG n;

    if (s_tw.init != 2 || !s_tw.timer)
        return;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&s_tw.lock);
    if (s_tw.handled || now.QuadPart - s_tw.armed_at < s_tw.due_ticks) {
        LeaveCriticalSection(&s_tw.lock);
        return;
    }
    s_tw.handled = 1;
    timer = s_tw.timer;
    LeaveCriticalSection(&s_tw.lock);

    sig = GB_MEM32(timer + 4) != 0;
    ev_set = !gb_timer_event_clear(timer);   /* takes the event if it was set */
    armed = xbox_kernel_cancel_timer(timer);
    ev_args[0] = timer;
    ev_args[1] = 1;
    ev_args[2] = 0;
    if (s_timer_fix || ev_set) {
        gb_kernel_call(GB_KTHUNK_KESETEVENT, ev_args, 3);
    } else if (armed) {
        /* Fix off: the check took the setting away; give it back (due now). */
        uint32_t set_args[5] = { timer, 0, 0, 0, 0 };

        gb_kernel_call(GB_KTHUNK_KESETTIMEREX, set_args, 5);
    }
    n = InterlockedIncrement(&s_tw.woken);
    if (n <= 20)
        gb_log("[GB] timer service asleep: timer 0x%08X not set again for %lld ms; "
               "%s (SignalState %d)%s (%ld so far)\n", timer,
               (long long)((now.QuadPart - s_tw.armed_at) * 1000 / s_tw.freq),
               ev_set ? "event set" : armed ? "armed" : "dead", sig,
               s_timer_fix ? "; woken" : "; left", (long)n);
}

static void gb_KeSetTimerEx(void)
{
    recomp_func_t real = recomp_lookup_kernel(GB_MEM32(GB_KTHUNK_KESETTIMEREX));
    uint32_t timer = GB_MEM32(g_esp + 4);
    uint32_t ret_eax, set_args[5];
    int i;

    if (s_timer_fix < 0) {
        s_timer_fix = 1;
    }
    if (!real)
        return;
    if (!timer) {
        real();
        return;
    }
    for (i = 0; i < 5; i++)
        set_args[i] = GB_MEM32(g_esp + 4 + 4 * i);   /* timer, due (2), period, DPC */
    {
        uint32_t ret = GB_MEM32(g_esp);

        if (ret == GB_XAPI_REARM_RET || ret == GB_XAPI_CREATE_RET)
            gb_timer_watch_armed(timer, set_args[1], set_args[2]);
    }
    GB_MEM32(timer + 4) = 0;
    real();                     /* pops its arguments */
    ret_eax = g_eax;
    /* SignalState 1 means a firing overlapped the call, in one of three
     * ways. It came after the reset: the event is still set (the probe takes
     * it, so it is set again). It is the previous firing's late write (the
     * toolkit writes SignalState after waking the title's thread, which can
     * run first and set the timer again): the event is clear but the new
     * setting is still armed (taken off by the check, so it is set again,
     * and checked again). It came before the reset: the event is clear and
     * nothing is armed; that firing is lost. */
    for (i = 0; i < 4 && GB_MEM32(timer + 4); i++) {
        uint32_t ev_args[3];

        ev_args[0] = timer;
        ev_args[1] = 1;
        ev_args[2] = 0;
        if (!gb_timer_event_clear(timer)) {
            gb_kernel_call(GB_KTHUNK_KESETEVENT, ev_args, 3);
            break;
        }
        if (xbox_kernel_cancel_timer(timer)) {
            GB_MEM32(timer + 4) = 0;
            gb_kernel_call(GB_KTHUNK_KESETTIMEREX, set_args, 5);
            continue;
        }
        {
            LONG n = InterlockedIncrement(&s_timer_refired);

            if (n <= 50)
                gb_log("[GB] kernel timer 0x%08X: a firing was erased while the timer "
                       "was being set (%ld so far)%s\n", timer, (long)n,
                       s_timer_fix ? "; delivered again" : "; lost");
        }
        if (s_timer_fix)
            gb_kernel_call(GB_KTHUNK_KESETEVENT, ev_args, 3);
        break;
    }
    g_eax = ret_eax;
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    /* Kernel imports are called through synthetic addresses at 0xFE000000+,
     * read from the import table, so only those can match. */
    if (xbox_va >= 0xFE000000u
            && xbox_va == GB_MEM32(GB_KTHUNK_MMALLOC_CONTIG_EX))
        return gb_MmAllocateContiguousMemoryEx;
    if (xbox_va >= 0xFE000000u
            && xbox_va == GB_MEM32(GB_KTHUNK_KESETTIMER))
        return gb_KeSetTimer;
    if (xbox_va >= 0xFE000000u
            && xbox_va == GB_MEM32(GB_KTHUNK_KESETTIMEREX))
        return gb_KeSetTimerEx;
    if (xbox_va >= 0xFE000000u) {
        recomp_func_t m = gb_mods_lookup(xbox_va);   /* NULL with no mod on */
        if (m)
            return m;
    }

    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    static volatile LONG told;

    /* The first twenty in the log for bug reports: code the lifter did not
     * find (the bring-up loop in docs/BUILDING.md adds it). */
    if (InterlockedIncrement(&told) <= 20)
        gb_log("[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
               va, (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    gb_log("[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
           "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent:
 * the first ten addresses go in the log for bug reports. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    {
        /* Each address once (the same few run in a loop), ten at most. */
        static uint32_t seen[10];
        static volatile LONG used;
        LONG i, n = used;
        for (i = 0; i < n && i < 10; i++)
            if (seen[i] == va)
                return;
        n = InterlockedIncrement(&used);
        if (n > 10)
            return;
        seen[n - 1] = va;
        gb_log("[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X (a no-op)\n", text, va);
    }
}

/* Widescreen and ultrawide (GB_ASPECT, gb_d3d11.c "aspect").
 *
 * The game's 3D views all build their projection in one engine routine,
 * guest 0x0019FB80 (thiscall: the camera in ecx; field of view in degrees,
 * aspect, near, far on the stack; it also derives the culling planes from
 * them). The field of view is horizontal: with the same angle a wider aspect
 * shows the same width and less height (the console's own widescreen mode
 * does that, and crops). Wider view, GB_ASPECT=16:9 and wider: a call with the
 * 4:3 aspect is changed to the picture's aspect and a field of view widened
 * so that the vertical angle stays the 4:3 one,
 *     fov' = 2 atan(tan(fov / 2) x aspect / (4/3)),
 * which is what the game's own unused "wider view" case (0x001738B0, flag
 * +0x0D) does for 16:9. The culling planes follow. Calls with any other
 * aspect (the re-apply in 0x001A0090, which passes back the values already
 * changed here) are left alone.
 *
 * Menu panel views: the game's panel object (0x00173BE0 sets it up,
 * 0x00173950 re-sets its projection every frame; Create A Baller's player)
 * draws 3D into a rectangle of a menu, with that rectangle's width over height
 * as the aspect. The back end draws every target wider, so without help the
 * panel's 3D comes out a third too wide at 16:9 and off its place, while the
 * menu around it is kept at 4:3. The panel's views are told to the back end
 * (gb_d3d11_view_set), which then draws the menu as any other menu, and the
 * view is fitted to that: with 2D kept at 4:3 the panel's viewport
 * (0x001A0090, called from 0x00173BE0) is narrowed by (4/3) / aspect around
 * the centre, so after the wider target it lands on its 4:3 place at its own
 * shape; with 2D stretched (GB_ASPECT_2D=stretch or auto) the viewport
 * stretches with the menu, and the projection's aspect and field of view are
 * widened by the same factor, so the player keeps their shape and the panel
 * shows a little more at its sides.
 */
/*
 * Both need the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable. */
extern void sub_0019FB80_gen(void);         /* engine: perspective projection and culling planes */
extern void sub_001A0090_gen(void);         /* engine: a camera's viewport (x, y, w, h in 640x480 units, z range) */
extern void sub_001C94AC_gen(void);         /* XDK: XGetVideoFlags */
double gb_d3d11_aspect(void);
int gb_d3d11_aspect_mode(void);
int gb_d3d11_aspect_2d(void);
void gb_d3d11_view_set(int panel);

/* The panel object's projection calls (from 0x00173950 and 0x00173BE0) and
 * its viewport call (from 0x00173BE0); the re-apply inside 0x001A0090. */
#define GB_PANEL_PROJ_A 0x00173971u
#define GB_PANEL_PROJ_B 0x00173D4Cu
#define GB_PANEL_VIEWPORT 0x00173D2Bu
#define GB_PROJ_REAPPLY 0x001A03DCu

/* The panel fit is on whenever the wider view is. */
static int gb_panel_fit(void)
{
}



void sub_0019FB80(void)
{
    static volatile LONG seen;
    uint32_t from = GB_MEM32(g_esp);
    int panel = (from == GB_PANEL_PROJ_A || from == GB_PANEL_PROJ_B) && gb_panel_fit();

    if (gb_d3d11_aspect_mode() == 1 && from != GB_PROJ_REAPPLY)
        gb_d3d11_view_set(panel);
    if (panel) {
        /* A menu panel: with the menu stretched, widen its view by the same
         * factor (the viewport stretches with the menu); kept at 4:3, its
         * viewport is narrowed instead (sub_001A0090) and nothing changes
         * here. */
        double k = gb_d3d11_aspect() * 0.75;
        if (gb_d3d11_aspect_mode() == 1 && gb_d3d11_aspect_2d() != 0 && k > 1.0 + 1e-6) {
            uint32_t fb = GB_MEM32(g_esp + 4), ab = GB_MEM32(g_esp + 8);
            float fov, asp;
            memcpy(&fov, &fb, 4);
            memcpy(&asp, &ab, 4);
            if (fov > 0.0f && fov < 179.0f && asp > 0.0f) {
                double t = tan((double)fov * 3.14159265358979323846 / 360.0) * k;
                float nf = (float)(2.0 * atan(t) * 180.0 / 3.14159265358979323846);
                float na = (float)((double)asp * k);
                memcpy(&fb, &nf, 4);
                memcpy(&ab, &na, 4);
                GB_MEM32(g_esp + 4) = fb;
                GB_MEM32(g_esp + 8) = ab;
            }
        }
    } else if (GB_MEM32(g_esp + 8) == 0x3FAAAAABu && gb_d3d11_aspect_mode() == 1) {
        double a = gb_d3d11_aspect();
        uint32_t bits = GB_MEM32(g_esp + 4);
        float fov, af, nf;
        double t;

        memcpy(&fov, &bits, 4);
        if (a > 4.0 / 3.0 + 1e-6 && fov > 0.0f && fov < 179.0f) {
            t = tan((double)fov * 3.14159265358979323846 / 360.0) * a * 0.75;
            nf = (float)(2.0 * atan(t) * 180.0 / 3.14159265358979323846);
            af = (float)a;
            memcpy(&bits, &nf, 4);
            GB_MEM32(g_esp + 4) = bits;
            memcpy(&bits, &af, 4);
            GB_MEM32(g_esp + 8) = bits;
            InterlockedIncrement(&seen);
        }
    }
    sub_0019FB80_gen();
}

/* A camera's viewport (thiscall; x, y, w, h as floats in 640x480 units, then
 * the z range). The menu panel's, with 2D kept at 4:3 and a wide aspect: the
 * rectangle narrowed by (4/3) / aspect around the screen's centre, so that
 * once the back end has drawn the target wider it lands where the menu's 4:3
 * picture puts it, at its own shape (see the projection wrapper above). */
void sub_001A0090(void)
{
    if (GB_MEM32(g_esp) == GB_PANEL_VIEWPORT && gb_panel_fit() && gb_d3d11_aspect_mode() == 1
        && gb_d3d11_aspect_2d() == 0) {
        double k = gb_d3d11_aspect() * 0.75;
        uint32_t xb = GB_MEM32(g_esp + 4), wb = GB_MEM32(g_esp + 12);
        float x, w;
        memcpy(&x, &xb, 4);
        memcpy(&w, &wb, 4);
        if (k > 1.0 + 1e-6 && w > 0.0f) {
            float nx = (float)(320.0 + ((double)x - 320.0) / k), nw = (float)((double)w / k);
            memcpy(&xb, &nx, 4);
            memcpy(&wb, &nw, 4);
            GB_MEM32(g_esp + 4) = xb;
            GB_MEM32(g_esp + 12) = wb;
        }
    }
    sub_001A0090_gen();
}

void sub_001C94AC(void)
{

    sub_001C94AC_gen();
    /* Only to the game's options code (sub_000A21C0, both calls), which sets
     * the options bit the 3D view reads. Not to start-up (0x0016428A), which
     * would put the widescreen flag into Direct3D's CreateDevice: the device
     * then fails to come up on this toolkit and nothing is ever drawn
     * (measured). */
}

/* Game step rate (GB_GAME_HZ).
 *
 * The game advances one logic step per drawn frame and reads "steps a second"
 * from its display record (guest 0x002DDC54: +0x24 integer, +0x28 float) at
 * about 1,376 places, to turn seconds, miles an hour and 60-a-second content
 * into per-step units. The record's constructor, guest 0x00164710 (thiscall:
 * the record in ecx, returned in eax), writes the constant 60 into both.
 * About 530 of the readers run once at start-up and freeze their results, so
 * the only moment early enough to change the rate is inside the constructor:
 * the wrapper calls the original and then overwrites the two fields.
 *
 * GB_GAME_HZ=<20..240> selects the rate; unset, empty or 60 leaves the
 * original values bit for bit. The Direct3D 11 back end reads the same switch
 * for its frame limit (gb_d3d11.c), so the two cannot disagree.
 *
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable. */
extern void sub_00164710_gen(void);         /* game: display record constructor */

int gb_game_hz(void)
{
    static int hz = -1;
    if (hz < 0) {
        const char *e = getenv("GB_GAME_HZ");
        int n = (e && *e) ? atoi(e) : 60;
        hz = (n >= 20 && n <= 240) ? n : 60;
        if (e && *e && hz != n)
            gb_log("[GB] game rate: GB_GAME_HZ=%s is outside 20 to 240, using 60\n", e);
    }
    return hz;
}

void sub_00164710(void)
{
    int hz = gb_game_hz();
    uint32_t rec, bits;
    float f;

    sub_00164710_gen();
    rec = g_eax;
    if (hz == 60) {
        return;
    }
    f = (float)hz;
    memcpy(&bits, &f, 4);
    gb_log("[GB] game rate: %d steps a second (GB_GAME_HZ; record 0x%08X had %u / 0x%08X)\n",
           hz, rec, GB_MEM32(rec + 0x24), GB_MEM32(rec + 0x28));
    GB_MEM32(rec + 0x24) = (uint32_t)hz;
    GB_MEM32(rec + 0x28) = bits;
}

/* Scene player at other step rates (GB_GAME_HZ).
 *
 * The engine's scene player (cut scenes: the camera fly-by before tip-off,
 * the player introductions after it) counts integer frames of 60-a-second
 * content: its step, guest 0x00180420 (thiscall, no stack arguments), fires
 * the scene's events whose frame equals the current one (exact compare), sets
 * every object of the scene to that frame, then increments the frame
 * (+0x4C). It never asks the rate, so at 120 a scene played in half the time.
 *
 * The wrapper lets the real step run only when a sixtieth of a second has
 * built up: an accumulator per player gains 60 a call and the step runs each
 * time it reaches the rate (120: every other call; 90: two calls in three;
 * 50: sometimes twice in one call). Each frame number still comes exactly
 * once, so events fire as authored. The scene's start (guest 0x00180850,
 * returning to 0x0018090D) always runs, to pose frame 0, and resets the
 * accumulator. Between steps the objects keep their last pose, so above 60 a
 * scene moves 60 times a second.
 *
 * At 60 the wrapper only calls the original. */
extern void sub_00180420_gen(void);         /* engine: scene player, one frame */

static struct { uint32_t self; int acc; } s_scene[16];

/* Above 60 the scene's camera would move only on the steps where the scene
 * advances. The scene camera (the controller named at guest 0x002DD6AC, vtable
 * 0x0024CBDC) keeps its eye and target at +0x94 (six floats); its first
 * helper's +0x14 is the frame it was posed at. On the steps in between, the
 * camera is given the pose between its last two frames, as far along as the
 * time since the last frame (so it runs one sixtieth of a second behind the
 * scene, and moves evenly). At a cut (a frame that is not the next one) it
 * takes the new pose as it is. Before each real step the true pose is put
 * back, so the game's own code never reads a blended one. */
#define GB_SCENE_CAM_VT 0x0024CBDCu
static struct {
    uint32_t obj, owner, frame;
    int have;
    uint32_t prev[6], cur[6];
} s_scam;

static int gb_scene_cam(uint32_t *obj, uint32_t *frame)
{
    uint32_t o = GB_MEM32(0x002DD6ACu), h;
    if (o < 0x10000u || GB_MEM32(o) != GB_SCENE_CAM_VT)
        return 0;
    h = GB_MEM32(o + 0x1C0);
    if (h < 0x10000u)
        return 0;
    *obj = o;
    *frame = GB_MEM32(h + 0x14);
    return 1;
}

static void gb_scene_cam_after(uint32_t self, int acc, int hz)
{
    uint32_t o, f;
    int k;
    if (!gb_scene_cam(&o, &f)) {
        s_scam.obj = 0;
        return;
    }
    if (o != s_scam.obj) {
        s_scam.obj = o;
        s_scam.have = 0;
    }
    if (!s_scam.have || f != s_scam.frame) {   /* a new frame was posed */
        int next = s_scam.have && f == s_scam.frame + 1;
        for (k = 0; k < 6; k++) {
            s_scam.prev[k] = next ? s_scam.cur[k] : GB_MEM32(o + 0x94 + 4 * k);
            s_scam.cur[k] = GB_MEM32(o + 0x94 + 4 * k);
        }
        s_scam.frame = f;
        s_scam.owner = self;
        s_scam.have = 1;
    }
    if (self != s_scam.owner)
        return;
    for (k = 0; k < 6; k++) {               /* the pose acc / hz of a frame after prev */
        float a, b, v;
        uint32_t bits;
        memcpy(&a, &s_scam.prev[k], 4);
        memcpy(&b, &s_scam.cur[k], 4);
        v = a + (b - a) * (float)acc / (float)hz;
        memcpy(&bits, &v, 4);
        GB_MEM32(o + 0x94 + 4 * k) = bits;
    }
}

static void gb_scene_cam_before(void)
{
    uint32_t o, f;
    int k;
    if (s_scam.have && gb_scene_cam(&o, &f) && o == s_scam.obj)
        for (k = 0; k < 6; k++)
            GB_MEM32(o + 0x94 + 4 * k) = s_scam.cur[k];
}

static int gb_scene_smooth(void)
{
    static int on = -1;
    if (on < 0) {
        on = 1;
    }
    return on;
}

void sub_00180420(void)
{
    int hz = gb_game_hz(), i, runs, smooth;
    uint32_t self = g_ecx, ret = GB_MEM32(g_esp);

    if (hz == 60) {
        sub_00180420_gen();
        return;
    }
    for (i = 0; i < 16 && s_scene[i].self && s_scene[i].self != self; i++)
        ;
    if (i == 16)
        i = 15;                             /* more players than slots: share the last */
    if (s_scene[i].self != self) {
        s_scene[i].self = self;
        s_scene[i].acc = 0;
    }
    smooth = hz > 60 && gb_scene_smooth();
    if (ret == 0x0018090Du) {               /* the scene's start: frame 0 now */
        s_scene[i].acc = 0;
        if (smooth)
            gb_scene_cam_before();
        sub_00180420_gen();
        if (smooth)
            gb_scene_cam_after(self, 0, hz);
        return;
    }
    s_scene[i].acc += 60;
    runs = 0;
    while (s_scene[i].acc >= hz) {
        s_scene[i].acc -= hz;
        if (runs++) {                       /* again: as if called once more */
            g_esp -= 4;
            GB_MEM32(g_esp) = ret;
            g_ecx = self;
        } else if (smooth && (!s_scam.have || self == s_scam.owner)) {
            gb_scene_cam_before();
        }
        sub_00180420_gen();
    }
    if (!runs)
        g_esp += 4;                         /* not this call: plain ret */
    if (smooth)
        gb_scene_cam_after(self, s_scene[i].acc, hz);
}

/* Camera blend after the pre-game scene (GB_GAME_HZ).
 *
 * The blend camera's start, guest 0x00161780 (thiscall: from, to, steps,
 * flag; ret 0x10), mixes two cameras over a number of steps. Two of its three
 * callers pass half the step rate (half a second at any rate); the one in
 * 0x001606F0 (return address 0x0016078B), the blend from the pre-game scene
 * into the game camera, passes the constant 55, which is 0.92 s at 60 and
 * 0.46 s at 120. The wrapper scales that one to the rate: 55 * rate / 60.
 * At 60 it only calls the original. */
extern void sub_00161780_gen(void);         /* engine: camera blend, start */

void sub_00161780(void)
{
    int hz = gb_game_hz();
    if (hz != 60 && GB_MEM32(g_esp) == 0x0016078Bu) {
        int steps = (int)GB_MEM32(g_esp + 12);
        GB_MEM32(g_esp + 12) = (uint32_t)((steps * hz + 30) / 60);
    }
    sub_00161780_gen();
}

/* Replays above 60 steps a second (GB_GAME_HZ).
 *
 * The Replay Manager (a task; update at guest 0x00176650, thiscall, no stack
 * arguments) records one slot of a 300-slot ring each step and plays slots
 * back at "speed" slots a step, so the ring holds 5 s at 60 and only 2.5 s at
 * 120. The ring's size is fixed by the object's layout, so above 60 the whole
 * update runs 60 times a second, as it does on a console: recording, the
 * playback position, the start delay and the speed blend all keep their 60 a
 * second meaning. A request waiting to be taken (+0x24) runs the update at
 * once, so a replay starts and ends on the step it was asked for.
 *
 * What comes in from outside is in steps of the current rate, and is converted
 * where it enters:
 *   - the start, guest 0x001764E0 (thiscall: slots back, delay, id, speed;
 *     ret 0x10): slots back and delay become sixtieths of a second. Not for
 *     the manager's own delayed start (returns to 0x001766FB, already
 *     converted) or a scene's replay event (returns to 0x0017E993: the scene's
 *     data are sixtieths);
 *   - the slot count, guest 0x00175990, which its callers use to clamp a
 *     length in steps: it answers in steps;
 *   - two countdowns set from the rate (+0xC42EC while recording goes on after
 *     the event, +0xC42F0 during playback) are counted on the steps the update
 *     does not run, so they keep real time.
 *
 * On those steps a playing replay is also posed between its last slot and the
 * next one, by the game's own routine for fractional positions (0x001761A0,
 * which slow motion uses), so it moves on every step. The true position is put
 * back afterwards.
 *
 * One recorded channel (the play's moments: snapshot 0x001741D0, playback
 * 0x00174240) notes an event only if it happened on the very step being
 * recorded, by comparing step numbers. With a slot every other step that
 * would lose every second event, so its snapshot also takes events from the
 * steps since the last slot. Its playback slows the replay down at those
 * moments; it stays out of the poses between slots, so it runs as often as at
 * 60.
 *
 * At 60 and below the wrappers only call the originals. */
extern void sub_00176650_gen(void);         /* replay manager: one step */
extern void sub_001764E0_gen(void);         /* replay manager: start a replay */
extern void sub_00175990_gen(void);         /* replay manager: slots held */
extern void sub_001741D0_gen(void);         /* replay channel "moments": snapshot */
extern void sub_00174240_gen(void);         /* replay channel "moments": playback */
extern void sub_001761A0(void);             /* replay manager: pose the current position */

#define GB_RP_REQUEST   0x24u               /* state asked for, 0 = none */
#define GB_RP_STATE     0x1Cu               /* 1 record, 2 play, 3 hold, 4 record on */
#define GB_RP_POS       0x2Cu               /* float: ring position */
#define GB_RP_SPEED     0x34u               /* float: slots a step */
#define GB_RP_LEFT      0x58u               /* float: slots left to play */
#define GB_RP_RECORD_ON 0xC42ECu
#define GB_RP_PLAY_WAIT 0xC42F0u
#define GB_RP_PLAY_FLAG 0xC42F4u
#define GB_RP_WRITE     0xC42DCu            /* next slot to write */

static int s_replay_acc;
static int s_replay_between;                /* posing between two slots */

static int gb_replay_hz(void)
{
    int hz = gb_game_hz();
    return hz;
}

static int gb_replay_smooth(void)
{
    static int on = -1;
    if (on < 0) {
        on = 1;
    }
    return on;
}


void sub_00176650(void)
{
    int hz = gb_replay_hz();
    uint32_t self = g_ecx, st, bits;
    float speed, left, pos;

    s_replay_acc += 60;
    if (hz <= 60 || GB_MEM32(self + GB_RP_REQUEST) != 0 || s_replay_acc >= hz) {
        s_replay_acc = (hz > 60 && s_replay_acc >= hz) ? s_replay_acc - hz : 0;
        sub_00176650_gen();
        return;
    }

    /* A step the update does not run. */
    st = GB_MEM32(self + GB_RP_STATE);
    if (st == 4 && (int)GB_MEM32(self + GB_RP_RECORD_ON) > 1)
        GB_MEM32(self + GB_RP_RECORD_ON) -= 1;
    if (st == 2 && (GB_MEM32(self + GB_RP_PLAY_FLAG) & 0xFF) &&
        (int)GB_MEM32(self + GB_RP_PLAY_WAIT) > 1)
        GB_MEM32(self + GB_RP_PLAY_WAIT) -= 1;
    if (st == 2 && gb_replay_smooth()) {
        bits = GB_MEM32(self + GB_RP_SPEED);
        memcpy(&speed, &bits, 4);
        bits = GB_MEM32(self + GB_RP_LEFT);
        memcpy(&left, &bits, 4);
        if (speed > 0.0f && left > speed + 1.0f) {
            uint32_t keep = GB_MEM32(self + GB_RP_POS), ret = GB_MEM32(g_esp);
            memcpy(&pos, &keep, 4);
            pos += speed * (float)s_replay_acc / (float)hz;
            if (pos >= 300.0f)
                pos -= 300.0f;
            memcpy(&bits, &pos, 4);
            GB_MEM32(self + GB_RP_POS) = bits;
            g_esp -= 4;
            GB_MEM32(g_esp) = ret;
            g_ecx = self;
            s_replay_between = 1;
            sub_001761A0();
            s_replay_between = 0;
            GB_MEM32(self + GB_RP_POS) = keep;
        }
    }
    g_esp += 4;                             /* not this step: plain ret */
}

void sub_001764E0(void)
{
    int hz = gb_replay_hz();
    uint32_t ret = GB_MEM32(g_esp);

    if (hz > 60 && ret != 0x001766FBu && ret != 0x0017E993u) {
        int slots = (int)GB_MEM32(g_esp + 4), delay = (int)GB_MEM32(g_esp + 8);
        int s2 = (slots * 60 + hz / 2) / hz;
        int d2 = delay > 0 ? (delay * 60 + hz - 1) / hz : delay;
        GB_MEM32(g_esp + 4) = (uint32_t)s2;
        GB_MEM32(g_esp + 8) = (uint32_t)d2;
    }
    sub_001764E0_gen();
}

void sub_00175990(void)
{
    int hz = gb_replay_hz();
    sub_00175990_gen();
    if (hz > 60)
        g_eax = (uint32_t)((int)g_eax * hz / 60);
}

void sub_001741D0(void)
{
    static uint32_t last;                   /* the game step of the last slot */
    int hz = gb_replay_hz();
    uint32_t self = g_ecx, buf = GB_MEM32(g_esp + 4);

    sub_001741D0_gen();
    if (hz > 60) {
        uint32_t step = GB_MEM32(GB_MEM32(0x002C40E4u) + 0x10C);
        uint32_t obj = GB_MEM32(self + 8), flags = GB_MEM32(buf);
        static const struct { uint32_t off, bit; } ev[3] = { {0x18, 1}, {0x28, 2}, {0x20, 4} };
        int i;
        if (last && step > last)
            for (i = 0; i < 3; i++) {
                uint32_t at = GB_MEM32(obj + ev[i].off);
                if (at > last && at <= step)
                    flags |= ev[i].bit;
            }
        GB_MEM32(buf) = flags;
        last = step;
    }
}

void sub_00174240(void)
{
    if (s_replay_between) {
        g_esp += 4 + 0x10;                  /* ret 0x10 */
        return;
    }
    sub_00174240_gen();
}

/* Entry hook of the match's scoring listener. */
void sub_00174620_enter(void)
{
}

/* Frame drop, for step rates the PC cannot draw at.
 *
 * One game step is one drawn frame, and the frame limit paces the steps. When
 * drawing a frame takes longer than a step's slot (the toolkit runs every
 * vertex program on one CPU thread: a busy match view costs about 14 ms, a
 * 120-a-second slot is 8.3 ms), the steps come late and the whole game runs
 * in slow motion. With the frame drop on, the game's thread leaves out the
 * geometry of the next frame whenever the last flip finished late: that step
 * still runs, but hands the executor almost nothing, and the executor catches
 * up. Game speed stays real; pictures a second fall to what the PC can draw.
 *
 * How: Direct3D's two draw calls that carry the game's geometry,
 * DrawVertices (guest 0x001FF290) and DrawIndexedVertices (0x001FF330), both
 * (primitive, a, b) with the callee removing 12 bytes, return at once during
 * a dropped frame. Swap (0x00203C40) is the frame boundary: there the frame
 * just ended is recorded for the back end, which does not present it (clears
 * and immediate-mode 2D still ran, so it is a cleared, half-drawn picture),
 * and the decision for the next frame is taken.
 *
 * Rules, each against a way this could go wrong:
 *   - never two dropped frames in a row (at worst every other picture);
 *   - only when the last drawn frame was heavy (8000 triangles or more):
 *     menus, loading screens and the one-off renders to
 *     texture they make are never dropped;
 *   - only when the last flip was more than 250 thousandths of a period
 *     behind its slot;
 *   - swaps here and flips in the back end must stay one for one; if their
 *     counts drift apart the drop switches itself off for the run.
 *
 * On when GB_GAME_HZ is above 60 and the Direct3D 11 back end draws. With it
 * off (the default game) the three wrappers only call the originals.
 */
/*
 * Needs the lifter run with --exclude-manual on this file (port/regen.ps1
 * does). Addresses are for the pinned executable. */
extern void sub_00203C40_gen(void);         /* D3D: Swap */
extern void sub_001FF290_gen(void);         /* D3D: DrawVertices */
extern void sub_001FF330_gen(void);         /* D3D: DrawIndexedVertices */
int gb_d3d11_flip_late(void);
int gb_d3d11_flip_count(void);
int gb_d3d11_flip_tris(void);
void gb_d3d11_mark_swap(unsigned swap_index, int dropped);

static struct {
    int on;                                 /* -1 not looked yet */
    int late_min, tris_min;
    int drop_now;                           /* the frame being built is dropped */
    unsigned swaps;
    unsigned dropped, calls_skipped;
    int ahead_min, ahead_max;
} s_drop = { -1, 250, 8000, 0, 0, 0, 0, 1 << 30, -(1 << 30) };

static int gb_drop_on(void)
{
    if (s_drop.on < 0) {
        const char *d = getenv("GB_D3D11");
        int on = gb_game_hz() > 60;

        if (!(d && *d && *d != '0'))
            on = 0;                         /* the CPU rasteriser has no flip count to follow */
        s_drop.on = on;
        if (on)
            gb_log("[GB] frame drop: on (a frame's geometry is left out when the last flip was "
                            "over %d thousandths of a period late and the last frame had %d triangles or more)\n",
                    s_drop.late_min, s_drop.tris_min);
    }
    return s_drop.on;
}

void sub_00203C40(void)
{
    int ahead;

    if (!gb_drop_on()) {
        sub_00203C40_gen();
        return;
    }
    /* Record the frame that ends here before its flip can be reached. */
    gb_d3d11_mark_swap(s_drop.swaps, s_drop.drop_now);
    ahead = (int)(s_drop.swaps - (unsigned)gb_d3d11_flip_count());
    s_drop.swaps++;
    sub_00203C40_gen();

    if (ahead < s_drop.ahead_min) s_drop.ahead_min = ahead;
    if (ahead > s_drop.ahead_max) s_drop.ahead_max = ahead;
    if (ahead < -1 || ahead > 16) {
        gb_log("[GB] frame drop: swaps and flips no longer match (swap %u is %d ahead of the "
                        "flips); switched off for this run\n", s_drop.swaps, ahead);
        s_drop.on = 0;
        s_drop.drop_now = 0;
        return;
    }
    if (s_drop.drop_now) {
        s_drop.dropped++;
        s_drop.drop_now = 0;                /* never two in a row */
    } else if (gb_d3d11_flip_late() > s_drop.late_min && gb_d3d11_flip_tris() >= s_drop.tris_min) {
        s_drop.drop_now = 1;
    }
}

void sub_001FF290(void)
{
    if (s_drop.drop_now) {
        s_drop.calls_skipped++;
        g_esp += 4 + 12;                    /* ret 0xC */
        return;
    }
    sub_001FF290_gen();
}

void sub_001FF330(void)
{
    if (s_drop.drop_now) {
        s_drop.calls_skipped++;
        g_esp += 4 + 12;                    /* ret 0xC */
        return;
    }
    sub_001FF330_gen();
}

/* ── Saves: trace, write guard, the game's fatal error ──────
 *
 * The title's save code (addresses for the pinned
 * executable). Every function here is a method, `this` in ecx:
 *
 *   0x000D8D00  constructor of a save task (an object of about 50 KB)
 *   0x000D8F50  its state machine, two stack arguments (state, data)
 *   0x000D82F0  open the save (XCreateSaveGame, OPEN_EXISTING)
 *   0x000D8390  create it (OPEN_ALWAYS)
 *   0x000D78F0  write: six blocks, descriptors 0x14 apart from this+0xBB18
 *               (size at +4, source at +8), the file path at this+0xC1F4;
 *               signs, writes, checks no result
 *   0x000D7AE0  read the whole file (al = 1 when it worked)
 *   0x000D7B80  check the signature and every block's size (al = 1 when good)
 *
 * A read or check that fails is logged. The write runs inside gb_save_write_begin/end, so a
 * window closed during it waits for it (gb_save.c).
 *
 * The fatal error: EA's routine 0x001A6D20 formats its message into a
 * 512-byte buffer on its own stack, prints it, and tail-jumps to the exit
 * routine 0x001A6FF0 with its stack pointer back where it entered, so the
 * text is still at esp - 0x200 there. The exit routine is also how a normal
 * quit leaves (it ends in XLaunchNewImage, ExitProcess(0) in the toolkit).
 */
#include "gb_save.h"

extern void sub_000D78F0_gen(void);
extern void sub_000D7AE0_gen(void);
extern void sub_000D7B80_gen(void);
extern void sub_000D82F0_gen(void);
extern void sub_000D8390_gen(void);

#define GB_SAVE_DESC   0xBB18u          /* first block descriptor in the save task */
#define GB_SAVE_PATH   0xC1F4u          /* the save file's path (8-bit text) */
#define GB_FATAL_FILE  0x002EC4E8u      /* EA's fatal routine: source file name, line */
#define GB_FATAL_LINE  0x002EC4ECu


/* Guest 8-bit text into a host buffer. Save tasks live in contiguous memory,
 * which the title addresses at 0x80000000 and up; the toolkit maps the guest
 * range below 0xFD000000 (the limit its lifted string copies check). */
static void gb_guest_str(uint32_t va, char *out, size_t n)
{
    size_t i = 0;
    if (va >= 0x00010000u && va < 0xFD000000u - 0x1000u)
        for (; i + 1 < n; i++) {
            char c = *(const char *)((uintptr_t)(va + i) + g_xbox_mem_offset);
            if (!c)
                break;
            out[i] = (c >= 32 && c < 127) ? c : '?';
        }
    out[i] = 0;
}

void sub_000D8D00_enter(void)
{
}

void sub_000D8F50_enter(void)
{
}

/* Open and create return nothing: they leave a flag at this+0xC314 (1 = the
 * save folder exists) and a status at this+0xC318 (2 found, 1 not), and the
 * file's path at this+0xC1F4. Read and check return their answer in al. */
static void gb_save_result(const char *what, void (*gen)(void), int in_object)
{
    uint32_t self = g_ecx;
    char path[264];
    int ok;
    gen();
    gb_guest_str(self + GB_SAVE_PATH, path, sizeof path);
    if (in_object) {
    } else {
        ok = (g_eax & 0xFFu) != 0;
        if (!ok)
            gb_log("[SAVE] %s: NO %s\n", what, path);
    }
}

void sub_000D82F0(void) { gb_save_result("open the save", sub_000D82F0_gen, 1); }
void sub_000D8390(void) { gb_save_result("create the save", sub_000D8390_gen, 1); }
void sub_000D7AE0(void) { gb_save_result("read the save file", sub_000D7AE0_gen, 0); }
void sub_000D7B80(void) { gb_save_result("save file signed and sized right", sub_000D7B80_gen, 0); }

void sub_000D78F0(void)
{
    gb_save_write_begin();
    sub_000D78F0_gen();
    gb_save_write_end();
}

/* XAPI's WriteFile (guest 0x001C9DA2). The save writer calls it twice: the
 * 20-byte signature, then the data. */
void sub_001C9DA2_enter(void)
{
}

static volatile LONG s_fatal;

void sub_001A6D20_enter(void)
{
    InterlockedExchange(&s_fatal, 1);
}

void sub_001A6FF0_enter(void)
{
    if (s_fatal) {
        char text[520], file[200], msg[800];
        uint32_t fp = GB_MEM32(GB_FATAL_FILE);
        gb_guest_str(g_esp - 0x200u, text, sizeof text);
        file[0] = 0;
        if (fp)
            gb_guest_str(fp, file, sizeof file);
        if (file[0])
            snprintf(msg, sizeof msg, "%s\n(%s, line %u)", text, file, GB_MEM32(GB_FATAL_LINE));
        else
            snprintf(msg, sizeof msg, "%s", text);
        gb_fatal_report(msg);
    } else {
        gb_log("[GB] the game is quitting (its exit routine)\n");
        gb_save_before_exit("quit");
    }
    fflush(stderr);
}

/* ── Pause when the window goes to the background ─────────────
 *
 * When the game's window loses keyboard focus during a match, the port asks
 * the game to pause exactly as a START press does, and does nothing else: no
 * thread is held, so the sound and the renderer keep their own timing. When
 * focus returns the pause menu stays up until the player resumes.
 *
 * How the game pauses (pinned executable):
 *   - A START press in a match writes the pressing controller's number into
 *     a request word, match + 0xDA (16 bits; -1 when nothing is pending;
 *     -4 to -2 is the end-of-game countdown).
 *   - A task named "game pause" (its pointer at match + 0x3FC, its owner,
 *     the match, at task + 0x18) is updated every game step by
 *     sub_00036680. When the pause layer is hidden it takes a request and
 *     opens the pause menu for that controller (sub_00156BA0), then sets the
 *     word back to -1.
 *   - The pause layer is the task whose pointer is at 0x002DC854; its byte
 *     +0x10 is 1 while it is suspended (no pause menu or end-of-game screen).
 *   - The controllers of the players on court: six dwords at
 *     [[0x002DCF14] + 0x10] + 0x18. sub_00156BA0 opens the normal pause menu
 *     only for a controller found there.
 *
 * This entry hook runs at the top of that update, on the game's thread, so
 * it acts only in a match and in the same step as the game's own check.
 * Each step while the window is in the background and no request has been
 * made for this loss of focus: if the layer is hidden, nothing is pending
 * and a human player is on court, write that player's controller number.
 * Otherwise (a replay, the end-of-game countdown, a demo with no human
 * player) try again next step; once focus returns, stop trying.
 *
 * The window must have had focus once in this run (gb_d3d11.c), so scripted
 * runs, whose window never takes focus, never pause.
 *
 * GB_PAUSE_BACKGROUND=0 turns it off; it is on by default. */
#define GB_PAUSE_LAYER_PTR  0x002DC854u
#define GB_PAUSE_PLAYERS    0x002DCF14u

#define GB_MEM16(va) \
    (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

int gb_window_focus_lost(void);            /* gb_d3d11.c */


static int gb_pause_lost(ULONGLONG since_first)
{
    (void)since_first;
    return gb_window_focus_lost();
}

void sub_00036680_enter(void)
{
    static int on = -1, done;
    static ULONGLONG first, wait_since;
    uint32_t task = g_ecx, match, layer, players, i, ctl = 0xFFFFFFFFu;
    ULONGLONG now = GetTickCount64();

    if (on < 0) {
        const char *e = getenv("GB_PAUSE_BACKGROUND");
        on = !(e && e[0] == '0');
        first = now;
    }
    if (!on)
        return;
    if (!gb_pause_lost(now - first)) {
        done = 0;
        wait_since = 0;
        return;
    }
    if (done)
        return;
    if (!wait_since)
        wait_since = now;

    match = GB_MEM32(task + 0x18);
    layer = GB_MEM32(GB_PAUSE_LAYER_PTR);
    if (!match || !layer)
        return;
    if (!GB_MEM8(layer + 0x10)) {
        done = 1;                           /* a pause menu or end screen is already up */
        return;
    }
    if (GB_MEM16(match + 0xDA) == 0xFFFFu) {
        players = GB_MEM32(GB_PAUSE_PLAYERS);
        players = players ? GB_MEM32(players + 0x10) : 0;
        for (i = 0; players && i < 6; i++) {
            uint32_t c = GB_MEM32(players + 0x18 + 4u * i);
            if (c <= 3) { ctl = c; break; }
        }
    }
    if (ctl == 0xFFFFFFFFu) {
        return;
    }
    GB_MEM16(match + 0xDA) = (uint16_t)ctl;
    done = 1;
}

/* ── Attract-mode delay (GB_DEMO_DELAY) ───────────────────────
 *
 * demo_delay (seconds on the title screen before the attract demo) is set
 * by the disc's start-up script after gb_game_options_apply runs, so it is
 * written here instead, at the top of the one routine that reads it: the
 * title screen arming its timer (sub_00120790, value at 0x002DA770, ticks =
 * seconds x 0x002EC514, the rate of the game's wall-clock tick counter, 100
 * a second: real time at any step rate). 0 means never: written as
 * 1,000,000 s, which stays under 2^31 ticks. */
void sub_00120790_enter(void)
{
    static long v = -2;

    if (v == -2) {
        const char *e = getenv("GB_DEMO_DELAY");
        v = (e && e[0]) ? strtol(e, NULL, 10) : -1;
        if (v == 0 || v > 1000000)
            v = 1000000;
    }
    if (v > 0)
        GB_MEM32(0x002DA770u) = (uint32_t)v;
}

/* ── The game's own cheats, without entering their codes (GB_CHEATS) ──────
 *
 * The game takes twelve cheat codes on its User Setup screen, once every
 * player is ready ("Enter cheat codes now."): a held button, then four face
 * buttons. Its checker (0x000E5010) packs the last four presses into one
 * word and, on a match, writes a value into a 0x90-byte settings record at
 * 0x002C3B50 (its accessor is 0x0002DB60, which zeroes the record on first
 * use and sets bit 0 of 0x002C3BE0). The rest of the game reads those
 * fields: during play (unlimited turbo, no counters, trails, no HUD,
 * exploding rims) or when players and the ball are set up after User Setup
 * (quicks, two-pointers, body and head size, the ball). The routine
 * 0x001206E0 zeroes all the cheat fields; the game calls it when the title
 * screen is set up and when the players back out of User Setup. The game
 * refuses codes in mode 1 (Street School): the checker returns at once when
 * the mode, profile + 0x78 = 0x002C8D48, is 1.
 *
 *   GB_CHEATS=<names, separated by commas>   unset: off, the game as it is
 *   GB_CHEATS=all                            all twelve (for the two pairs
 *                                            that share a field, the later
 *                                            in the table below wins)
 *
 * This entry hook runs at the top of User Setup's per-frame update
 * (0x0011CBD0, the screen object in ecx), on the game's thread, so the
 * fields are written exactly where a player's codes would be: on that
 * screen, before teams, players and the ball are set up. Each frame a chosen
 * field that holds 0 gets the chosen value, so a code a player enters by
 * hand is never overwritten, and the values come back after the game's own
 * reset. In Street School nothing is written, and fields still holding a
 * value this switch wrote are cleared, so no cheat reaches a lesson. Unknown
 * names are ignored.
 *
 * Not reproduced: in mode 4 the game turns the alternate ball's value 2 into
 * 1 for some created teams (0x000E5159 to 0x000E51F2); this switch writes 2.
 *
 * "Unlock everything" (GB_UNLOCK_ALL=1): every reward of the game's Rewards
 * screen unlocked. The profile (a static at 0x002C8CD0) keeps one bit per
 * reward id, ids 0 to 0x70, in four words at profile + 0x20 (0x002C8CF0),
 * part of the first block of the save (profile + 0x18, 96 bytes). The game's
 * own grant (0x000A59D0) also sets bit n of profile + 0x34 (0x002C8D04) for
 * the n-th of the 19 ids in its table at 0x0023A000 (ids 0x2E to 0x40, so
 * bits 0 to 18). The entry hook on the game's "is this reward unlocked" test
 * (0x000A5980, profile in ecx) sets all those bits before each test, so
 * whatever the boot-time load has put there, every reward reads as unlocked.
 * The game's next save (Autosave on leaving Options, or Game Data) writes
 * them to the file. Not done while the game is writing its save. Not
 * reproduced: one grant side effect that sets an option bit for one reward
 * (0x000A5A6C to 0x000A5A9C), which needs the rewards table's types; reward
 * points are left alone. gb_save.c copies the save folder to
 * backups\<date_time>_before-unlock\ at start, before anything changes, so
 * restoring that copy undoes it.
 */
#define GB_CHEAT_RECORD       0x002C3B50u   /* the settings record */
#define GB_CHEAT_RECORD_INIT  0x002C3BE0u   /* bit 0: the record is set up */
#define GB_PROFILE            0x002C8CD0u
#define GB_PROFILE_READY      0x002C8C60u   /* 1 once the profile is constructed */
#define GB_GAME_MODE          (GB_PROFILE + 0x78u)
#define GB_MODE_STREET_SCHOOL 1u

static const struct gb_cheat {
    const char *name;
    uint8_t off[2];                         /* record offsets; a value of 0 = unused */
    uint8_t val[2];
} gb_cheats[] = {
    { "street_kids",     { 0x34, 0 },    { 1, 0 } },
    { "no_counters",     { 0x7C, 0 },    { 1, 0 } },
    { "gb_trails",       { 0x1C, 0 },    { 1, 0 } },
    { "all_quicks",      { 0x44, 0 },    { 2, 0 } },
    { "unlimited_turbo", { 0x70, 0 },    { 1, 0 } },
    { "easy_2_pointers", { 0x50, 0 },    { 2, 0 } },
    { "hard_2_pointers", { 0x50, 0 },    { 1, 0 } },
    { "big_heads",       { 0x38, 0 },    { 2, 0 } },
    { "wnba_ball",       { 0x04, 0x14 }, { 1, 3 } },
    { "alternate_ball",  { 0x14, 0 },    { 2, 0 } },
    { "no_hud",          { 0x2C, 0 },    { 1, 0 } },
    { "explosive_rims",  { 0x18, 0 },    { 1, 0 } },
};
#define GB_CHEAT_COUNT ((int)(sizeof gb_cheats / sizeof gb_cheats[0]))
#define GB_CHEAT_WORDS (0x90 / 4)

/* The value wanted at each record word, 0 where nothing is wanted. */
static uint32_t s_cheat_want[GB_CHEAT_WORDS];
static int s_cheats_on = -1;


/* Compare n characters of s with a table name: case, '-' and ' ' ignored. */
static int gb_cheat_name_is(const char *s, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n && name[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c == '-' || c == ' ')
            c = '_';
        if (c != name[i])
            return 0;
    }
    return i == n && !name[i];
}

static void gb_cheats_read(void)
{
    const char *e = getenv("GB_CHEATS"), *p;
    int chosen[GB_CHEAT_COUNT * 2], n = 0;

    s_cheats_on = 0;
    if (!e || !e[0])
        return;
    for (p = e; *p; ) {
        const char *q = p;
        size_t len;
        int found = 0;
        while (*q && *q != ',' && *q != ';')
            q++;
        len = (size_t)(q - p);
        while (len && *p == ' ') { p++; len--; }
        while (len && p[len - 1] == ' ') len--;
        if (len && gb_cheat_name_is(p, len, "all")) {
            for (int k = 0; k < GB_CHEAT_COUNT && n < GB_CHEAT_COUNT * 2; k++)
                chosen[n++] = k;
            found = 1;
        }
        for (int k = 0; len && !found && k < GB_CHEAT_COUNT; k++)
            if (gb_cheat_name_is(p, len, gb_cheats[k].name)) {
                if (n < GB_CHEAT_COUNT * 2)
                    chosen[n++] = k;
                found = 1;
            }
        p = *q ? q + 1 : q;
    }
    for (int i = 0; i < n; i++) {
        const struct gb_cheat *c = &gb_cheats[chosen[i]];
        for (int j = 0; j < 2; j++)
            if (c->val[j])
                s_cheat_want[c->off[j] / 4] = c->val[j];   /* a later name wins a shared field */
    }
    for (int i = 0; i < GB_CHEAT_WORDS; i++)
        if (s_cheat_want[i])
            s_cheats_on = 1;
}

void sub_0011CBD0_enter(void)
{
    uint32_t mode;

    if (s_cheats_on < 0)
        gb_cheats_read();
    if (!s_cheats_on)
        return;
    if (!(GB_MEM8(GB_CHEAT_RECORD_INIT) & 1u) || !GB_MEM8(GB_PROFILE_READY))
        return;                             /* not set up yet: its first use would zero it */
    mode = GB_MEM32(GB_GAME_MODE);
    for (int i = 1; i < GB_CHEAT_WORDS; i++) {  /* +0x00 is the record's table pointer */
        uint32_t want = s_cheat_want[i], va = GB_CHEAT_RECORD + 4u * (uint32_t)i;
        if (!want)
            continue;
        if (mode == GB_MODE_STREET_SCHOOL) {
            if (GB_MEM32(va) == want)
                GB_MEM32(va) = 0;
        } else if (GB_MEM32(va) == 0) {
            GB_MEM32(va) = want;
        }
    }
}

void sub_000A5980_enter(void)
{
    static int on = -1;
    uint32_t profile = g_ecx;

    if (on < 0) {
        const char *e = getenv("GB_UNLOCK_ALL");
        on = e && e[0] == '1';
    }
    if (!on || profile != GB_PROFILE || gb_save_writing())
        return;
    if (GB_MEM32(profile + 0x20u) == 0xFFFFFFFFu && GB_MEM32(profile + 0x24u) == 0xFFFFFFFFu &&
        GB_MEM32(profile + 0x28u) == 0xFFFFFFFFu &&
        (GB_MEM32(profile + 0x2Cu) & 0x0001FFFFu) == 0x0001FFFFu &&
        (GB_MEM32(profile + 0x34u) & 0x0007FFFFu) == 0x0007FFFFu)
        return;                             /* already done */
    GB_MEM32(profile + 0x20u) = 0xFFFFFFFFu;    /* ids 0x00 to 0x1F */
    GB_MEM32(profile + 0x24u) = 0xFFFFFFFFu;    /* ids 0x20 to 0x3F */
    GB_MEM32(profile + 0x28u) = 0xFFFFFFFFu;    /* ids 0x40 to 0x5F */
    GB_MEM32(profile + 0x2Cu) |= 0x0001FFFFu;   /* ids 0x60 to 0x70 */
    GB_MEM32(profile + 0x34u) |= 0x0007FFFFu;   /* the 19 ids of the table at 0x0023A000 */
}
