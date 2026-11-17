// The KaiKart launcher: the one app in hbmenu. It shows the games, sets up what the chosen one needs
// (the disc imported from an image on the card, the Retro Rewind release its build is made for),
// then starts its build in wiicompiled/apps/ through kaiHEN's loader (ounce-hbloader), which loads
// it once the launcher exits, the way hbmenu starts apps. Launcher, then game, is one game launch.
//
// Drawn with libnx's text console. The install steps are kaihen/shared (kaikart_install.cpp).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>

#include <switch.h>

#include "kaihen_https.h"
#include "kaikart_install.h"

// The Retro Rewind release the Retro Rewind build is made for (kaihen/build.sh writes it).
#if __has_include("kaihen_build_info.h")
#include "kaihen_build_info.h"
#endif
#ifndef KAIHEN_RR_VERSION
#define KAIHEN_RR_VERSION ""
#define KAIHEN_RR_CODE_SHA256 ""
#endif
#ifndef KAIKART_VERSION
#define KAIKART_VERSION "dev"
#endif
#ifndef KAIHEN_UPDATE_REPO
#define KAIHEN_UPDATE_REPO ""
#endif

namespace {

constexpr const char* kRoot = "sdmc:/wiicompiled";
constexpr const char* kDisc = "sdmc:/wiicompiled/disc/DATA";
constexpr const char* kRetroRewind = "sdmc:/wiicompiled/RetroRewind/RetroRewind6";

struct Game {
    const char* title;
    const char* path;
    const char* release_name;  // <release_name>-<version>.nro in a release
    bool retro_rewind;
};
const Game kGames[] = {
    {"Retro Rewind", "sdmc:/wiicompiled/apps/RetroRewind.nro", "RetroRewind", true},
    {"Original Mario Kart Wii", "sdmc:/wiicompiled/apps/WiiCompiled.nro", "WiiCompiled", false},
};
constexpr int kGameCount = sizeof kGames / sizeof kGames[0];
constexpr int kUpdatesRow = kGameCount;
constexpr int kRows = kGameCount + 2;  // and "Check for updates", "Return to hbmenu"

std::string g_self;  // this launcher's .nro (hbmenu passes its path first)

std::string ReleaseBase(const std::string& version) {
    return std::string("https://github.com/") + KAIHEN_UPDATE_REPO + "/releases/download/v" + version;
}

// --- the log: wiicompiled/UserData/Logs/kaikart.log -------------------------------------------------

FILE* g_log;

void LogLine(const char* line) {
    if (!g_log) return;
    std::fputs(line, g_log);
    std::fflush(g_log);
}

void Log(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    LogLine(line);
}

// --- the card's own small files (romfs:/wiicompiled), as the games copy them ------------------------

void CopyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return;
    const std::string partial = to + ".partial";
    if (FILE* out = std::fopen(partial.c_str(), "wb")) {
        char buffer[16384];
        size_t n;
        bool failed = false;
        while ((n = std::fread(buffer, 1, sizeof buffer, in)) > 0) failed |= std::fwrite(buffer, 1, n, out) != n;
        failed |= std::fclose(out) != 0;
        if (failed || (std::remove(to.c_str()), std::rename(partial.c_str(), to.c_str())) != 0) std::remove(partial.c_str());
    }
    std::fclose(in);
}

bool FollowsApp(const char* name) { return !std::strcmp(name, "dsp_coef.bin") || !std::strcmp(name, "cacert.pem"); }

void CopyMissing(const std::string& from, const std::string& to) {
    DIR* dir = opendir(from.c_str());
    if (!dir) return;
    mkdir(to.c_str(), 0777);
    while (dirent* entry = readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        const std::string source = from + "/" + entry->d_name, target = to + "/" + entry->d_name;
        struct stat in, out;
        if (stat(source.c_str(), &in) != 0) continue;
        if (S_ISDIR(in.st_mode)) {
            CopyMissing(source, target);
            continue;
        }
        const bool present = stat(target.c_str(), &out) == 0;
        if (present && !(FollowsApp(entry->d_name) && out.st_size != in.st_size)) continue;
        if (!std::strcmp(entry->d_name, "settings.env") && stat("sdmc:/wiicompiled/hbxl-env", &out) == 0) continue;
        CopyFile(source, target);
    }
    closedir(dir);
}

// --- the screen ---------------------------------------------------------------------------------------

void Clear() { std::printf("\x1b[2J\x1b[1;1H"); }

void Header() {
    Clear();
    std::printf("\n  KaiKart %s\n\n", KAIKART_VERSION);
}

void DrawMenu(int selected, const std::string& message) {
    Header();
    for (int i = 0; i < kRows; i++) {
        if (i < kGameCount) {
            std::printf("  %s %s", i == selected ? ">" : " ", kGames[i].title);
            if (kGames[i].retro_rewind && KAIHEN_RR_VERSION[0]) std::printf("  (%s)", KAIHEN_RR_VERSION);
            std::printf("\n");
        } else {
            if (i == kUpdatesRow) std::printf("\n");
            std::printf("  %s %s\n", i == selected ? ">" : " ", i == kUpdatesRow ? "Check for updates" : "Return to hbmenu");
        }
    }
    std::printf("\n  %s\n", message.c_str());
    std::printf("\n\n  Up/Down: move   A: choose\n");
    consoleUpdate(nullptr);
}

void DrawProgress(const char* title, kaikart::Progress& progress) {
    std::string step, detail, advice;
    float fraction;
    bool failed;
    {
        std::lock_guard<std::mutex> lock(progress.mutex);
        step = progress.step;
        detail = progress.detail;
        advice = progress.advice;
        fraction = progress.fraction;
        failed = progress.failed;
    }
    Header();
    std::printf("  %s\n\n", title);
    if (failed) {
        std::printf("  Stopped: %s\n\n  %s\n\n\n  B: back\n", detail.c_str(), advice.c_str());
    } else {
        std::printf("  %s\n", step.c_str());
        if (!detail.empty()) std::printf("  %s\n", detail.c_str());
        std::printf("\n");
        if (fraction >= 0) {
            constexpr int kWidth = 50;
            const int filled = int(fraction * kWidth + 0.5f);
            std::printf("  [");
            for (int i = 0; i < kWidth; i++) std::putchar(i < filled ? '#' : '-');
            std::printf("] %d%%\n", int(fraction * 100));
        }
        std::printf("\n\n  This happens once. Keep the console on and the card in.\n");
    }
    consoleUpdate(nullptr);
}

// Runs one install step on a worker thread while the screen shows its progress. True when it
// finished; false when it failed (after the player pressed B) or the app is closing.
template <typename Work>
bool RunStep(const char* title, PadState& pad, Work work) {
    struct Job {
        Work* work;
        kaikart::Progress progress;
        std::atomic<bool> done{false};
        bool ok = false;
    } job{&work};
    // Its own thread with room for the extractor and the downloader (1 MiB of stack).
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1 << 20);
    pthread_t worker;
    pthread_create(&worker, &attributes, [](void* argument) -> void* {
        auto* job = static_cast<Job*>(argument);
        job->ok = (*job->work)(job->progress);
        job->done = true;
        return nullptr;
    }, &job);
    pthread_attr_destroy(&attributes);
    kaikart::Progress& progress = job.progress;
    std::atomic<bool>& done = job.done;
    u64 last = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 now = armGetSystemTick();
        if (now - last > armNsToTicks(200'000'000)) {  // five times a second
            DrawProgress(title, progress);
            last = now;
        }
        if (done) break;
        svcSleepThread(16'000'000);
    }
    pthread_join(worker, nullptr);
    if (job.ok) {
        // A step with a bar shows it full for a moment, before the next step or the game.
        bool bar;
        {
            std::lock_guard<std::mutex> lock(progress.mutex);
            bar = progress.fraction >= 0;
            if (bar) progress.fraction = 1;
        }
        if (bar) {
            DrawProgress(title, progress);
            svcSleepThread(700'000'000);
        }
        return true;
    }
    DrawProgress(title, progress);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
        svcSleepThread(16'000'000);
    }
    return false;
}

// One game per kaiHEN session: the games note their process and the console's tick in
// UserData/session.txt (kaihen/platform/kaihen_platform.c); hbmenu, this launcher and the games
// share a process, so a match means a game already ran since hbmenu opened.
bool GameRanThisSession() {
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    unsigned long long saved_pid = 0, saved_tick = 0;
    FILE* file = std::fopen("sdmc:/wiicompiled/UserData/session.txt", "r");
    if (!file) return false;
    const bool same = std::fscanf(file, "%llu %llu", &saved_pid, &saved_tick) == 2 && saved_pid == pid &&
                      saved_tick <= armGetSystemTick();
    std::fclose(file);
    return same;
}

bool Exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// --- updates ------------------------------------------------------------------------------------------

void DrawLines(const char* title, const std::string& body, const char* keys) {
    Header();
    std::printf("  %s\n\n  %s\n\n\n  %s\n", title, body.c_str(), keys);
    consoleUpdate(nullptr);
}

u64 WaitButtons(PadState& pad, u64 wanted) {
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad) & wanted;
        if (down) return down;
        svcSleepThread(16'000'000);
    }
    return 0;
}

// "Check for updates": a newer release on GitHub replaces this launcher and the games on the card,
// then the launcher starts again. True when it's restarting.
bool CheckForUpdates(PadState& pad) {
    kaikart::Release release;
    std::string error;
    bool asked = false;
    RunStep("Updates", pad, [&](kaikart::Progress& p) {
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.step = "Checking for updates...";
        }
        asked = kaikart::NewerRelease(KAIHEN_UPDATE_REPO, "KaiKart", KAIKART_VERSION, release, error);
        return true;
    });
    if (!asked) {
        DrawLines("Updates", "Couldn't check for updates: " + error + ".\n  Check the console's internet connection.",
                  "B: back");
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    if (release.version.empty()) {
        DrawLines("Updates", "KaiKart " KAIKART_VERSION " is up to date.", "B: back");
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    DrawLines("Updates", "KaiKart " + release.version + " is available. You have " KAIKART_VERSION ".",
              "A: download and install   B: back");
    if (!(WaitButtons(pad, HidNpadButton_A | HidNpadButton_B) & HidNpadButton_A)) return false;
    Log("[kaikart] updating to %s\n", release.version.c_str());
    const std::string base = release.download_base, sums = base + "/SHA256SUMS";
    const std::string title = "Updating to KaiKart " + release.version;
    // The games on the card first, then the launcher itself (it runs from memory, so its file can go).
    for (const Game& game : kGames) {
        if (!Exists(game.path)) continue;
        const std::string url = base + "/" + game.release_name + "-" + release.version + ".nro";
        if (!RunStep(title.c_str(), pad,
                     [&](kaikart::Progress& p) { return kaikart::DownloadChecked(url, sums, game.path, p); })) {
            return false;
        }
    }
    if (!RunStep(title.c_str(), pad, [&](kaikart::Progress& p) {
            return kaikart::DownloadChecked(base + "/KaiKart-" + release.version + ".nro", sums, g_self, p);
        })) {
        return false;
    }
    if (R_FAILED(envSetNextLoad(g_self.c_str(), g_self.c_str()))) {
        DrawLines("Updates", "Updated to " + release.version + ". Close KaiKart and open it again to use it.", "B: back");
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    Log("[kaikart] updated to %s; starting it\n", release.version.c_str());
    return true;
}

// Everything the game needs, then the hand-off. False (with `message`) when it can't start it.
bool Prepare(const Game& game, PadState& pad, std::string& message) {
    if (GameRanThisSession()) {
        message = "A game already ran in this kaiHEN session, and starting another one crashes.\n"
                  "  Close KaiKart with HOME, then hold R while starting a game to open hbmenu again.";
        return false;
    }
    // The game's build matches this launcher's version (both come from one release); otherwise it's
    // downloaded from that release.
    const std::string build = kaikart::NroVersion(game.path);
    if (build != KAIKART_VERSION) {
        Log("[kaikart] %s build is '%s', getting %s\n", game.release_name, build.c_str(), KAIKART_VERSION);
        const std::string base = ReleaseBase(KAIKART_VERSION);
        const std::string file = std::string(game.release_name) + "-" KAIKART_VERSION ".nro";
        if (!RunStep((std::string("Getting ") + game.title).c_str(), pad, [&](kaikart::Progress& p) {
                return kaikart::DownloadChecked(base + "/" + file, base + "/SHA256SUMS", game.path, p);
            })) {
            message = build.empty() ? "That game isn't on the card yet." : "That game's build doesn't match KaiKart.";
            return false;
        }
    }
    if (!kaikart::DiscReady(kDisc)) {
        Log("[kaikart] importing the disc\n");
        if (!RunStep("Setting up your disc", pad,
                     [](kaikart::Progress& p) { return kaikart::ImportDisc(kRoot, kDisc, p); })) {
            message = "The disc isn't set up yet.";
            return false;
        }
    }
    if (game.retro_rewind && !kaikart::RetroRewindReady(kRetroRewind, KAIHEN_RR_CODE_SHA256)) {
        const bool update = Exists(kRetroRewind);
        Log("[kaikart] %s Retro Rewind %s\n", update ? "updating to" : "installing", KAIHEN_RR_VERSION);
        const std::string title = std::string(update ? "Updating Retro Rewind to " : "Installing Retro Rewind ") +
                                  KAIHEN_RR_VERSION;
        if (!RunStep(title.c_str(), pad, [](kaikart::Progress& p) {
                return kaikart::InstallRetroRewind(kRetroRewind, KAIHEN_RR_VERSION, KAIHEN_RR_CODE_SHA256, p);
            })) {
            message = "Retro Rewind isn't set up yet.";
            return false;
        }
    }
    if (R_FAILED(envSetNextLoad(game.path, game.path))) {
        message = "kaiHEN's loader didn't accept the game.";
        return false;
    }
    Log("[kaikart] starting %s\n", game.path);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argc > 0 && argv[0] ? argv[0] : "sdmc:/switch/KaiKart.nro";
    if (R_SUCCEEDED(romfsInit())) {
        CopyMissing("romfs:/wiicompiled", kRoot);
        romfsExit();
    }
    mkdir("sdmc:/wiicompiled/UserData", 0777);
    mkdir("sdmc:/wiicompiled/UserData/Logs", 0777);
    g_log = std::fopen("sdmc:/wiicompiled/UserData/Logs/kaikart.log", "w");
    Log("[kaikart] %s started\n", KAIKART_VERSION);
    kaikart::SetLog(LogLine);
    kaihen_https::SetCaBundle("sdmc:/wiicompiled/cacert.pem");
    // libnx's socket defaults, with TCP receive buffers that may grow to 1 MiB (the Retro Rewind
    // download is about 1.9 GB).
    SocketInitConfig sockets = *socketGetDefaultInitConfig();
    sockets.tcp_rx_buf_max_size = 0x100000;
    socketInitialize(&sockets);

    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    int selected = 0;
    std::string message = envHasNextLoad() ? "" : "kaiHEN's loader can't start another app from here.";
    bool starting = false, restarting = false;
    DrawMenu(selected, message);
    while (!starting && appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down == 0) {
            svcSleepThread(16'000'000);
            continue;
        }
        if (down & HidNpadButton_AnyUp) selected = (selected + kRows - 1) % kRows;
        if (down & HidNpadButton_AnyDown) selected = (selected + 1) % kRows;
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_A) {
            if (selected == kRows - 1) break;
            message.clear();
            if (selected == kUpdatesRow) {
                if (CheckForUpdates(pad)) {
                    restarting = true;
                    break;
                }
            } else {
                starting = Prepare(kGames[selected], pad, message);
            }
        }
        if (!starting) DrawMenu(selected, message);
    }
    if (starting || restarting) {
        Header();
        std::printf("  %s...\n", restarting ? "Starting the new KaiKart" : (std::string("Starting ") + kGames[selected].title).c_str());
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    socketExit();
    Log("[kaikart] exit\n");
    if (g_log) std::fclose(g_log);
    return 0;
}

// --- what mbed TLS needs on kaiHEN (kaihen/platform/kaihen_mbedtls_config.h) ---------------------------
extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    randomGet(output, len);
    *olen = len;
    return 0;
}

extern "C" int64_t mbedtls_ms_time(void) { return (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000); }
