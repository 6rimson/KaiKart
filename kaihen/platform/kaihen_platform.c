/* The kaiHEN platform layer (native Switch 2 homebrew on libounce, libnx for Switch 2): what the
 * shared Switch 2 runtime expects from the system. The game's files are in sdmc:/wiicompiled/
 * (kaihen/sd.py): portable.txt, settings.env, UserData/, disc/ and RetroRewind/.
 *   - startup (userAppInit, before main): sockets, the log, the settings in settings.env, the folder;
 *   - the screen: libnx's default NWindow, which Dawn presents to through VK_NN_vi_surface, sized
 *     to the handheld screen or the TV (the display section, by hbx_switch2_vi_window);
 *   - Vulkan: OunceVK, linked in;
 *   - the CPU, GPU and memory clocks: clkrst, directly (the app may use it on kaiHEN), and the
 *     power state (psm): memory and GPU run raised only on a charger;
 *   - input and audio: SDL's kaiHEN drivers (kaihen/patches/sdl3-kaihen.diff), on libnx's pad and
 *     audout; this file configures the pad at startup.
 * Names: kaihen_* is this layer's own. hbx_* and hbxl_* (and the HBX_SWITCH2 build flag) are the
 * hooks the runtime and the Dawn and aurora patches call, named in the HBLauncher version of this
 * port; they keep those names until a rename of its own (to kaihen_*), planned before 1.0. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <pthread.h>
#include <switch.h>

/* The game's name and version (kaihen/docker/build.sh writes them), for the first line of its log. */
#if __has_include("kaihen_build_info.h")
#include "kaihen_build_info.h"
#endif
#ifndef KAIHEN_PRODUCT_NAME
#define KAIHEN_PRODUCT_NAME "game"
#define KAIHEN_APP_VERSION "unknown"
#endif

#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>


#define KAIHEN_ROOT "sdmc:/wiicompiled"

/* Clang reads the thread pointer from TPIDR_EL0; libnx keeps it in the thread's TLS slot (GCC's
 * -mtp=soft asks libnx). Each thread gets libnx's value in TPIDR_EL0, which the kernel keeps per
 * thread: the main thread at startup, every pthread as it starts (link: --wrap=pthread_create), and
 * the shim's own libnx threads (watchdog, log writer, profiler) at their start: Clang code they
 * call into, such as the runtime's, reads thread-locals through it. */
void *__aarch64_read_tp(void);

static inline void copy_thread_pointer(void) {
    void *tp = __aarch64_read_tp();
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(tp));
}



/* --- the card log ------------------------------------------------------------------------------ */
/* Without nxlink, stdout and stderr go to UserData/Logs/kaihen.log. Writing each line to the card
 * from whichever thread logged it stalled the game every few seconds, so they go to a "kaihenlog"
 * device that copies into a memory ring, and a low-priority thread writes the ring to the card. */
#include <fcntl.h>
#include <sys/iosupport.h>

static char g_ring[1 << 20];
static size_t g_ring_head, g_ring_tail; /* write and read positions, counted up forever */
static u64 g_ring_dropped;
static Mutex g_ring_lock;
static int g_log_fd = -1;
static Thread g_log_thread;
static volatile int g_log_stop;
/* KAIHEN_LOG_SYNC=1 in settings.env: each write goes to the card at once, so a crash keeps every
 * line before it (slower; for finding crashes). g_drain_lock keeps one drain at a time. */
static volatile int g_log_sync;
static Mutex g_drain_lock;
static void log_drain(void);

/* Writes the log to the card now, for the dock steps (settings_overlay.cpp): a crash in the next step
 * then keeps every line before it. One write each; writing every line at once instead, for 10 s
 * after a dock change, stuttered the game for 400-700 ms every 1.5 s (2026-11-26). */
void kaihen_log_flush(void) {
    if (g_log_fd >= 0)
        log_drain();
}

static ssize_t ring_write(struct _reent *r, void *fd, const char *data, size_t length) {
    (void)r, (void)fd;
    mutexLock(&g_ring_lock);
    const size_t room = sizeof g_ring - (g_ring_head - g_ring_tail);
    const size_t n = length < room ? length : room;
    for (size_t done = 0; done < n;) {
        const size_t at = (g_ring_head + done) % sizeof g_ring;
        size_t part = sizeof g_ring - at;
        if (part > n - done)
            part = n - done;
        memcpy(g_ring + at, data + done, part);
        done += part;
    }
    g_ring_head += n;
    g_ring_dropped += length - n;
    mutexUnlock(&g_ring_lock);
    if (g_log_sync)
        log_drain();
    return (ssize_t)length;
}

static int ring_open(struct _reent *r, void *file, const char *path, int flags, int mode) {
    (void)r, (void)file, (void)path, (void)flags, (void)mode;
    return 0;
}

static int ring_close(struct _reent *r, void *fd) {
    (void)r, (void)fd;
    return 0;
}

static const devoptab_t g_ring_device = {
    .name = "kaihenlog",
    .structSize = sizeof(int),
    .open_r = ring_open,
    .close_r = ring_close,
    .write_r = ring_write,
};

/* Writes what the ring holds to the card. Only the log thread, or the exit once it has stopped. */
static void log_drain(void) {
    static char chunk[64 << 10];
    mutexLock(&g_drain_lock);
    for (;;) {
        mutexLock(&g_ring_lock);
        size_t n = g_ring_head - g_ring_tail;
        if (n > sizeof chunk)
            n = sizeof chunk;
        for (size_t done = 0; done < n;) {
            const size_t at = (g_ring_tail + done) % sizeof g_ring;
            size_t part = sizeof g_ring - at;
            if (part > n - done)
                part = n - done;
            memcpy(chunk + done, g_ring + at, part);
            done += part;
        }
        g_ring_tail += n;
        mutexUnlock(&g_ring_lock);
        if (n == 0)
            break;
        write(g_log_fd, chunk, n);
    }
    mutexUnlock(&g_drain_lock);
}

static void log_thread(void *arg) {
    (void)arg;
    copy_thread_pointer();
    while (!g_log_stop) {
        svcSleepThread(200000000ull);
        log_drain();
    }
}

/* 1 when the log at path ends the way a game that went back to hbmenu or quit ends its log. */
static int log_ended_cleanly(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 1;
    char tail[512];
    const off_t size = lseek(fd, 0, SEEK_END);
    const off_t from = size > (off_t)(sizeof tail - 1) ? size - (off_t)(sizeof tail - 1) : 0;
    ssize_t n = lseek(fd, from, SEEK_SET) == from ? read(fd, tail, sizeof tail - 1) : -1;
    close(fd);
    if (n <= 0)
        return size <= 0;
    tail[n] = 0;
    return strstr(tail, "[kaihen] returning to hbmenu") != NULL || strstr(tail, ": quitting\n") != NULL;
}

static void log_start(void) {
    /* The previous run's log stays as kaihen.prev.log (only the last one), so a problem survives
     * the next launch. A run that didn't end cleanly (a crash, or closed from the HOME Menu) is kept
     * as kaihen.crash.log instead, until the next such run, so two more launches don't lose it.
     * The card's file system won't rename over a file, so the old one goes first. */
    const char *keep = log_ended_cleanly(KAIHEN_ROOT "/UserData/Logs/kaihen.log")
                           ? KAIHEN_ROOT "/UserData/Logs/kaihen.prev.log"
                           : KAIHEN_ROOT "/UserData/Logs/kaihen.crash.log";
    remove(keep);
    rename(KAIHEN_ROOT "/UserData/Logs/kaihen.log", keep);
    g_log_fd = open(KAIHEN_ROOT "/UserData/Logs/kaihen.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_log_fd < 0 || AddDevice(&g_ring_device) < 0)
        return;
    const int ring = open("kaihenlog:/", O_WRONLY);
    if (ring < 0)
        return;
    fflush(stdout);
    dup2(ring, STDOUT_FILENO);
    dup2(ring, STDERR_FILENO);
    close(ring);
    if (R_SUCCEEDED(threadCreate(&g_log_thread, log_thread, NULL, NULL, 0x4000, 0x3B, -2)))
        threadStart(&g_log_thread);
}

/* At exit: the log thread ends, and the rest of the ring goes to the card. */
static void log_finish(void) {
    if (g_log_fd < 0)
        return;
    fflush(NULL);
    if (g_log_thread.handle) {
        g_log_stop = 1;
        threadWaitForExit(&g_log_thread);
        threadClose(&g_log_thread);
        g_log_thread.handle = 0;
    }
    log_drain();
    if (g_ring_dropped) {
        char note[96];
        const int n = snprintf(note, sizeof note, "[kaihen] %lu bytes of log dropped (the ring was full)\n",
                               (unsigned long)g_ring_dropped);
        write(g_log_fd, note, (size_t)n);
    }
    close(g_log_fd);
    g_log_fd = -1;
}
static int g_nxlink = -1;

/* --- startup ------------------------------------------------------------------------------ */

/* settings.env: KEY=VALUE lines, # comments. A card set up by an older build has the same file as
 * hbxl-env: it is renamed once. */
#define KAIHEN_SETTINGS KAIHEN_ROOT "/settings.env"

static void load_settings(void) {
    struct stat st;
    if (stat(KAIHEN_SETTINGS, &st) != 0 && stat(KAIHEN_ROOT "/hbxl-env", &st) == 0)
        rename(KAIHEN_ROOT "/hbxl-env", KAIHEN_SETTINGS);
    FILE *f = fopen(KAIHEN_SETTINGS, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *end = line + strcspn(line, "\r\n");
        *end = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line)
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}

void hbxl_cpu_clock_restore(void);
static void extra_clocks_set(void);
static void power_check(void);
static void power_close(void);
void __libnx_exit(int rc) __attribute__((noreturn));

/* --- sampling profiler (KAIHEN_PROFILE=1 in settings.env) -------------------------------------- */
/* During races, about a thousand times a second, a thread on a core of its own pauses the game
 * thread for a moment and records the time, its pc and lr, and up to 8 callers from the frame-pointer
 * chain. They go to UserData/Logs/profile.bin at exit: a header {userAppInit's address (subtract the
 * ELF's to get the load address), sample count, words per sample}, then the samples. */
int kaihen_in_race(void);           /* runtime/src/hle/vi.cpp */
long long kaihen_frame_age_us(void); /* the same: how long since the last frame was presented */
void userAppInit(void);

static u64 *g_samples;
static size_t g_sample_count; /* words */
enum { PROFILE_FRAMES = 8, PROFILE_WORDS = 3 + PROFILE_FRAMES, PROFILE_SAMPLES = 200000 };
static Thread g_profiler;
static volatile int g_profiler_stop;
static int g_profile_loads; /* KAIHEN_PROFILE=load: only frames that take over 200 ms */

static void profiler(void *arg) {
    (void)arg;
    copy_thread_pointer();
    const Handle game = envGetMainThreadHandle();
    while (!g_profiler_stop) {
        svcSleepThread(1000000ull);
        if (g_sample_count + PROFILE_WORDS > (size_t)PROFILE_SAMPLES * PROFILE_WORDS)
            continue;
        if (g_profile_loads ? kaihen_frame_age_us() < 200000 : !kaihen_in_race())
            continue;
        if (R_FAILED(svcSetThreadActivity(game, ThreadActivity_Paused)))
            continue;
        ThreadContext context;
        const Result rc = svcGetThreadContext3(&context, game);
        u64 *sample = g_samples + g_sample_count;
        if (R_SUCCEEDED(rc)) {
            sample[0] = armGetSystemTick();
            sample[1] = context.pc.x;
            sample[2] = context.lr;
            /* Frame records {fp, lr}, on the thread's stack (a guest coroutine's, too) above sp. */
            u64 fp = context.fp, sp = context.sp;
            for (int i = 0; i < PROFILE_FRAMES; i++) {
                u64 lr = 0;
                if (fp > sp && fp < sp + 0x1000000 && !(fp & 7)) {
                    lr = ((const u64 *)fp)[1];
                    const u64 next = ((const u64 *)fp)[0];
                    sp = fp;
                    fp = next;
                } else {
                    fp = 0;
                }
                sample[3 + i] = lr;
            }
        }
        svcSetThreadActivity(game, ThreadActivity_Runnable);
        if (R_SUCCEEDED(rc))
            g_sample_count += PROFILE_WORDS;
    }
}

static void profiler_start(int core) {
    const char *on = getenv("KAIHEN_PROFILE");
    if (!on || (strcmp(on, "1") != 0 && strcmp(on, "load") != 0))
        return;
    g_profile_loads = strcmp(on, "load") == 0;
    g_samples = malloc((size_t)PROFILE_SAMPLES * PROFILE_WORDS * sizeof *g_samples);
    if (g_samples && R_SUCCEEDED(threadCreate(&g_profiler, profiler, NULL, NULL, 0x4000, 0x2C, core)))
        threadStart(&g_profiler);
    fprintf(stderr, "[kaihen] profiling %s on core %d (userAppInit at %p)\n",
            g_profile_loads ? "frames over 200 ms" : "races", core, (void *)userAppInit);
}

static void profiler_finish(void) {
    /* Not from the profiler thread itself: it would wait for its own end. */
    if (!g_profiler.handle || threadGetCurHandle() == g_profiler.handle)
        return;
    g_profiler_stop = 1;
    threadWaitForExit(&g_profiler);
    threadClose(&g_profiler);
    g_profiler.handle = 0;
    const int fd = open(KAIHEN_ROOT "/UserData/Logs/profile.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    const u64 header[3] = {(u64)(uintptr_t)userAppInit, g_sample_count / PROFILE_WORDS, PROFILE_WORDS};
    write(fd, header, sizeof header);
    write(fd, g_samples, g_sample_count * sizeof *g_samples);
    close(fd);
    fprintf(stderr, "[kaihen] %lu profile samples written\n", (unsigned long)(g_sample_count / PROFILE_WORDS));
}
void __appExit(void);

/* SQLite (aurora's shader and pipeline caches): its log on ours; the VFS without file locks as the
 * default, since libnx's SD card files take no fcntl locks; and an fstat that works on them. */
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>

static void sqlite_log(void *arg, int code, const char *message) {
    (void)arg;
    fprintf(stderr, "[sqlite] %d: %s\n", code, message);
}

/* Where libnx's stat calls fail on these files, SQLite gets a regular file of the right length (it
 * also wants an inode, which it only compares between connections to one file). */
static int length_stat(int fd, struct stat *st) {
    const off_t here = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END);
    if (here < 0 || end < 0)
        return -1;
    lseek(fd, here, SEEK_SET);
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0644;
    st->st_size = end;
    st->st_ino = (ino_t)fd + 1;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

static int sqlite_fstat(int fd, struct stat *st) {
    if (fstat(fd, st) == 0)
        return 0;
    static int reported;
    if (reported++ < 8)
        fprintf(stderr, "[kaihen] fstat %d failed (%d): SQLite uses the file's length instead\n", fd, errno);
    return length_stat(fd, st);
}

/* libnx's stat opens the file to read its size, and the console's file system won't open a file
 * that is already open for writing (EIO): SQLite stats its own open database before creating the
 * journal. The entry's type, asked without opening it, is all SQLite needs there. */
static int sqlite_stat(const char *path, struct stat *st) {
    if (stat(path, st) == 0)
        return 0;
    const int error = errno;
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    FsDirEntryType type;
    char fs_path[FS_MAX_PATH];
    snprintf(fs_path, sizeof fs_path, "%s", strncmp(path, "sdmc:", 5) == 0 ? path + 5 : path);
    if (error == ENOENT || !fs || R_FAILED(fsFsGetEntryType(fs, fs_path, &type))) {
        errno = error;
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->st_mode = (type == FsDirEntryType_Dir ? S_IFDIR | 0755 : S_IFREG | 0644);
    st->st_size = type == FsDirEntryType_Dir ? 0 : 1; /* unknown, but a file in use isn't empty */
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

static void sqlite_setup(void) {
    sqlite3_config(SQLITE_CONFIG_LOG, sqlite_log, (void *)0);
    sqlite3_vfs *vfs = sqlite3_vfs_find("unix-none");
    if (vfs) {
        sqlite3_vfs_register(vfs, 1);
        if (vfs->iVersion >= 3 && vfs->xSetSystemCall) {
            vfs->xSetSystemCall(vfs, "fstat", (sqlite3_syscall_ptr)sqlite_fstat);
            vfs->xSetSystemCall(vfs, "stat", (sqlite3_syscall_ptr)sqlite_stat);
            vfs->xSetSystemCall(vfs, "lstat", (sqlite3_syscall_ptr)sqlite_stat);
        }
    }
    fprintf(stderr, "[kaihen] sqlite: %s\n", vfs ? "unix-none VFS" : "no unix-none VFS");
}

/* Ends the game without tearing it down (its GPU and worker threads still run). An application
 * that just ends counts as crashed ("the software needed to close"): it has to ask the applet
 * manager to end it (ISelfController Exit). libnx does that for a program it started itself, or
 * with __nx_applet_exit_mode 1: appletExit then leaves an exit function that sends the request,
 * and the applet manager ends the process. The CPU clock goes back to the launch rate first. */
extern u32 __nx_applet_exit_mode;

static void __attribute__((noreturn)) quit_now(const char *why) {
    fprintf(stderr, "[kaihen] %s: quitting\n", why);
    hbxl_cpu_clock_restore();
    profiler_finish();
    fflush(NULL);
    log_finish();
    __nx_applet_exit_mode = 1;
    const LoaderReturnFn loader_return = envGetExitFuncPtr();
    for (int i = 0; i < 8 && envGetExitFuncPtr() == loader_return; i++)
        appletExit();
    const LoaderReturnFn self_exit = envGetExitFuncPtr();
    if (self_exit && self_exit != loader_return)
        self_exit(0);
    svcExitProcess();
    __builtin_unreachable();
}

/* --- returning to hbmenu --------------------------------------------------------------------- */
/* As any homebrew does: return through libnx's exit, and kaiHEN's hbloader (ounce-hbloader) loads
 * hbmenu again, in this same process; the next game loads at the same addresses. hbloader parks a
 * thread a program leaves running (pauses it for good) and closes one that ended unclosed, but a
 * parked thread still waiting there hangs the next launch, and the closing crashed after online
 * play. So nothing is left to it: the runtime ends graphics, audio and its workers first
 * (settings_overlay.cpp ConsoleReturnToLauncher), and the detached threads that ended are closed
 * here (__wrap_pthread_detach). hbloader's report, sdmc:/kaihen/nro-cleanup.txt, then says "clean".
 * Never pause a thread for good here: hbloader's own pause would fail, and it ends the process. */

static volatile int g_return_requested;
int kaihen_return_requested(void) { return g_return_requested; }

/* The game's other threads still run until hbloader parks them and may still log: userAppExit
 * leaves the log open on this path. */
static volatile int g_returning;
static int g_exit_fd = -1; /* UserData/Logs/exit.txt while returning (step) */
static void step(const char *name);
static int list_threads(void);
static int kaihen_close_ended_threads(int *still_running);
static void log_resources(const char *when);
static int g_games_before; /* games that ran before this one in this kaiHEN session */
static void vulkan_finish(void);
static Thread g_watchdog;

void kaihen_return_to_hbmenu(void) {
    if (threadGetCurHandle() != envGetMainThreadHandle())
        quit_now("return to hbmenu off the main thread");
    /* The watchdog uses the applet and input services, which libnx's exit closes: it ends first. */
    g_returning = 1;
    threadWaitForExit(&g_watchdog);
    threadClose(&g_watchdog);
    power_close();
    hbxl_cpu_clock_restore();
    vulkan_finish();
    int detached_running = 0;
    const int detached_closed = kaihen_close_ended_threads(&detached_running);
    fprintf(stderr, "[kaihen] at exit: %d ended detached threads closed, %d still running\n", detached_closed,
            detached_running);
    log_resources("exit");
    fprintf(stderr, "[kaihen] returning to hbmenu\n");
    profiler_finish();
    fflush(NULL);
    log_finish();
    g_exit_fd = open(KAIHEN_ROOT "/UserData/Logs/exit.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    step("log closed");
    /* A thread still running is paused for good by hbloader, and the next launch loads the same
     * game at the same addresses: a paused thread waiting on a condition variable or lock there can
     * take the new game's wake-up and lock, and the game hangs. So the game's workers end before
     * this (settings_overlay.cpp ConsoleReturnToLauncher), and the detached ones are closed (above).
     * A thread that only sleeps waits on nothing, and hbloader parks it; if any thread is doing
     * anything else, it quits to the HOME Menu instead, which ends the process. */
    if (list_threads() > 0) {
        step("threads still waiting: quitting to the HOME Menu instead");
        close(g_exit_fd);
        g_exit_fd = -1;
        quit_now("threads still running at the return");
    }
    __libnx_exit(0); /* services closed (userAppExit too, and the GPU driver's), then back to hbloader */
}

/* Until the game handles the console's applet messages (milestone 3): a thread that services the
 * applet. Holding + and - for a second asks the game to quit between frames; if no
 * frame comes within 3 seconds, it quits to the home menu instead. It ends when a return starts. */
static void watchdog(void *arg) {
    (void)arg;
    copy_thread_pointer();
    PadState pad;
    padInitializeDefault(&pad);
    int held = 0, ticks = 0;
    while (!g_returning && appletMainLoop()) {
        if (ticks++ % 20 == 0)
            power_check(); /* once a second */
        padUpdate(&pad);
        const u64 buttons = padGetButtons(&pad);
        held = (buttons & HidNpadButton_Plus) && (buttons & HidNpadButton_Minus) ? held + 1 : 0;
        if (held >= 20)
            break;
        svcSleepThread(50000000ull);
    }
    if (g_returning)
        return;
    fprintf(stderr, "[kaihen] + and - held: returning to hbmenu\n");
    g_return_requested = 1;
    for (int waited = 0; waited < 60; waited++) {
        svcSleepThread(50000000ull);
        if (g_returning)
            return;
    }
    quit_now("no frame to return from");
}

/* CPU cores: the game thread keeps a core of its own, the runtime pins a few workers to cores of
 * their own (hbxl_pin_next_thread: GX and video 5, audio 3, aurora's GPU worker 4, in the runtime's
 * numbering, where the game thread is on core 1), and every other thread may run on any core that
 * is neither. libnx alone starts every
 * pthread at the lowest priority with the game thread's core as its home, so the workers waited
 * behind the game thread. */
static Mutex g_cores_lock;
static int g_game_core = -1, g_pin_core = -1;
static u64 g_cores, g_pinned_cores;

static int kaihen_core(int runtime_core) {
    /* The runtime's game core (1) and this console's game core trade places. */
    if (runtime_core == 1)
        return g_game_core;
    if (runtime_core == g_game_core)
        return 1;
    return runtime_core;
}

static void cores_start(void) {
    svcGetInfo(&g_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    g_game_core = (int)svcGetCurrentProcessorNumber();
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_game_core, 1ull << g_game_core);
}

void hbxl_pin_next_thread(int core) {
    mutexLock(&g_cores_lock);
    const int mapped = g_game_core >= 0 ? kaihen_core(core) : -1;
    if (mapped >= 0 && mapped < 64 && mapped != g_game_core && ((g_cores >> mapped) & 1)) {
        g_pin_core = mapped;
        g_pinned_cores |= 1ull << mapped;
    }
    mutexUnlock(&g_cores_lock);
}

struct thread_start {
    void *(*function)(void *);
    void *argument;
    int core; /* pinned, or -1 */
};

static void *start_with_thread_pointer(void *start) {
    const struct thread_start copy = *(struct thread_start *)start;
    free(start);
    copy_thread_pointer();
    if (g_game_core >= 0) {
        if (copy.core >= 0) {
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, copy.core, 1ull << copy.core);
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
        } else {
            mutexLock(&g_cores_lock);
            u64 mask = g_cores & ~(1ull << g_game_core) & ~g_pinned_cores;
            mutexUnlock(&g_cores_lock);
            if (!mask)
                mask = g_cores & ~(1ull << g_game_core);
            if (mask)
                svcSetThreadCoreMask(CUR_THREAD_HANDLE, __builtin_ctzll(mask), mask);
        }
    }
    return copy.function(copy.argument);
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*function)(void *), void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*function)(void *), void *argument) {
    struct thread_start *start = malloc(sizeof *start);
    if (!start)
        return EAGAIN;
    mutexLock(&g_cores_lock);
    *start = (struct thread_start){function, argument, g_pin_core};
    g_pin_core = -1;
    mutexUnlock(&g_cores_lock);
    const int rc = __real_pthread_create(thread, attr, start_with_thread_pointer, start);
    if (rc)
        free(start);
    return rc;
}

/* --- the card's own files ----------------------------------------------------------------------- */
/* The app carries the small files the wiicompiled folder needs (romfs:/wiicompiled, staged by
 * kaihen/docker/build.sh): settings, Config.toml, the runtime's DSP coefficients and certificates, the Wii
 * bootstrap files. A card that has only the app and a disc image gets them at its first launch.
 * Files already on the card are kept, except the runtime's own two, which follow the app. */
#include <dirent.h>
#include <limits.h>

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in)
        return;
    char partial[PATH_MAX];
    snprintf(partial, sizeof partial, "%s.partial", to);
    FILE *out = fopen(partial, "wb");
    if (out) {
        char buffer[16384];
        size_t n, failed = 0;
        while ((n = fread(buffer, 1, sizeof buffer, in)) > 0)
            failed |= fwrite(buffer, 1, n, out) != n;
        failed |= fclose(out) != 0;
        if (failed || (unlink(to), rename(partial, to)) != 0)
            unlink(partial);
    }
    fclose(in);
}

static int follows_app(const char *name) {
    return strcmp(name, "dsp_coef.bin") == 0 || strcmp(name, "cacert.pem") == 0;
}

static void copy_missing(const char *from, const char *to) {
    DIR *dir = opendir(from);
    if (!dir)
        return;
    mkdir(to, 0777);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char source[PATH_MAX], target[PATH_MAX];
        snprintf(source, sizeof source, "%s/%s", from, entry->d_name);
        snprintf(target, sizeof target, "%s/%s", to, entry->d_name);
        struct stat in, out;
        if (stat(source, &in) != 0)
            continue;
        if (S_ISDIR(in.st_mode)) {
            copy_missing(source, target);
            continue;
        }
        const int present = stat(target, &out) == 0;
        if (present && !(follows_app(entry->d_name) && out.st_size != in.st_size))
            continue;
        /* settings.env replaces an older card's hbxl-env, which load_settings renames to it. */
        if (strcmp(entry->d_name, "settings.env") == 0 && stat(KAIHEN_ROOT "/hbxl-env", &out) == 0)
            continue;
        copy_file(source, target);
    }
    closedir(dir);
}

static void card_setup(void) {
    if (R_FAILED(romfsMountSelf("romfs")))
        return;
    copy_missing("romfs:/wiicompiled", KAIHEN_ROOT);
    romfsUnmount("romfs");
}

static void count_session_games(void);

void userAppInit(void) {
    copy_thread_pointer();
    cores_start();
    card_setup();
    count_session_games();
    /* libnx's defaults, with TCP receive buffers that may grow to 1 MiB (from 256 KiB): the
     * first-launch Retro Rewind download (about 1.9 GB) was limited by it on Wi-Fi. */
    SocketInitConfig sockets = *socketGetDefaultInitConfig();
    sockets.tcp_rx_buf_max_size = 0x100000;
    socketInitialize(&sockets);
    /* With nxlink -s, stdout and stderr go to the PC; otherwise to the card. */
    g_nxlink = nxlinkStdio();
    mkdir(KAIHEN_ROOT "/UserData", 0777);
    mkdir(KAIHEN_ROOT "/UserData/Logs", 0777);
    if (g_nxlink < 0)
        log_start();
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    load_settings();
    if (g_log_fd >= 0 && getenv("KAIHEN_LOG_SYNC") && strcmp(getenv("KAIHEN_LOG_SYNC"), "1") == 0) {
        g_log_sync = 1;
        fprintf(stderr, "[kaihen] log: each line written to the card at once (KAIHEN_LOG_SYNC=1)\n");
    }
    extra_clocks_set();
    sqlite_setup();
    /* SDL draws nothing itself (aurora presents through Dawn to the NWindow), so its offscreen
     * video driver. settings.env wins. */
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    /* Compiled shaders are cached by Dawn (UserData/Cache/dawn_cache.db), which hands each pipeline's
     * shaders to the driver when it creates the pipeline. OunceVK's own cache
     * (sdmc:/config/ouncevk/shader-cache) then saved all of them again, about 8,000 shaders it already
     * had, on every launch: minutes of card writes that slowed the menus until a race had started, a
     * store that grew without end (79,453 records for 6,122 shaders) and a queue lost at each exit.
     * With Dawn's cache doing the work, the driver's is off. settings.env wins. */
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
    if (chdir(KAIHEN_ROOT) != 0)
        fprintf(stderr, "[kaihen] no %s on the card (%d)\n", KAIHEN_ROOT, errno);
    /* Up to four players (local multiplayer): the SDL driver gives each console player slot its
     * own controller (kaihen/patches/sdl3-kaihen.diff); a pair of Joy-Cons is one player. */
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    /* A single Joy-Con is held sideways in this game, so the console shows and keeps it that way. */
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    if (R_SUCCEEDED(threadCreate(&g_watchdog, watchdog, NULL, NULL, 0x10000, 0x2C, -2)))
        threadStart(&g_watchdog);
    fprintf(stderr, "[kaihen] %s %s started, nxlink %s; game thread on core %d of mask %lx; hold + and - to quit\n",
            KAIHEN_PRODUCT_NAME, KAIHEN_APP_VERSION, g_nxlink >= 0 ? "on" : "off", g_game_core, (unsigned long)g_cores);
    if (g_games_before)
        fprintf(stderr, "[kaihen] game %d in this kaiHEN session\n", g_games_before + 1);
    log_resources("launch");
    profiler_start(g_game_core == 1 ? 0 : 1); /* a core no worker is pinned to */
}

void userAppExit(void) {
    if (!g_returning) {
        fprintf(stderr, "[kaihen] exit\n");
        log_finish();
        if (g_nxlink >= 0)
            close(g_nxlink);
        socketExit();
    }
    /* Returning to hbmenu: the sockets stay open until the end of __appExit, so the log over
     * nxlink shows every step. */
}

/* libounce's __appExit (runtime/init.c, as its init.o does it) and __libnx_exit, with a line in the
 * log before each step: where a return to hbmenu fails shows in the log. */
void __nx_gpu_exit(void) __attribute__((weak));
void __nx_win_exit(void) __attribute__((weak));
void __nx_exit(Result rc, LoaderReturnFn retaddr) __attribute__((noreturn));

/* Returning to hbmenu: each step goes to nxlink when it is on, and to UserData/Logs/exit.txt on the
 * card (the log is closed by then), flushed each time, so the last one written is where a failed
 * return stopped (g_exit_fd, opened in kaihen_return_to_hbmenu). */

static void step(const char *name) {
    if (!g_returning)
        return;
    char line[96];
    const int n = snprintf(line, sizeof line, "[kaihen] exit: %s\n", name);
    if (g_nxlink >= 0)
        write(STDERR_FILENO, line, (size_t)n);
    if (g_exit_fd >= 0) {
        write(g_exit_fd, line, (size_t)n);
        fsync(g_exit_fd);
    }
}

void __appExit(void) {
    static int drivers_closed;
    step("userAppExit");
    userAppExit();
    if (!drivers_closed) {
        drivers_closed = 1;
        step("GPU driver");
        if (__nx_gpu_exit)
            __nx_gpu_exit();
        step("window");
        if (__nx_win_exit)
            __nx_win_exit();
    }
    if (g_returning) {
        /* Returning to hbmenu: the game's files (shader caches, logs, saves) are closed, and the card
         * stays mounted: unmounting it with the game's threads still around crashed. hbmenu opens its
         * own file system session. */
        step("closing files");
        for (int fd = 3; fd < 1024; fd++)
            if (fd != g_nxlink && fd != g_exit_fd)
                close(fd);
        step("file system left open");
    } else {
        fsdevUnmountAll();
        fsExit();
    }
    step("time");
    timeExit();
    step("input");
    hidExit();
    step("applet");
    appletExit();
    step("sockets");
    if (g_returning)
        socketExit();
    step("done, back to hbloader");
    if (g_exit_fd >= 0) {
        close(g_exit_fd);
        g_exit_fd = -1;
    }
    smExit();
}

void __libnx_exit(int rc) {
    (void)rc;
    __appExit();
    __nx_exit(0, envGetExitFuncPtr());
}

/* The folder the runtime treats as the executable's (runtime_config.h ExecutableDirectory). Without
 * the "sdmc:" prefix: newlib resolves a plain absolute path on the card, and code that only
 * recognizes "/" as absolute (SQLite's Unix layer) then sees one. */
const char *kaihen_root_directory(void) { return "/wiicompiled"; }

void hbxl_logf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* --- threads at a return to hbmenu ----------------------------------------------------------- */
/* The process's free thread slots and memory, at launch and on the way back to hbmenu: a run that
 * leaves threads behind leaves fewer slots for the next launch. */
static void log_resources(const char *when) {
    u64 free_threads = 0, total = 0, used = 0;
    svcGetInfo(&free_threads, InfoType_FreeThreadCount, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(stderr, "[kaihen] %s: %lu free thread slots, memory %lu of %lu MiB used\n", when,
            (unsigned long)free_threads, (unsigned long)(used >> 20), (unsigned long)(total >> 20));
}

/* At a return: each thread of the game's that is still running (hbloader will park it), with where
 * it is (pc, lr), to exit.txt. The image's base is userAppInit's address. Found the way hbloader
 * finds them: libnx's ThreadVars at the end of each TLS slot. Returns how many are doing anything
 * but sleep (svcSleepThread): those are the ones a next launch can't live with. */
static void exit_line(const char *line, int n) {
    if (g_exit_fd >= 0)
        write(g_exit_fd, line, (size_t)n);
}

static int list_threads(void) {
    char line[320];
    const u64 own = (u64)armGetTls() & ~(u64)0x1FF;
    const Handle main_thread = envGetMainThreadHandle();
    int count = 0, waiting = 0;
    u64 address = 0;
    for (;;) {
        MemoryInfo info = {0};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, address)))
            break;
        if (info.type == MemType_ThreadLocal && (info.perm & Perm_R)) {
            for (u64 slot = info.addr; slot + 0x200 <= info.addr + info.size; slot += 0x200) {
                if (slot == own)
                    continue;
                u32 *vars = (u32 *)(slot + 0x200 - 0x20);
                Handle handle = vars[1];
                if (vars[0] != 0x21545624 || handle == INVALID_HANDLE || handle == main_thread)
                    continue;
                u64 id = 0;
                s32 index = -1;
                if (R_FAILED(svcGetThreadId(&id, handle)) || R_SUCCEEDED(svcWaitSynchronization(&index, &handle, 1, 0)))
                    continue; /* gone, or ended */
                u64 pc = 0, lr = 0, x0 = 0, fp = 0;
                if (R_SUCCEEDED(svcSetThreadActivity(handle, ThreadActivity_Paused))) {
                    ThreadContext context;
                    for (int tries = 0; tries < 10; tries++) {
                        memset(&context, 0, sizeof context);
                        if (R_SUCCEEDED(svcGetThreadContext3(&context, handle))) {
                            pc = context.pc.x;
                            lr = context.lr;
                            x0 = context.cpu_gprs[0].x;
                            fp = context.fp;
                            break;
                        }
                        svcSleepThread(100000);
                    }
                    svcSetThreadActivity(handle, ThreadActivity_Runnable);
                }
                const int sleeping = pc >= (u64)(uintptr_t)svcSleepThread && pc <= (u64)(uintptr_t)svcSleepThread + 8;
                if (!sleeping)
                    waiting++;
                int n = snprintf(line, sizeof line, "[kaihen] exit: thread %lu %s, pc 0x%lx lr 0x%lx x0 0x%lx;",
                                 (unsigned long)id, sleeping ? "sleeping" : "waiting", (unsigned long)pc,
                                 (unsigned long)lr, (unsigned long)x0);
                /* Callers, from the frame-pointer chain (each frame: the caller's fp, then lr). */
                for (int depth = 0; depth < 6 && fp && !(fp & 7) && n < (int)sizeof line - 24; depth++) {
                    MemoryInfo frame = {0};
                    u32 frame_page = 0;
                    if (R_FAILED(svcQueryMemory(&frame, &frame_page, fp)) || !(frame.perm & Perm_R) ||
                        fp + 16 > frame.addr + frame.size)
                        break;
                    const u64 *record = (const u64 *)(uintptr_t)fp;
                    n += snprintf(line + n, sizeof line - n, " 0x%lx", (unsigned long)record[1]);
                    fp = record[0];
                }
                if (n < (int)sizeof line - 1)
                    line[n++] = '\n';
                exit_line(line, n);
                count++;
            }
        }
        const u64 next = info.addr + info.size;
        if (next <= address)
            break;
        address = next;
    }
    u64 free_threads = 0;
    svcGetInfo(&free_threads, InfoType_FreeThreadCount, CUR_PROCESS_HANDLE, 0);
    const int n = snprintf(line, sizeof line,
                           "[kaihen] exit: %d threads running, %d of them not just sleeping; %lu free thread slots; "
                           "userAppInit at 0x%lx\n",
                           count, waiting, (unsigned long)free_threads, (unsigned long)(uintptr_t)userAppInit);
    exit_line(line, n);
    return waiting;
}

/* --- screen and Vulkan -------------------------------------------------------------------- */

/* The picture's size, as Torchlight Rekindled does it on Switch 2 (GPL-3.0,
 * github.com/GreenSwampDweller/torchlight-rekindled, src/platform/switch/platform_frame.cpp):
 * the handheld screen's own 1920x1080, not libnx's 1280x720 default (a Switch 1 size); docked, the
 * lines chosen in the port menu's "TV output" (1080, 1440 or 2160), never more than the TV takes, since the
 * console crops a picture larger than the screen rather than scaling it. The game's window (its
 * picture) follows: main.cpp at start, settings_overlay.cpp when the console docks or undocks. */
static uint32_t g_docked_lines;     /* KAIHEN_DOCKED_LINES; 0 until read */
static int g_display_mode = -1;     /* the console's mode the window was last sized for */
static int g_display_reapply;       /* the docked lines changed: size again */
static u32 g_display_width, g_display_height;

static void save_setting(const char *key, const char *value);

uint32_t kaihen_docked_lines(void) {
    if (!g_docked_lines) {
        const char *lines = getenv("KAIHEN_DOCKED_LINES");
        const unsigned long n = lines ? strtoul(lines, NULL, 10) : 1080;
        g_docked_lines = n >= 2160 ? 2160 : n >= 1440 ? 1440 : 1080;
    }
    return g_docked_lines;
}

static int display_docked(void) { return appletGetOperationMode() == AppletOperationMode_Console; }

static void display_wanted(u32 *width, u32 *height) {
    *width = 1920;
    *height = 1080;
    if (!display_docked())
        return;
    s32 tv_width = 0, tv_height = 0;
    if (R_FAILED(appletGetDefaultDisplayResolution(&tv_width, &tv_height)) || tv_width <= 0 || tv_height <= 0)
        return;
    const uint32_t lines = kaihen_docked_lines();
    if (lines >= 2160) {
        *width = (u32)tv_width;
        *height = (u32)tv_height;
    } else if (lines >= 1440 && tv_height >= 1440) {
        *width = 2560;
        *height = 1440;
    }
}

static void display_apply(u32 width, u32 height) {
    const Result rc = nwindowSetDimensions(nwindowGetDefault(), width, height);
    g_display_width = width;
    g_display_height = height;
    g_display_mode = (int)appletGetOperationMode();
    s32 tv_width = 0, tv_height = 0;
    appletGetDefaultDisplayResolution(&tv_width, &tv_height);
    fprintf(stderr, "[kaihen] display: %s, picture %ux%u (0x%x), screen %dx%d, docked choice %u lines\n",
            display_docked() ? "docked" : "handheld", width, height, (unsigned)rc, tv_width, tv_height,
            (unsigned)kaihen_docked_lines());
}

/* Docked now, for the port menu's "TV output" row. */
int kaihen_display_docked_now(void) { return display_docked(); }

/* The picture's size now, for the game's window at start. */
void kaihen_display_size(uint32_t *width, uint32_t *height) {
    u32 w, h;
    display_wanted(&w, &h);
    *width = w;
    *height = h;
}

/* Once a frame, from the game's window thread: 1 with the new size when the console docked or
 * undocked (or the docked lines changed) and the picture's size changes. The caller then releases
 * the surface, and the window takes the size when it's made again. */
int kaihen_display_follow(uint32_t *width, uint32_t *height) {
    if ((int)appletGetOperationMode() == g_display_mode && !g_display_reapply)
        return 0;
    if ((int)appletGetOperationMode() != g_display_mode) {
        fprintf(stderr, "[kaihen] console %s\n", display_docked() ? "docked" : "undocked");
    }
    g_display_reapply = 0;
    u32 w, h;
    display_wanted(&w, &h);
    g_display_mode = (int)appletGetOperationMode();
    if (w == g_display_width && h == g_display_height)
        return 0;
    /* The window's size may change only while no buffers are registered with it (libnx), so it
     * isn't set here: the game releases its surface (settings_overlay.cpp), and the next frame
     * makes a new one, whose hbx_switch2_vi_window sets the size first. Setting it here, with the
     * swapchain's 4K buffers still registered, crashed the game at an undock (2026-11-26). */
    g_display_width = w;
    g_display_height = h;
    fprintf(stderr, "[kaihen] display: %s, picture %ux%u once the surface is made again\n",
            display_docked() ? "docked" : "handheld", w, h);
    *width = w;
    *height = h;
    return 1;
}

/* The port menu's "TV output": 1080, 1440 or 2160 lines when docked, saved in settings.env. */
void kaihen_set_docked_lines(uint32_t lines) {
    g_docked_lines = lines >= 2160 ? 2160 : lines >= 1440 ? 1440 : 1080;
    char value[8];
    snprintf(value, sizeof value, "%u", (unsigned)g_docked_lines);
    save_setting("KAIHEN_DOCKED_LINES", value);
    g_display_reapply = 1;
}

/* The Resolution row's value on a TV (KAIHEN_TV_RESOLUTION in settings.env, 1 to 4); the game's
 * Config.toml keeps the handheld one. 0 until set: then the game uses the Config.toml value. */
static float g_tv_resolution = -1.0f;

float kaihen_tv_resolution(void) {
    if (g_tv_resolution < 0.0f) {
        const char *value = getenv("KAIHEN_TV_RESOLUTION");
        const float scale = value ? strtof(value, NULL) : 0.0f;
        g_tv_resolution = scale >= 1.0f && scale <= 4.0f ? scale : 0.0f;
    }
    return g_tv_resolution;
}

void kaihen_set_tv_resolution(float scale) {
    g_tv_resolution = scale;
    char value[16];
    snprintf(value, sizeof value, "%.1f", (double)scale);
    save_setting("KAIHEN_TV_RESOLUTION", value);
}

void *hbx_switch2_vi_window(void) {
    NWindow *window = nwindowGetDefault();
    u32 width, height;
    display_wanted(&width, &height);
    display_apply(width, height);
    return window;
}

/* Dawn's Switch path asks the driver for its entry point the way Mesa's ICD exports it. */
/* The Vulkan instance at a return to hbmenu. Dawn destroys its device but keeps the instance, so the
 * driver was never shut down: with its shader cache on, its writer threads were left mid-write.
 * The instance is followed here and destroyed at a return, while the screen is already off, on a
 * thread of its own: the return waits at most kFinishSeconds for it. */
static VkInstance g_vk_instance;
static int g_vk_devices;
static PFN_vkDestroyInstance g_vk_destroy_instance;
static PFN_vkCreateDevice g_vk_create_device;
static PFN_vkDestroyDevice g_vk_destroy_device;
static PFN_vkGetDeviceProcAddr g_vk_get_device_proc_addr;

static VkResult VKAPI_CALL kaihen_vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                                   VkInstance *out) {
    const PFN_vkCreateInstance create = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    const VkResult result = create(info, allocator, out);
    if (result == VK_SUCCESS && !g_vk_instance) {
        g_vk_instance = *out;
        g_vk_destroy_instance = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(*out, "vkDestroyInstance");
        g_vk_create_device = (PFN_vkCreateDevice)vkGetInstanceProcAddr(*out, "vkCreateDevice");
        g_vk_destroy_device = (PFN_vkDestroyDevice)vkGetInstanceProcAddr(*out, "vkDestroyDevice");
        g_vk_get_device_proc_addr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(*out, "vkGetDeviceProcAddr");
    }
    return result;
}

static void VKAPI_CALL kaihen_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    if (instance == g_vk_instance)
        g_vk_instance = VK_NULL_HANDLE;
    g_vk_destroy_instance(instance, allocator);
}

static VkResult VKAPI_CALL kaihen_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkDevice *out) {
    const VkResult result = g_vk_create_device(physical, info, allocator, out);
    if (result == VK_SUCCESS)
        __atomic_add_fetch(&g_vk_devices, 1, __ATOMIC_RELAXED);
    return result;
}

static void VKAPI_CALL kaihen_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    if (device)
        __atomic_sub_fetch(&g_vk_devices, 1, __ATOMIC_RELAXED);
    g_vk_destroy_device(device, allocator);
}

static PFN_vkVoidFunction VKAPI_CALL kaihen_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)kaihen_vkDestroyDevice;
    return g_vk_get_device_proc_addr(device, name);
}

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)kaihen_vkCreateInstance;
    if (instance && instance == g_vk_instance) {
        if (strcmp(name, "vkDestroyInstance") == 0)
            return (PFN_vkVoidFunction)kaihen_vkDestroyInstance;
        if (strcmp(name, "vkCreateDevice") == 0)
            return (PFN_vkVoidFunction)kaihen_vkCreateDevice;
        if (strcmp(name, "vkDestroyDevice") == 0)
            return (PFN_vkVoidFunction)kaihen_vkDestroyDevice;
        if (strcmp(name, "vkGetDeviceProcAddr") == 0)
            return (PFN_vkVoidFunction)kaihen_vkGetDeviceProcAddr;
    }
    return vkGetInstanceProcAddr(instance, name);
}

enum { kFinishSeconds = 5 };

static void vulkan_finish_thread(void *arg) {
    (void)arg;
    copy_thread_pointer();
    kaihen_vkDestroyInstance(g_vk_instance, NULL);
}

/* At a return: the instance destroyed, once Dawn has destroyed its device (the instance must
 * outlive its devices). */
static void vulkan_finish(void) {
    if (!g_vk_instance)
        return;
    if (__atomic_load_n(&g_vk_devices, __ATOMIC_RELAXED) != 0) {
        fprintf(stderr, "[kaihen] at exit: a Vulkan device is still alive; the driver stays as it is\n");
        return;
    }
    static Thread finisher;
    const u64 start = armGetSystemTick();
    if (R_FAILED(threadCreate(&finisher, vulkan_finish_thread, NULL, NULL, 0x40000, 0x2C, -2)) ||
        R_FAILED(threadStart(&finisher))) {
        fprintf(stderr, "[kaihen] at exit: could not start closing the driver\n");
        return;
    }
    if (R_SUCCEEDED(waitSingleHandle(finisher.handle, (u64)kFinishSeconds * 1000000000ull))) {
        threadClose(&finisher);
        fprintf(stderr, "[kaihen] at exit: driver closed in %lu ms\n",
                (unsigned long)(armTicksToNs(armGetSystemTick() - start) / 1000000));
    } else {
        fprintf(stderr, "[kaihen] at exit: driver still closing after %d s; returning anyway\n", kFinishSeconds);
    }
}

/* --- memory -------------------------------------------------------------------------------- */

void *hbxl_sdk_memalign(size_t align, size_t size) {
    return aligned_alloc(align < sizeof(void *) ? sizeof(void *) : align,
                         (size + align - 1) & ~(align - 1));
}

void hbxl_sdk_free(void *p) { free(p); }

/* --- clocks -------------------------------------------------------------------------------- */
/* All three are chosen in the port menu (settings_overlay.cpp, through kaihen_clock_*) and kept in
 * settings.env: KAIHEN_CPU_MHZ, KAIHEN_MEMORY_MHZ, KAIHEN_GPU_MHZ, each a rate in MHz, "max" (the top
 * of what the system offers) or 0 (the system's own rate). Defaults: the CPU at its stock maximum
 * (1702.4 MHz, applied by the runtime's CPU boost at launch), memory and GPU at the system's.
 *
 * On battery, the console halves the CPU's real speed (clkrst still reads the rate it was set to)
 * as soon as the GPU runs above 561 MHz or memory at 2750 MHz or more, handheld; on a supported
 * charger it doesn't (tested in this port and the Cemu-nx Switch 2 port, 2026-11-14). So memory and
 * GPU run at the chosen rates only while the console says enough power is supplied, and go back to
 * the system's when it is unplugged; the CPU keeps its rate either way. The watchdog checks once a
 * second. The launch rates go back on exit (hbxl_cpu_clock_restore). */

static const uint32_t kMaxHz = 1702400000u;
static Mutex g_clock_lock;
static uint32_t g_launch_hz, g_cpu_chosen_mhz = 1702;
static int g_clkrst, g_clock_ready, g_raised;
static ClkrstSession g_cpu;

static int clkrst_open(void) {
    if (!g_clkrst)
        g_clkrst = R_SUCCEEDED(clkrstInitialize()) ? 1 : -1;
    return g_clkrst > 0;
}

static int clock_open(void) {
    if (g_clock_ready)
        return g_clock_ready > 0;
    g_clock_ready = -1;
    if (!clkrst_open())
        return 0;
    if (R_FAILED(clkrstOpenSession(&g_cpu, PcvModuleId_CpuBus, 3)))
        return 0;
    if (R_FAILED(clkrstGetClockRate(&g_cpu, &g_launch_hz)) || g_launch_hz >= kMaxHz) {
        clkrstCloseSession(&g_cpu);
        return 0;
    }
    const char *want = getenv("KAIHEN_CPU_MHZ");
    if (want && strcmp(want, "max") == 0)
        g_cpu_chosen_mhz = kMaxHz / 1000000u;
    else if (want && *want)
        g_cpu_chosen_mhz = (uint32_t)strtoul(want, NULL, 10);
    if (g_cpu_chosen_mhz > kMaxHz / 1000000u)
        g_cpu_chosen_mhz = kMaxHz / 1000000u;
    g_clock_ready = 1;
    return 1;
}

/* The CPU rate for a choice: 0 is the launch rate; 1702 means 1702.4. */
static uint32_t cpu_rate(uint32_t mhz) {
    if (mhz == 0)
        return g_launch_hz;
    const uint32_t hz = mhz * 1000000u;
    return hz + 999999u >= kMaxHz ? kMaxHz : hz;
}

typedef struct {
    const char *name, *setting;
    PcvModuleId module;
    uint32_t handheld_mhz; /* the console's own rate in handheld mode, on battery */
    ClkrstSession session;
    int open;
    uint32_t launch_hz, lo, hi, chosen_mhz, applied_hz, now_hz;
    PcvClockRatesListType type;
    s32 count;
    u32 rates[32];
} ExtraClock;

static ExtraClock g_extra[] = {
    {"memory", "KAIHEN_MEMORY_MHZ", PcvModuleId_EMC, 2133},
    {"GPU", "KAIHEN_GPU_MHZ", PcvModuleId_GPU, 561},
};
enum { kExtraClocks = sizeof g_extra / sizeof g_extra[0] };

/* 1: a charger that supplies enough power, 0: battery (or not known). Set by power_check. */
static int g_psm, g_plugged = -1;
static uint32_t g_cpu_now_hz;

static int power_plugged(void) {
    if (!g_psm)
        g_psm = R_SUCCEEDED(psmInitialize()) ? 1 : -1;
    if (g_psm < 0)
        return 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    bool enough = false;
    if (R_FAILED(psmGetChargerType(&charger)) || R_FAILED(psmIsEnoughPowerSupplied(&enough)))
        return 0;
    return charger != PsmChargerType_Unconnected && enough;
}

/* The rate the system will take for a choice in MHz: clamped to its range, or the highest listed
 * step at or below it ("1389" matches a 1389.x step). */
static uint32_t extra_rate(const ExtraClock *c, uint32_t mhz) {
    const uint32_t hz = mhz * 1000000u, slack = 999999u;
    if (c->count <= 0)
        return hz;
    if (c->type == PcvClockRatesListType_Range && c->count >= 2)
        return hz < c->lo ? c->lo : hz > c->hi ? c->hi : hz;
    uint32_t best = 0;
    for (s32 i = 0; i < c->count && i < 32; i++)
        if (c->rates[i] <= hz + slack && c->rates[i] > best)
            best = c->rates[i];
    return best ? best : c->lo;
}

static int extra_open(ExtraClock *c) {
    if (c->open)
        return c->open > 0;
    c->open = -1;
    if (!clkrst_open() || R_FAILED(clkrstOpenSession(&c->session, c->module, 3)))
        return 0;
    if (R_FAILED(clkrstGetClockRate(&c->session, &c->launch_hz))) {
        clkrstCloseSession(&c->session);
        return 0;
    }
    c->applied_hz = c->now_hz = c->launch_hz;
    if (R_SUCCEEDED(clkrstGetPossibleClockRates(&c->session, c->rates, 32, &c->type, &c->count)) && c->count > 0) {
        const int range = c->type == PcvClockRatesListType_Range && c->count >= 2;
        const s32 n = range ? 2 : (c->count < 32 ? c->count : 32);
        c->lo = c->hi = c->rates[0];
        for (s32 i = 1; i < n; i++) {
            c->lo = c->rates[i] < c->lo ? c->rates[i] : c->lo;
            c->hi = c->rates[i] > c->hi ? c->rates[i] : c->hi;
        }
    } else {
        c->count = 0;
    }
    c->open = 1;
    return 1;
}

/* The system's rate: the launch rate, but on battery no higher than the console's handheld rate. A
 * clock can be left raised at launch (an app closed from the HOME Menu runs none of its exit code),
 * and on battery a raised memory or GPU clock halves the CPU's real speed. */
static uint32_t extra_system_rate(const ExtraClock *c) {
    if (g_plugged > 0)
        return c->launch_hz;
    const uint32_t handheld = extra_rate(c, c->handheld_mhz);
    return c->launch_hz > handheld ? handheld : c->launch_hz;
}

/* Puts a clock at its chosen rate when plugged in, at the system's rate otherwise. Call with
 * g_clock_lock held. */
static void extra_update(ExtraClock *c) {
    if (c->open <= 0)
        return;
    const uint32_t target = c->chosen_mhz && g_plugged > 0 ? extra_rate(c, c->chosen_mhz) : extra_system_rate(c);
    if (target == c->applied_hz)
        return;
    const Result rc = clkrstSetClockRate(&c->session, target);
    clkrstGetClockRate(&c->session, &c->now_hz);
    if (R_SUCCEEDED(rc))
        c->applied_hz = target;
    fprintf(stderr, "[kaihen] %s clock %.1f MHz (%s; rc 0x%x), reads %.1f MHz\n", c->name, target / 1e6,
            c->chosen_mhz == 0 ? "the system's" : g_plugged > 0 ? "chosen" : "the system's on battery", (unsigned)rc,
            c->now_hz / 1e6);
}

static uint32_t parse_mhz(const char *want, uint32_t max_hz) {
    if (!want || !*want)
        return 0;
    if (strcmp(want, "max") == 0)
        return max_hz / 1000000u;
    const unsigned long mhz = strtoul(want, NULL, 10);
    return mhz > 4000 ? 0 : (uint32_t)mhz;
}

static void extra_clocks_set(void) {
    mutexLock(&g_clock_lock);
    g_plugged = power_plugged();
    fprintf(stderr, "[kaihen] power: %s\n", g_plugged ? "plugged in" : "battery");
    for (int i = 0; i < kExtraClocks; i++) {
        ExtraClock *c = &g_extra[i];
        if (!extra_open(c)) {
            fprintf(stderr, "[kaihen] %s clock: no session\n", c->name);
            continue;
        }
        c->chosen_mhz = parse_mhz(getenv(c->setting), c->hi);
        fprintf(stderr, "[kaihen] %s clock %.1f MHz at launch, chosen %u MHz (offers %.1f-%.1f)\n", c->name,
                c->launch_hz / 1e6, (unsigned)c->chosen_mhz, c->lo / 1e6, c->hi / 1e6);
        extra_update(c);
    }
    mutexUnlock(&g_clock_lock);
}

/* For the log, so a slowdown can be matched to the power: the battery each time it crosses a 10% step,
 * its voltage state when that changes (at "no performance boost" the console caps the clocks), and
 * the CPU whenever it reads below the rate this app set. Nothing is written while all is normal. */
static int g_battery_step = -1, g_voltage_state = -1;
static uint32_t g_cpu_low_hz;

static const char *voltage_state_name(PsmBatteryVoltageState state) {
    switch (state) {
    case PsmBatteryVoltageState_NeedsShutdown: return "needs shutdown";
    case PsmBatteryVoltageState_NeedsSleep: return "needs sleep";
    case PsmBatteryVoltageState_NoPerformanceBoost: return "no performance boost";
    case PsmBatteryVoltageState_Normal: return "normal";
    default: return "unknown";
    }
}

/* Call with g_clock_lock held, after the CPU's rate was read. */
static void power_log(void) {
    u32 percent = 0;
    if (g_psm > 0 && R_SUCCEEDED(psmGetBatteryChargePercentage(&percent)) && (int)(percent / 10) != g_battery_step) {
        g_battery_step = (int)(percent / 10);
        fprintf(stderr, "[kaihen] battery %u%%\n", (unsigned)percent);
    }
    PsmBatteryVoltageState voltage;
    if (g_psm > 0 && R_SUCCEEDED(psmGetBatteryVoltageState(&voltage)) && (int)voltage != g_voltage_state) {
        g_voltage_state = (int)voltage;
        fprintf(stderr, "[kaihen] battery state: %s\n", voltage_state_name(voltage));
    }
    if (g_clock_ready <= 0 || g_cpu_now_hz == 0)
        return;
    const uint32_t set_hz = g_raised ? cpu_rate(g_cpu_chosen_mhz) : g_launch_hz;
    if (g_cpu_now_hz + 1000000u < set_hz) {
        if (g_cpu_now_hz != g_cpu_low_hz)
            fprintf(stderr, "[kaihen] CPU reads %.1f MHz, set to %.1f MHz (battery %u%%, %s)\n", g_cpu_now_hz / 1e6,
                    set_hz / 1e6, (unsigned)percent, g_plugged > 0 ? "plugged in" : "battery");
        g_cpu_low_hz = g_cpu_now_hz;
    } else if (g_cpu_low_hz) {
        fprintf(stderr, "[kaihen] CPU back to %.1f MHz\n", g_cpu_now_hz / 1e6);
        g_cpu_low_hz = 0;
    }
}

static int hz_differ(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) > 2000000u; }

/* The system puts the clocks back to its own rates by itself, for example after sleep or the HOME
 * Menu: on 2026-11-23 the CPU read 1100.8 MHz for the rest of a session after a visit to the
 * album. Unlike the battery halving, clkrst then reads the lower rate, so this puts back the rates
 * this app set whenever a reading differs from them. Only clocks set to a chosen rate: at the
 * system's own rate, the system decides. Call with g_clock_lock held, after the rates were read. */
static int g_put_back_logged;

static void clocks_put_back(void) {
    for (int i = 0; i < kExtraClocks; i++) {
        ExtraClock *c = &g_extra[i];
        if (c->open <= 0 || !c->chosen_mhz || !c->applied_hz || !c->now_hz || !hz_differ(c->now_hz, c->applied_hz))
            continue;
        if (g_put_back_logged < 20) {
            g_put_back_logged++;
            fprintf(stderr, "[kaihen] %s clock reads %.1f MHz, set to %.1f MHz: setting it again\n", c->name,
                    c->now_hz / 1e6, c->applied_hz / 1e6);
        }
        c->applied_hz = 0;
        extra_update(c);
    }
    if (g_clock_ready <= 0 || !g_raised || !g_cpu_now_hz)
        return;
    const uint32_t want = cpu_rate(g_cpu_chosen_mhz);
    if (!hz_differ(g_cpu_now_hz, want))
        return;
    const Result rc = clkrstSetClockRate(&g_cpu, want);
    uint32_t now = 0;
    clkrstGetClockRate(&g_cpu, &now);
    if (g_put_back_logged < 20) {
        g_put_back_logged++;
        fprintf(stderr, "[kaihen] CPU clock read %.1f MHz, set to %.1f MHz: set it again (rc 0x%x), reads %.1f MHz\n",
                g_cpu_now_hz / 1e6, want / 1e6, (unsigned)rc, now / 1e6);
    }
    g_cpu_now_hz = now;
}

/* Once a second, from the watchdog: the power state, and each clock's rate for the menu. */
static void power_check(void) {
    const int plugged = power_plugged();
    mutexLock(&g_clock_lock);
    if (plugged != g_plugged) {
        g_plugged = plugged;
        fprintf(stderr, "[kaihen] power: %s\n", plugged ? "plugged in" : "battery");
        for (int i = 0; i < kExtraClocks; i++)
            extra_update(&g_extra[i]);
    }
    for (int i = 0; i < kExtraClocks; i++)
        if (g_extra[i].open > 0)
            clkrstGetClockRate(&g_extra[i].session, &g_extra[i].now_hz);
    if (g_clock_ready > 0)
        clkrstGetClockRate(&g_cpu, &g_cpu_now_hz);
    clocks_put_back();
    power_log();
    mutexUnlock(&g_clock_lock);
}

/* Closes the psm session once the watchdog has stopped: a return to hbmenu with it still open ended
 * in "the software needed to close". */
static void power_close(void) {
    if (g_psm > 0)
        psmExit();
    g_psm = -1;
}

static void extra_clocks_restore(void) {
    mutexLock(&g_clock_lock);
    for (int i = 0; i < kExtraClocks; i++) {
        ExtraClock *c = &g_extra[i];
        if (c->open <= 0)
            continue;
        const uint32_t system = extra_system_rate(c);
        if (c->applied_hz != system)
            clkrstSetClockRate(&c->session, system);
        uint32_t now = 0;
        clkrstGetClockRate(&c->session, &now);
        fprintf(stderr, "[kaihen] at exit: %s clock back to %.1f MHz, reads %.1f\n", c->name, system / 1e6,
                now / 1e6);
        clkrstCloseSession(&c->session);
        c->open = 0;
    }
    mutexUnlock(&g_clock_lock);
}

/* settings.env with one KEY=VALUE line set: an existing line (commented out or not) is replaced. */
static void save_setting(const char *key, const char *value) {
    FILE *f = fopen(KAIHEN_SETTINGS, "r");
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) {
        if (f)
            fclose(f);
        return;
    }
    const size_t key_len = strlen(key);
    int written = 0;
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        const char *name = line;
        while (*name == '#' || *name == ' ')
            name++;
        if (strncmp(name, key, key_len) == 0 && name[key_len] == '=') {
            if (!written)
                fprintf(out, "%s=%s\n", key, value);
            written = 1;
            continue;
        }
        fputs(line, out);
    }
    if (f)
        fclose(f);
    if (!written)
        fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    f = fopen(KAIHEN_SETTINGS, "w");
    if (f) {
        fwrite(text, 1, size, f);
        fclose(f);
    }
    free(text);
}

static void save_mhz(const char *key, uint32_t mhz) {
    char value[16];
    snprintf(value, sizeof value, "%u", (unsigned)mhz);
    save_setting(key, value);
}

/* For the port menu. Clock 0 is memory, 1 the GPU; the CPU has its own calls. Rates read are the
 * watchdog's from the last second: no system call on the game thread. */
int kaihen_clock_count(void) { return kExtraClocks; }
const char *kaihen_clock_name(int i) { return i >= 0 && i < kExtraClocks ? g_extra[i].name : ""; }
uint32_t kaihen_clock_chosen_mhz(int i) { return i >= 0 && i < kExtraClocks ? g_extra[i].chosen_mhz : 0; }
uint32_t kaihen_clock_max_mhz(int i) {
    return i >= 0 && i < kExtraClocks && g_extra[i].open > 0 ? g_extra[i].hi / 1000000u : 0;
}
uint32_t kaihen_clock_now_hz(int i) { return i >= 0 && i < kExtraClocks ? g_extra[i].now_hz : 0; }
int kaihen_power_plugged(void) { return g_plugged > 0; }

/* What a player holds, for the port menu's button names: 0 nothing, 1 handheld, 2 Pro Controller,
 * 3 Joy-Con pair, 4 left Joy-Con alone (sideways), 5 right Joy-Con alone. Player 1 is No1 or
 * handheld, as in the SDL driver (kaihen/patches/sdl3-kaihen.diff). */
int kaihen_player_controller(int player) {
    if (player < 0 || player > 7)
        return 0;
    const u32 style = hidGetNpadStyleSet((HidNpadIdType)(HidNpadIdType_No1 + player));
    if (style & HidNpadStyleTag_NpadJoyLeft)
        return 4;
    if (style & HidNpadStyleTag_NpadJoyRight)
        return 5;
    if (style & HidNpadStyleTag_NpadJoyDual)
        return 3;
    if (style & HidNpadStyleTag_NpadFullKey)
        return 2;
    if (style)
        return 2;
    if (player == 0 && hidGetNpadStyleSet(HidNpadIdType_Handheld))
        return 1;
    return 0;
}

int kaihen_clock_choose_mhz(int i, uint32_t mhz) {
    if (i < 0 || i >= kExtraClocks)
        return -1;
    mutexLock(&g_clock_lock);
    g_extra[i].chosen_mhz = mhz;
    extra_update(&g_extra[i]);
    mutexUnlock(&g_clock_lock);
    save_mhz(g_extra[i].setting, mhz);
    return 0;
}

uint32_t kaihen_cpu_clock_chosen_mhz(void) { return clock_open() ? g_cpu_chosen_mhz : 0; }
uint32_t kaihen_cpu_clock_now_hz(void) { return g_cpu_now_hz; }

/* The runtime's CPU boost (settings_overlay.cpp ConsoleSetCpuBoost): on is the chosen rate. */
int hbxl_cpu_clock_rates(uint32_t *launch_hz, uint32_t *max_hz) {
    if (!clock_open())
        return 0;
    *launch_hz = g_launch_hz;
    *max_hz = cpu_rate(g_cpu_chosen_mhz);
    return 1;
}

long hbxl_cpu_clock_boost(int on) {
    if (!clock_open())
        return -ENODEV;
    const uint32_t want = on ? cpu_rate(g_cpu_chosen_mhz) : g_launch_hz;
    uint32_t now = 0;
    mutexLock(&g_clock_lock);
    const Result rc = clkrstSetClockRate(&g_cpu, want);
    if (R_SUCCEEDED(rc))
        clkrstGetClockRate(&g_cpu, &now);
    g_cpu_now_hz = now;
    mutexUnlock(&g_clock_lock);
    if (R_FAILED(rc))
        return -EIO;
    g_raised = on && want != g_launch_hz;
    return 0;
}

int kaihen_cpu_clock_choose_mhz(uint32_t mhz) {
    if (!clock_open())
        return -1;
    g_cpu_chosen_mhz = mhz;
    save_mhz("KAIHEN_CPU_MHZ", mhz);
    const long rc = hbxl_cpu_clock_boost(1);
    fprintf(stderr, "[kaihen] CPU clock %.1f MHz (%s), reads %.1f MHz\n", cpu_rate(mhz) / 1e6,
            mhz ? "chosen" : "the system's", g_cpu_now_hz / 1e6);
    return (int)rc;
}

void hbxl_cpu_clock_restore(void) {
    extra_clocks_restore();
    if (g_clock_ready > 0 && g_raised)
        hbxl_cpu_clock_boost(0);
}

/* --- randomness for mbed TLS (kaihen_mbedtls_config.h) -------------------------------------- */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    randomGet(output, len);
    *olen = len;
    return 0;
}

/* --- mbed TLS's millisecond clock (MBEDTLS_PLATFORM_MS_TIME_ALT) ---------------------------- */

int64_t mbedtls_ms_time(void) { return (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000); }

/* --- POSIX calls newlib declares and libnx doesn't provide ---------------------------------- */
/* The app has no signals, child processes or users. These answer the way a system without them
 * would, so SDL, the runtime and aurora take their fallbacks. */

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

int sigaction(int sig, const struct sigaction *action, struct sigaction *old) {
    (void)sig, (void)action;
    if (old)
        memset(old, 0, sizeof *old);
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    (void)how, (void)set;
    if (old)
        memset(old, 0, sizeof *old);
    return 0;
}

int pthread_setname_np(pthread_t thread, const char *name) {
    (void)thread, (void)name;
    return 0;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid, (void)status, (void)options;
    errno = ECHILD;
    return -1;
}

int pipe(int fds[2]) {
    (void)fds;
    errno = ENOSYS;
    return -1;
}

int execvp(const char *file, char *const argv[]) {
    (void)file, (void)argv;
    errno = ENOSYS;
    return -1;
}

uid_t geteuid(void) { return 0; }

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd, (void)owner, (void)group;
    return 0;
}

int fdatasync(int fd) { return fsync(fd); }

int getpagesize(void) { return 0x1000; }

/* --- diagnostics: every C++ throw, with a backtrace (link: --wrap=__cxa_throw) --------------- */
/* Addresses are absolute: subtract the load address (0x8000000 so far) for addr2line on the ELF.
 * The first two throws of each type are logged (config lookups throw out_of_range routinely). */

#include <unwind.h>

void __real___cxa_throw(void *object, void *type, void (*destructor)(void *)) __attribute__((noreturn));

static _Unwind_Reason_Code print_frame(struct _Unwind_Context *context, void *arg) {
    int *depth = arg;
    fprintf(stderr, "[kaihen]   #%d 0x%lx\n", *depth, (unsigned long)_Unwind_GetIP(context));
    return ++*depth < 16 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void __wrap___cxa_throw(void *object, void *type, void (*destructor)(void *)) {
    static const void *seen[64];
    static int seen_count[64], types;
    int slot = 0;
    while (slot < types && seen[slot] != type)
        slot++;
    if (slot == types && types < 64)
        seen[types++] = type;
    if (slot < 64 && seen_count[slot]++ < 2) {
        /* std::type_info: a vtable pointer, then the mangled name. */
        fprintf(stderr, "[kaihen] C++ throw %s\n", ((const char *const *)type)[1]);
        int depth = 0;
        _Unwind_Backtrace(print_frame, &depth);
    }
    __real___cxa_throw(object, type, destructor);
}

/* --- leaving (link: --wrap=exit, --wrap=_exit) ----------------------------------------------- */
/* Every exit ends the process through quit_now: tearing the game down while its GPU and worker
 * threads still run crashes ("the software needed to close"). */

void __wrap_exit(int status) {
    quit_now(status == 0 ? "exit(0)" : status == 1 ? "exit(1)" : "exit(failure)");
}

/* std::_Exit (the runtime's fatal paths) reaches _exit directly. */

void __wrap__exit(int status) {
    quit_now(status == 0 ? "_exit(0)" : status == 1 ? "_exit(1)" : "_exit(failure)");
}

/* --- games in one kaiHEN session --------------------------------------------------------------- */
/* hbmenu, the KaiKart launcher and the games all run in one process, so a game can follow another
 * in it (a return to hbmenu). The log says which game of the session this is, for bug reports: a
 * game notes the process ID, the console's tick and the count in UserData/session.txt. The tick
 * tells a new boot apart, where process IDs start over. */
static void count_session_games(void) {
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    const u64 now = armGetSystemTick();
    unsigned long long saved_pid = 0, saved_tick = 0;
    int games = 0;
    FILE *file = fopen(KAIHEN_ROOT "/UserData/session.txt", "r");
    if (file) {
        if (fscanf(file, "%llu %llu %d", &saved_pid, &saved_tick, &games) < 2 || saved_pid != pid || saved_tick > now)
            games = 0;
        else if (games < 1)
            games = 1; /* written before the count was kept */
        fclose(file);
    }
    g_games_before = games;
    if ((file = fopen(KAIHEN_ROOT "/UserData/session.txt", "w"))) {
        fprintf(file, "%llu %llu %d\n", (unsigned long long)pid,
                (unsigned long long)(games ? saved_tick : now), games + 1);
        fclose(file);
    }
}

/* The card's file system has no permissions, and libnx's chmod and fchmod return ENOSYS. libstdc++'s
 * std::filesystem::copy_file sets the copy's permissions before copying its data, so every copy failed
 * and left an empty file (a new NAND's first files, NANDSafeOpen's scratch copy). Unsupported counts
 * as done (link: --wrap=chmod, --wrap=fchmod). */
int __real_chmod(const char *path, mode_t mode);
int __real_fchmod(int fd, mode_t mode);

int __wrap_chmod(const char *path, mode_t mode) {
    const int result = __real_chmod(path, mode);
    if (result != 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        errno = 0;
        return 0;
    }
    return result;
}

int __wrap_fchmod(int fd, mode_t mode) {
    const int result = __real_fchmod(fd, mode);
    if (result != 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        errno = 0;
        return 0;
    }
    return result;
}

/* libnx's pthread_detach returns ENOSYS, which std::thread::detach turns into an exception
 * (link: --wrap=pthread_detach). A detached thread also has to be closed once it ends, as
 * pthread_join does (libnx's join waits, then threadClose and frees the record): otherwise it stays
 * half-closed, and kaiHEN's hbloader, which tidies those at a return to hbmenu, crashed doing it
 * after online play (its lookups and connections run on detached threads; 2026-11-18). So each
 * detached thread is kept here and joined once it has ended: when another thread is detached, and
 * at a return (kaihen_close_ended_threads). A libnx pthread_t starts with its Thread. */
enum { kMaxDetached = 256 };
static Mutex g_detached_lock;
static pthread_t g_detached[kMaxDetached];
static int g_detached_count;

static int thread_ended(pthread_t thread) {
    const Handle handle = ((const Thread *)(const void *)thread)->handle;
    return R_SUCCEEDED(svcWaitSynchronizationSingle(handle, 0));
}

/* Under g_detached_lock: joins the detached threads that have ended. How many it joined. */
static int join_ended_locked(void) {
    int joined = 0;
    for (int i = 0; i < g_detached_count;) {
        if (thread_ended(g_detached[i])) {
            pthread_join(g_detached[i], NULL);
            g_detached[i] = g_detached[--g_detached_count];
            joined++;
        } else {
            i++;
        }
    }
    return joined;
}

int __wrap_pthread_detach(pthread_t thread) {
    mutexLock(&g_detached_lock);
    join_ended_locked();
    if (g_detached_count < kMaxDetached)
        g_detached[g_detached_count++] = thread;
    mutexUnlock(&g_detached_lock);
    return 0;
}

/* At a return to hbmenu: the detached threads that have ended, closed; how many still run. A worker
 * just told to stop may still be on its way out, so this waits up to half a second for them. */
static int kaihen_close_ended_threads(int *still_running) {
    int joined = 0;
    for (int tries = 0; tries < 50; tries++) {
        mutexLock(&g_detached_lock);
        joined += join_ended_locked();
        *still_running = g_detached_count;
        mutexUnlock(&g_detached_lock);
        if (*still_running == 0)
            break;
        svcSleepThread(10000000ull);
    }
    return joined;
}
