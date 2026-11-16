/* The kaiHEN platform layer (native Switch 2 homebrew on libounce, libnx for Switch 2): what the
 * shared Switch 2 runtime expects from the system. The game's files are in sdmc:/wiicompiled/
 * (kaihen/sd.py): portable.txt, settings.env, UserData/, disc/ and RetroRewind/.
 *   - startup (userAppInit, before main): sockets, the log, the settings in settings.env, the folder;
 *   - the screen: libnx's default NWindow, which Dawn presents to through VK_NN_vi_surface;
 *   - Vulkan: OunceVK, linked in;
 *   - the CPU, GPU and memory clocks: clkrst, directly (the app may use it on kaiHEN), and the
 *     power state (psm): memory and GPU run raised only on a charger;
 *   - input and audio: SDL's kaiHEN drivers (kaihen/patches/sdl3-kaihen.diff), on libnx's pad and
 *     audout; this file configures the pad at startup. */
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
            return;
        write(g_log_fd, chunk, n);
    }
}

static void log_thread(void *arg) {
    (void)arg;
    copy_thread_pointer();
    while (!g_log_stop) {
        svcSleepThread(200000000ull);
        log_drain();
    }
}

static void log_start(void) {
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
int kaihen_in_race(void); /* runtime/src/hle/vi.cpp */
void userAppInit(void);

static u64 *g_samples;
static size_t g_sample_count; /* words */
enum { PROFILE_FRAMES = 8, PROFILE_WORDS = 3 + PROFILE_FRAMES, PROFILE_SAMPLES = 200000 };
static Thread g_profiler;
static volatile int g_profiler_stop;

static void profiler(void *arg) {
    (void)arg;
    copy_thread_pointer();
    const Handle game = envGetMainThreadHandle();
    while (!g_profiler_stop) {
        svcSleepThread(1000000ull);
        if (!kaihen_in_race() || g_sample_count + PROFILE_WORDS > (size_t)PROFILE_SAMPLES * PROFILE_WORDS)
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
    if (!on || strcmp(on, "1") != 0)
        return;
    g_samples = malloc((size_t)PROFILE_SAMPLES * PROFILE_WORDS * sizeof *g_samples);
    if (g_samples && R_SUCCEEDED(threadCreate(&g_profiler, profiler, NULL, NULL, 0x4000, 0x2C, core)))
        threadStart(&g_profiler);
    fprintf(stderr, "[kaihen] profiling races on core %d (userAppInit at %p)\n", core, (void *)userAppInit);
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
 * hbmenu again. It parks the threads a program leaves running (pauses them for good and gives their
 * stacks back to the heap), so the game's threads are left to it: pausing them here first makes its
 * own pause fail, and then it ends the process. The runtime shuts down graphics, audio and its GX
 * worker first (settings_overlay.cpp ConsoleReturnToLauncher). */

static volatile int g_return_requested;
int kaihen_return_requested(void) { return g_return_requested; }

/* The game's other threads still run until hbloader parks them and may still log: userAppExit
 * leaves the log open on this path. */
static volatile int g_returning;
static int g_exit_fd = -1; /* UserData/Logs/exit.txt while returning (step) */
static void step(const char *name);
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
    fprintf(stderr, "[kaihen] returning to hbmenu\n");
    profiler_finish();
    fflush(NULL);
    log_finish();
    g_exit_fd = open(KAIHEN_ROOT "/UserData/Logs/exit.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    step("log closed");
    __libnx_exit(0); /* services closed (userAppExit too, and the GPU driver's), then back to hbloader */
}

/* Until the game handles the console's applet messages (milestone 3): a thread that services the
 * applet. Holding + and - for a second asks the game to return to hbmenu between frames; if no
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

void userAppInit(void) {
    copy_thread_pointer();
    cores_start();
    socketInitializeDefault();
    /* With nxlink -s, stdout and stderr go to the PC; otherwise to the card. */
    g_nxlink = nxlinkStdio();
    mkdir(KAIHEN_ROOT "/UserData", 0777);
    mkdir(KAIHEN_ROOT "/UserData/Logs", 0777);
    if (g_nxlink < 0)
        log_start();
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    load_settings();
    extra_clocks_set();
    sqlite_setup();
    /* SDL draws nothing itself (aurora presents through Dawn to the NWindow), so its offscreen
     * video driver. settings.env wins. */
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    if (chdir(KAIHEN_ROOT) != 0)
        fprintf(stderr, "[kaihen] no %s on the card (%d)\n", KAIHEN_ROOT, errno);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    if (R_SUCCEEDED(threadCreate(&g_watchdog, watchdog, NULL, NULL, 0x10000, 0x2C, -2)))
        threadStart(&g_watchdog);
    fprintf(stderr, "[kaihen] started, nxlink %s; game thread on core %d of mask %lx; hold + and - to quit\n",
            g_nxlink >= 0 ? "on" : "off", g_game_core, (unsigned long)g_cores);
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

/* --- screen and Vulkan -------------------------------------------------------------------- */

void *hbx_switch2_vi_window(void) {
    NWindow *window = nwindowGetDefault();
    u32 width = 0, height = 0;
    nwindowGetDimensions(window, &width, &height);
    fprintf(stderr, "[kaihen] screen window %ux%u\n", width, height);
    return window;
}

/* Dawn's Switch path asks the driver for its entry point the way Mesa's ICD exports it. */
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    return vkGetInstanceProcAddr(instance, name);
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
    ClkrstSession session;
    int open;
    uint32_t launch_hz, lo, hi, chosen_mhz, applied_hz, now_hz;
    PcvClockRatesListType type;
    s32 count;
    u32 rates[32];
} ExtraClock;

static ExtraClock g_extra[] = {
    {"memory", "KAIHEN_MEMORY_MHZ", PcvModuleId_EMC},
    {"GPU", "KAIHEN_GPU_MHZ", PcvModuleId_GPU},
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

/* Puts a clock at its chosen rate when plugged in, at the launch rate otherwise. Call with
 * g_clock_lock held. */
static void extra_update(ExtraClock *c) {
    if (c->open <= 0)
        return;
    const uint32_t target = c->chosen_mhz && g_plugged > 0 ? extra_rate(c, c->chosen_mhz) : c->launch_hz;
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
        if (c->applied_hz != c->launch_hz)
            clkrstSetClockRate(&c->session, c->launch_hz);
        uint32_t now = 0;
        clkrstGetClockRate(&c->session, &now);
        fprintf(stderr, "[kaihen] at exit: %s clock back to %.1f MHz, reads %.1f\n", c->name, c->launch_hz / 1e6,
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

void kaihen_quit(const char *why) { quit_now(why); }

void __wrap_exit(int status) {
    quit_now(status == 0 ? "exit(0)" : status == 1 ? "exit(1)" : "exit(failure)");
}

/* std::_Exit (the runtime's fatal paths) reaches _exit directly. */

void __wrap__exit(int status) {
    quit_now(status == 0 ? "_exit(0)" : status == 1 ? "_exit(1)" : "_exit(failure)");
}

/* libnx's pthread_detach returns ENOSYS, which std::thread::detach turns into an exception. The
 * runtime detaches only long-lived workers it starts once (video decode, watchdogs), so succeeding
 * is enough: the thread runs on, and its record isn't reclaimed (link: --wrap=pthread_detach). */
int __wrap_pthread_detach(pthread_t thread) {
    (void)thread;
    return 0;
}
