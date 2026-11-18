// The KaiKart launcher: the one app in hbmenu. It shows the games, sets up what the chosen one needs
// (the disc imported from an image on the card, the Retro Rewind release its build is made for),
// then starts its build in wiicompiled/apps/ through kaiHEN's loader (ounce-hbloader), which loads
// it once the launcher exits, the way hbmenu starts apps. Launcher, then game, is one game launch.
//
// Drawn in software into libnx's framebuffer (ui.cpp). The install steps are kaihen/shared
// (kaikart_install.cpp).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <switch.h>

#include "kaihen_https.h"
#include "kaikart_install.h"
#include "ui.h"

// The Retro Rewind release the Retro Rewind build is made for (kaihen/docker/build.sh writes it).
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
    {"Mario Kart Wii", "sdmc:/wiicompiled/apps/WiiCompiled.nro", "WiiCompiled", false},
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

// Charcoal and neutral greys, one restrained accent (a muted Switch-style cyan) for the selection
// and the progress bar.
constexpr ui::Color kWhite = ui::Rgb(236, 237, 239), kGrey = ui::Rgb(150, 152, 156), kAccent = ui::Rgb(92, 182, 214),
                    kCard = ui::Rgb(43, 44, 47), kCardSelected = ui::Rgb(52, 54, 58), kRed = ui::Rgb(232, 120, 112),
                    kGreen = ui::Rgb(150, 196, 160), kLine = ui::Rgb(62, 63, 67), kFooter = ui::Rgb(24, 25, 27),
                    kRowText = ui::Rgb(206, 207, 210), kStepText = ui::Rgb(214, 215, 218), kTrack = ui::Rgb(52, 54, 58);
constexpr int kMargin = 64;

ui::Image g_icons[kGameCount];
std::string g_status[kGameCount];  // under each game's name: ready, or what its first start sets up
bool g_ready[kGameCount];

void Header() {
    ui::Begin();
    ui::BoldText(kMargin, 40, 52, kWhite, "KaiKart");
    const std::string version = "v" KAIKART_VERSION;
    ui::Text(ui::kWidth - kMargin - ui::TextWidth(24, version), 58, 24, kGrey, version);
    ui::Rect(kMargin, 108, ui::kWidth - 2 * kMargin, 2, kLine);
}

void Footer(const std::vector<std::pair<const char*, std::string>>& hints) {
    ui::Rect(0, ui::kHeight - 64, ui::kWidth, 64, kFooter);
    ui::Rect(0, ui::kHeight - 64, ui::kWidth, 1, kLine);
    int x = kMargin;
    for (const auto& hint : hints) x += ui::ButtonHint(x, ui::kHeight - 47, hint.first, hint.second) + 36;
}

void DrawMenu(int selected, const std::string& message) {
    Header();
    constexpr int kCardHeight = 140, kCardTop = 132;
    const int width = ui::kWidth - 2 * kMargin;
    for (int i = 0; i < kGameCount; i++) {
        const int y = kCardTop + i * (kCardHeight + 16);
        const bool chosen = i == selected;
        ui::RoundRect(kMargin, y, width, kCardHeight, 18, chosen ? kCardSelected : kCard);
        if (chosen) ui::RoundFrame(kMargin, y, width, kCardHeight, 18, 3, kAccent);
        if (g_icons[i].width) {
            ui::DrawImage(g_icons[i], kMargin + 18, y + 14, 112, 112);
        } else {
            ui::RoundRect(kMargin + 18, y + 14, 112, 112, 12, kLine);
        }
        ui::BoldText(kMargin + 156, y + 30, 36, kWhite, kGames[i].title);
        ui::Text(kMargin + 156, y + 82, 24, g_ready[i] ? kGreen : kGrey, g_status[i]);
    }
    const char* rows[] = {"Check for updates", "Return to hbmenu"};
    for (int r = 0; r < 2; r++) {
        const int index = kGameCount + r, y = kCardTop + kGameCount * (kCardHeight + 16) + 8 + r * 64;
        const bool chosen = index == selected;
        ui::RoundRect(kMargin, y, width, 52, 14, chosen ? kCardSelected : kCard);
        if (chosen) ui::RoundFrame(kMargin, y, width, 52, 14, 3, kAccent);
        ui::Text(kMargin + 24, y + 12, 26, chosen ? kWhite : kRowText, rows[r]);
    }
    if (!message.empty()) {
        ui::WrapText(kMargin, ui::kHeight - 64 - 70, width, 24, kRed, message);
    }
    Footer({{"A", "Choose"}, {"+", "Exit"}});
    ui::End();
}

// A page with a title and a message, for results and questions.
void DrawPage(const std::string& title, const std::string& body, ui::Color color,
              const std::vector<std::pair<const char*, std::string>>& hints) {
    Header();
    ui::BoldText(kMargin, 170, 40, kWhite, title);
    ui::WrapText(kMargin, 240, ui::kWidth - 2 * kMargin, 28, color, body);
    Footer(hints);
    ui::End();
}

// The line at the bottom of a step's page.
constexpr const char* kSetupNote = "This happens once. Keep the console on and the card in.";
constexpr const char* kDownloadNote = "Keep the console on and the card in.";

// A problem: what happened in red, what to do in the normal text colour, B to go back.
void DrawError(const std::string& title, const std::string& detail, const std::string& advice) {
    Header();
    ui::BoldText(kMargin, 170, 40, kWhite, title);
    const int width = ui::kWidth - 2 * kMargin;
    const int used = ui::WrapText(kMargin, 240, width, 28, kRed, detail);
    ui::WrapText(kMargin, 240 + used + 18, width, 28, kStepText, advice);
    Footer({{"B", "Back"}});
    ui::End();
}

void DrawProgress(const char* title, kaikart::Progress& progress, const char* note) {
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
    if (failed) {
        DrawError(title, detail, advice);
        return;
    }
    Header();
    const int width = ui::kWidth - 2 * kMargin;
    ui::BoldText(kMargin, 170, 40, kWhite, title);
    ui::Text(kMargin, 244, 28, kStepText, step);
    if (!detail.empty()) ui::Text(kMargin, 284, 24, kGrey, detail);
    if (fraction >= 0) {
        constexpr int kBarTop = 350, kBarHeight = 30;
        ui::RoundRect(kMargin, kBarTop, width, kBarHeight, kBarHeight / 2, kTrack);
        const int filled = int(std::min(1.0f, fraction) * width);
        if (filled > kBarHeight) ui::RoundRect(kMargin, kBarTop, filled, kBarHeight, kBarHeight / 2, kAccent);
        const std::string percent = std::to_string(int(fraction * 100)) + "%";
        ui::Text(ui::kWidth - kMargin - ui::TextWidth(26, percent), kBarTop + 44, 26, kWhite, percent);
    }
    if (note && *note) ui::Text(kMargin, ui::kHeight - 64 - 56, 24, kGrey, note);
    Footer({});
    ui::End();
}

// Buttons pressed since the last call, as on a Pro Controller. A single Joy-Con held sideways
// arrives as if upright, so it's turned the way the games turn it (kaihen/patches/sdl3-kaihen.diff):
// its stick moves the selection, its right face button chooses (A), its bottom one goes back (B),
// and + or - exits.
u64 PressedButtons(PadState& pad) {
    padUpdate(&pad);
    const u32 style = padGetStyleSet(&pad);
    if (!(style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))) {
        return padGetButtonsDown(&pad);
    }
    static u64 previous = 0;
    const bool right = (style & HidNpadStyleTag_NpadJoyRight) != 0;
    const u64 held = padGetButtons(&pad);
    u64 now = 0;
    s32 x, up;
    if (right) {
        if (held & HidNpadButton_X) now |= HidNpadButton_A;
        if (held & HidNpadButton_A) now |= HidNpadButton_B;
        if (held & HidNpadButton_Plus) now |= HidNpadButton_Plus;
        const HidAnalogStickState stick = padGetStickPos(&pad, 1);
        x = stick.y;
        up = -stick.x;
    } else {
        if (held & HidNpadButton_Down) now |= HidNpadButton_A;
        if (held & HidNpadButton_Left) now |= HidNpadButton_B;
        if (held & HidNpadButton_Minus) now |= HidNpadButton_Plus;
        const HidAnalogStickState stick = padGetStickPos(&pad, 0);
        x = -stick.y;
        up = stick.x;
    }
    constexpr s32 kPush = 16000;  // about half way
    if (up > kPush) now |= HidNpadButton_Up;
    if (up < -kPush) now |= HidNpadButton_Down;
    if (x > kPush) now |= HidNpadButton_Right;
    if (x < -kPush) now |= HidNpadButton_Left;
    const u64 down = now & ~previous;
    previous = now;
    return down;
}

template <typename Work>
bool RunStep(const char* title, const char* note, PadState& pad, Work work) {
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
        PressedButtons(pad);
        const u64 now = armGetSystemTick();
        if (now - last > armNsToTicks(200'000'000)) {  // five times a second
            DrawProgress(title, progress, note);
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
            DrawProgress(title, progress, note);
            svcSleepThread(700'000'000);
        }
        return true;
    }
    DrawProgress(title, progress, note);
    while (appletMainLoop()) {
        if (PressedButtons(pad) & HidNpadButton_B) break;
        svcSleepThread(16'000'000);
    }
    return false;
}

bool Exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// --- updates ------------------------------------------------------------------------------------------

u64 WaitButtons(PadState& pad, u64 wanted) {
    while (appletMainLoop()) {
        const u64 down = PressedButtons(pad) & wanted;
        if (down) return down;
        svcSleepThread(16'000'000);
    }
    return 0;
}

// "Check for updates": a newer release on GitHub replaces this launcher and the games on the card,
// then the launcher starts again. True when it's restarting.
bool CheckForUpdates(PadState& pad) {
    kaikart::Release release;
    std::string error, retro_rewind;
    bool asked = false;
    RunStep("Updates", "", pad, [&](kaikart::Progress& p) {
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.step = "Checking for updates...";
        }
        asked = kaikart::NewerRelease(KAIHEN_UPDATE_REPO, "KaiKart", KAIKART_VERSION, release, error);
        if (asked && release.version.empty() && KAIHEN_RR_VERSION[0]) retro_rewind = kaikart::LatestRetroRewindVersion();
        return true;
    });
    if (!asked) {
        Log("[kaikart] update check: %s\n", error.c_str());
        DrawError("Updates", "Couldn't check for updates.",
                  "Please check that the console is connected to the internet, then try again.");
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    if (release.version.empty()) {
        std::string text = "KaiKart " KAIKART_VERSION " is up to date.";
        // Retro Rewind ahead of every KaiKart release: its new code needs a new KaiKart first.
        if (!retro_rewind.empty() && kaikart::VersionNewer(retro_rewind, KAIHEN_RR_VERSION)) {
            text += "\n\nRetro Rewind " + retro_rewind + " is out. It needs a KaiKart update, which isn't out yet; "
                    "until then Retro Rewind " KAIHEN_RR_VERSION " keeps working.";
        }
        DrawPage("Updates", text, kWhite, {{"B", "Back"}});
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    DrawPage("Updates", "KaiKart " + release.version + " is available. You have " KAIKART_VERSION ".", kWhite,
             {{"A", "Download and install"}, {"B", "Back"}});
    if (!(WaitButtons(pad, HidNpadButton_A | HidNpadButton_B) & HidNpadButton_A)) return false;
    Log("[kaikart] updating to %s\n", release.version.c_str());
    const std::string base = release.download_base, sums = base + "/SHA256SUMS";
    const std::string title = "Updating to KaiKart " + release.version;
    // The games on the card first, then the launcher itself (it runs from memory, so its file can go).
    for (const Game& game : kGames) {
        if (!Exists(game.path)) continue;
        const std::string url = base + "/" + game.release_name + "-" + release.version + ".nro";
        if (!RunStep(title.c_str(), kDownloadNote, pad,
                     [&](kaikart::Progress& p) { return kaikart::DownloadChecked(url, sums, game.path, p); })) {
            return false;
        }
    }
    if (!RunStep(title.c_str(), kDownloadNote, pad, [&](kaikart::Progress& p) {
            return kaikart::DownloadChecked(base + "/KaiKart-" + release.version + ".nro", sums, g_self, p);
        })) {
        return false;
    }
    if (R_FAILED(envSetNextLoad(g_self.c_str(), g_self.c_str()))) {
        DrawPage("Updates", "Updated to " + release.version + ". Close KaiKart and open it again to use it.", kWhite,
                 {{"B", "Back"}});
        WaitButtons(pad, HidNpadButton_B);
        return false;
    }
    Log("[kaikart] updated to %s; starting it\n", release.version.c_str());
    return true;
}

// Everything the game needs, then the hand-off. False (with `message`) when it can't start it.
bool Prepare(const Game& game, PadState& pad, std::string& message) {
    // The game's build matches this launcher's version (both come from one release); otherwise it's
    // downloaded from that release.
    const std::string build = kaikart::NroVersion(game.path);
    if (build != KAIKART_VERSION) {
        Log("[kaikart] %s build is '%s', getting %s\n", game.release_name, build.c_str(), KAIKART_VERSION);
        const std::string base = ReleaseBase(KAIKART_VERSION);
        const std::string file = std::string(game.release_name) + "-" KAIKART_VERSION ".nro";
        if (!RunStep((std::string("Getting ") + game.title).c_str(), kDownloadNote, pad, [&](kaikart::Progress& p) {
                return kaikart::DownloadChecked(base + "/" + file, base + "/SHA256SUMS", game.path, p);
            })) {
            message = build.empty() ? "That game isn't on the card yet." : "That game's build doesn't match KaiKart.";
            return false;
        }
    }
    if (!kaikart::DiscReady(kDisc)) {
        Log("[kaikart] importing the disc\n");
        if (!RunStep("Setting up your disc", kSetupNote, pad,
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
        if (!RunStep(title.c_str(), update ? kDownloadNote : kSetupNote, pad, [](kaikart::Progress& p) {
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

// What each game's card says: ready, or what its first start sets up.
void RefreshStatus() {
    const bool disc = kaikart::DiscReady(kDisc);
    const bool retro_rewind = kaikart::RetroRewindReady(kRetroRewind, KAIHEN_RR_CODE_SHA256);
    for (int i = 0; i < kGameCount; i++) {
        const Game& game = kGames[i];
        std::vector<std::string> needs;
        if (kaikart::NroVersion(game.path) != KAIKART_VERSION) needs.push_back("its game build");
        if (!disc) needs.push_back("your disc");
        if (game.retro_rewind && !retro_rewind) needs.push_back(Exists(kRetroRewind) ? "a Retro Rewind update" : "Retro Rewind");
        g_ready[i] = needs.empty();
        if (g_ready[i]) {
            g_status[i] = game.retro_rewind && KAIHEN_RR_VERSION[0] ? "Ready to play · Retro Rewind " KAIHEN_RR_VERSION
                                                                   : "Ready to play";
        } else {
            std::string list;
            for (size_t n = 0; n < needs.size(); n++) {
                list += n == 0 ? "" : n + 1 == needs.size() ? " and " : ", ";
                list += needs[n];
            }
            g_status[i] = "First start sets up " + list;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argc > 0 && argv[0] ? argv[0] : "sdmc:/switch/KaiKart.nro";
    const bool romfs = R_SUCCEEDED(romfsInit());
    if (romfs) CopyMissing("romfs:/wiicompiled", kRoot);
    mkdir("sdmc:/wiicompiled/UserData", 0777);
    mkdir("sdmc:/wiicompiled/UserData/Logs", 0777);
    // The previous run's log stays as kaikart.prev.log (only the last one), so a setup or update
    // problem survives the next start. The card's file system won't rename over a file.
    std::remove("sdmc:/wiicompiled/UserData/Logs/kaikart.prev.log");
    std::rename("sdmc:/wiicompiled/UserData/Logs/kaikart.log", "sdmc:/wiicompiled/UserData/Logs/kaikart.prev.log");
    g_log = std::fopen("sdmc:/wiicompiled/UserData/Logs/kaikart.log", "w");
    Log("[kaikart] %s started\n", KAIKART_VERSION);
    kaikart::SetLog(LogLine);
    kaihen_https::SetCaBundle("sdmc:/wiicompiled/cacert.pem");
    // libnx's socket defaults, with TCP receive buffers that may grow to 1 MiB (the Retro Rewind
    // download is about 1.9 GB).
    SocketInitConfig sockets = *socketGetDefaultInitConfig();
    sockets.tcp_rx_buf_max_size = 0x100000;
    socketInitialize(&sockets);

    if (!ui::Init()) Log("[kaikart] the screen or the system font didn't start\n");
    for (int i = 0; i < kGameCount; i++) {
        const std::string icon = std::string("romfs:/icons/") + kGames[i].release_name + ".jpg";
        if (romfs) ui::LoadJpeg(icon.c_str(), g_icons[i]);
    }
    if (romfs) romfsExit();
    RefreshStatus();
    // As the games: up to four players (so the console keeps players 2-4 connected while the
    // launcher is open), single Joy-Cons held sideways. Player 1 controls the menu.
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    PadState pad;
    padInitializeDefault(&pad);

    int selected = 0;
    std::string message = envHasNextLoad() ? "" : "kaiHEN's loader can't start another app from here.";
    bool starting = false, restarting = false;
    DrawMenu(selected, message);
    while (!starting && appletMainLoop()) {
        const u64 down = PressedButtons(pad);
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
                if (!starting) RefreshStatus();
            }
        }
        if (!starting) DrawMenu(selected, message);
    }
    if (starting || restarting) {
        Header();
        ui::BoldText(kMargin, 170, 40, kWhite,
                 restarting ? std::string("Starting the new KaiKart...") : std::string("Starting ") + kGames[selected].title + "...");
        Footer({});
        ui::End();
    }
    ui::Exit();
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
