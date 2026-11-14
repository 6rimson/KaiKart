/* kaiHEN (native Switch 2 homebrew) stand-ins for what HBLauncher's runtime gave the game, on
 * libounce (libnx for Switch 2). The game's files are those of the HBLauncher microSD layout:
 * sdmc:/wiicompiled/ holds portable.txt, UserData/, disc/ and RetroRewind/.
 *   - startup (userAppInit, before main): sockets, the log, the settings in hbxl-env, the folder;
 *   - the screen: libnx's default NWindow, which Dawn presents to through VK_NN_vi_surface;
 *   - Vulkan: OunceVK, linked in;
 *   - the CPU clock: clkrst, directly (the app may use it on kaiHEN);
 *   - input and audio: SDL's HBLauncher drivers, fed from libnx (hid and audout) through an
 *     hbx context of this file's own. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <switch.h>

#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>

#include "hbx_app.h" /* HBLauncher SDK: struct hbx_ctx, struct hbx_input_state */
#include "hbx_sdk.h"

#define KAIHEN_ROOT "sdmc:/wiicompiled"

/* SDL's HBLauncher drivers read this: the joystick driver needs it set, the audio driver calls
 * its audio table (filled below). Nothing else in it is used. */
static struct hbx_ctx g_ctx;
const struct hbx_ctx *hbxl_ctx = &g_ctx;

static FILE *g_log;
static int g_nxlink = -1;

/* --- startup ------------------------------------------------------------------------------ */

/* hbxl-env: KEY=VALUE lines, # comments, as HBLauncher reads them. */
static void load_settings(void) {
    FILE *f = fopen(KAIHEN_ROOT "/hbxl-env", "r");
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
    fflush(NULL);
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

/* Until the game handles the console's applet messages and input (milestone 3): a thread that
 * services the applet, and quits the game when + and - are held together for a second. */
static Thread g_watchdog;

static void watchdog(void *arg) {
    (void)arg;
    PadState pad;
    padInitializeDefault(&pad);
    int held = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 buttons = padGetButtons(&pad);
        held = (buttons & HidNpadButton_Plus) && (buttons & HidNpadButton_Minus) ? held + 1 : 0;
        if (held >= 20)
            break;
        svcSleepThread(50000000ull);
    }
    quit_now("+ and - held");
}

void userAppInit(void) {
    socketInitializeDefault();
    /* With nxlink -s, stdout and stderr go to the PC; otherwise to the card. */
    g_nxlink = nxlinkStdio();
    mkdir(KAIHEN_ROOT "/UserData", 0777);
    mkdir(KAIHEN_ROOT "/UserData/Logs", 0777);
    g_log = fopen(KAIHEN_ROOT "/UserData/Logs/kaihen.log", "w");
    if (g_nxlink < 0 && g_log) {
        fflush(stdout);
        dup2(fileno(g_log), STDOUT_FILENO);
        dup2(fileno(g_log), STDERR_FILENO);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    load_settings();
    sqlite_setup();
    /* As HBLauncher's environment: SDL draws nothing itself (aurora presents through Dawn to the
     * NWindow), so its offscreen video driver. hbxl-env wins. */
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    if (chdir(KAIHEN_ROOT) != 0)
        fprintf(stderr, "[kaihen] no %s on the card (%d)\n", KAIHEN_ROOT, errno);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    if (R_SUCCEEDED(threadCreate(&g_watchdog, watchdog, NULL, NULL, 0x10000, 0x2C, -2)))
        threadStart(&g_watchdog);
    fprintf(stderr, "[kaihen] started, nxlink %s; hold + and - to quit\n", g_nxlink >= 0 ? "on" : "off");
}

void userAppExit(void) {
    fprintf(stderr, "[kaihen] exit\n");
    if (g_log)
        fclose(g_log);
    if (g_nxlink >= 0)
        close(g_nxlink);
    socketExit();
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

/* --- input: the controller, through libnx ------------------------------------------------ */
/* Player 1 or handheld, as the HBLauncher SDK reported them: HBX_* bits, sticks -100..100 with +y
 * up, a small deadzone. SDL's hbx joystick driver turns this into a Switch Pro style gamepad. */

static int32_t stick_axis(s32 value) {
    int32_t v = (int32_t)((int64_t)value * 100 / 32767);
    if (v > -8 && v < 8)
        return 0;
    return v > 100 ? 100 : v < -100 ? -100 : v;
}

void hbx_input_poll(const struct hbx_ctx *ctx, struct hbx_input_state *state) {
    static PadState pad;
    static int ready;
    (void)ctx;
    if (!ready) {
        padInitializeDefault(&pad);
        ready = 1;
    }
    padUpdate(&pad);
    static const struct { u64 nx; uint32_t hbx; } map[] = {
        {HidNpadButton_A, HBX_A},         {HidNpadButton_B, HBX_B},
        {HidNpadButton_X, HBX_X},         {HidNpadButton_Y, HBX_Y},
        {HidNpadButton_L, HBX_L},         {HidNpadButton_R, HBX_R},
        {HidNpadButton_ZL, HBX_ZL},       {HidNpadButton_ZR, HBX_ZR},
        {HidNpadButton_Plus, HBX_PLUS},   {HidNpadButton_Minus, HBX_MINUS},
        {HidNpadButton_StickL, HBX_STICK_L}, {HidNpadButton_StickR, HBX_STICK_R},
        {HidNpadButton_Up, HBX_UP},       {HidNpadButton_Down, HBX_DOWN},
        {HidNpadButton_Left, HBX_LEFT},   {HidNpadButton_Right, HBX_RIGHT},
    };
    const u64 held = padGetButtons(&pad);
    uint32_t buttons = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (held & map[i].nx)
            buttons |= map[i].hbx;
    const HidAnalogStickState left = padGetStickPos(&pad, 0), right = padGetStickPos(&pad, 1);
    state->pressed = buttons & ~state->previous;
    state->released = state->previous & ~buttons;
    state->previous = buttons;
    state->buttons = buttons;
    state->stick_lx = stick_axis(left.x);
    state->stick_ly = stick_axis(left.y);
    state->stick_rx = stick_axis(right.x);
    state->stick_ry = stick_axis(right.y);
    state->connected = padIsConnected(&pad) ? 1 : 0;
    state->touch_count = 0;
}

/* --- audio: audout, behind the AudioOut table SDL's hbx driver calls ------------------------- */
/* The driver keeps the "object" and descriptors in its own memory; here a descriptor is a libnx
 * AudioOutBuffer (40 of its 64 bytes) and the object is unused: there is one AudioOut. */

_Static_assert(sizeof(AudioOutBuffer) <= 0x40, "AudioOutBuffer fits a descriptor");

static void audio_initialize_parameter(void *parameter) { (void)parameter; }

static int32_t audio_open(void *object, const void *parameter) {
    (void)object, (void)parameter;
    const Result rc = audoutInitialize();
    if (R_FAILED(rc))
        fprintf(stderr, "[kaihen] audout: %x\n", rc);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

static int32_t audio_sample_rate(void *object) { (void)object; return (int32_t)audoutGetSampleRate(); }
static int32_t audio_channel_count(void *object) { (void)object; return (int32_t)audoutGetChannelCount(); }
static int32_t audio_sample_format(void *object) { (void)object; return (int32_t)audoutGetPcmFormat(); }

static void audio_set_buffer_info(void *descriptor, const void *pcm, uint64_t capacity, uint64_t size) {
    AudioOutBuffer *buffer = descriptor;
    memset(buffer, 0, sizeof *buffer);
    buffer->buffer = (void *)pcm;
    buffer->buffer_size = capacity;
    buffer->data_size = size;
}

static int32_t audio_append(void *object, void *descriptor) {
    (void)object;
    return R_SUCCEEDED(audoutAppendAudioOutBuffer(descriptor)) ? 1 : 0;
}

static int32_t audio_start(void *object) { (void)object; return R_SUCCEEDED(audoutStartAudioOut()) ? 0 : -1; }
static int32_t audio_stop(void *object) { (void)object; return R_SUCCEEDED(audoutStopAudioOut()) ? 0 : -1; }
static int32_t audio_nothing(void *object) { (void)object; return 0; }

static void *audio_released(void *object) {
    (void)object;
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0)
        return NULL;
    return released;
}

static int32_t audio_close(void *object) {
    (void)object;
    audoutExit();
    return 0;
}

__attribute__((constructor)) static void fill_audio_table(void) {
    g_ctx.audio = (struct hbx_audio){
        .initialize_parameter = (uintptr_t)audio_initialize_parameter,
        .open_default_audio_out = (uintptr_t)audio_open,
        .sample_rate = (uintptr_t)audio_sample_rate,
        .channel_count = (uintptr_t)audio_channel_count,
        .sample_format = (uintptr_t)audio_sample_format,
        .set_buffer_info = (uintptr_t)audio_set_buffer_info,
        .append_buffer = (uintptr_t)audio_append,
        .start = (uintptr_t)audio_start,
        .stop = (uintptr_t)audio_stop,
        .buffer_count = (uintptr_t)audio_nothing,
        .flush_buffers = (uintptr_t)audio_nothing,
        .released_buffer = (uintptr_t)audio_released,
        .close = (uintptr_t)audio_close,
    };
}

/* --- CPU clock ----------------------------------------------------------------------------- */

static const uint32_t kMaxHz = 1702400000u;
static uint32_t g_launch_hz;
static int g_clock_ready, g_raised;
static ClkrstSession g_cpu;

static int clock_open(void) {
    if (g_clock_ready)
        return g_clock_ready > 0;
    g_clock_ready = -1;
    if (R_FAILED(clkrstInitialize()))
        return 0;
    if (R_FAILED(clkrstOpenSession(&g_cpu, PcvModuleId_CpuBus, 3)) ||
        R_FAILED(clkrstGetClockRate(&g_cpu, &g_launch_hz)) || g_launch_hz >= kMaxHz) {
        clkrstExit();
        return 0;
    }
    g_clock_ready = 1;
    return 1;
}

int hbxl_cpu_clock_rates(uint32_t *launch_hz, uint32_t *max_hz) {
    if (!clock_open())
        return 0;
    *launch_hz = g_launch_hz;
    *max_hz = kMaxHz;
    return 1;
}

long hbxl_cpu_clock_boost(int on) {
    if (!clock_open())
        return -ENODEV;
    uint32_t want = on ? kMaxHz : g_launch_hz, now = 0;
    if (R_FAILED(clkrstSetClockRate(&g_cpu, want)) || R_FAILED(clkrstGetClockRate(&g_cpu, &now)))
        return -EIO;
    if (now != want)
        return -EAGAIN;
    g_raised = on;
    return 0;
}

void hbxl_cpu_clock_restore(void) {
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
