// First-launch setup for the kaiHEN apps (kaihen_setup.h). The card's own small files are copied
// from the app before this runs (kaihen_platform.c, card_setup); this part does the slow work with
// a progress screen drawn through aurora's ImGui, before the game's first frame.
#include "kaihen_setup.h"

#include "kaihen_https.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"
#include "wii_disc_extractor.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <cryptopp/filters.h>
#include <cryptopp/files.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha.h>
#include <imgui.h>
#include <mbedtls/sha256.h>
#include <zlib.h>

// The app's version, where its updates come from, and the Retro Rewind release its translation
// was made from (kaihen/build.sh writes it).
#if __has_include("kaihen_build_info.h")
#include "kaihen_build_info.h"
#endif
#ifndef KAIHEN_APP_VERSION
#define KAIHEN_APP_VERSION "0.0.0"
#define KAIHEN_UPDATE_REPO ""
#endif
#ifndef KAIHEN_RR_VERSION
#define KAIHEN_RR_VERSION ""
#define KAIHEN_RR_CODE_SHA256 ""
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" int kaihen_return_requested(void);
extern "C" uint32_t kaihen_setup_buttons(void);  // kaihen_platform.c
extern "C" const char* kaihen_app_path(void);
namespace settings_overlay {
[[noreturn]] void ConsoleReturnToLauncher() noexcept;  // settings_overlay.cpp
}

namespace kaihen_setup {
namespace {

namespace fs = std::filesystem;

// A clean PAL Mario Kart Wii, revision 0 (the build's translation is made from it).
constexpr const char* kMainDolSha256 = "80D18895B39C63BD80F457398BFCBB91B7D16AC116A41A88967E954080155B05";
constexpr const char* kStaticRSha256 = "16D9D146112541FEFEA701ECB5BC1A496F9D50E4A752FBB5B6778E7C6399F67D";
constexpr uint64_t kDiscBytes = 3ull << 30;  // room the extracted disc needs, with some to spare

// What the screen shows; the worker thread writes it, the screen reads it.
struct Status {
    std::mutex mutex;
    std::string step;     // "Importing your disc"
    std::string detail;   // a file name, or what went wrong
    std::string advice;   // on a problem: what the player should do
    float progress = -1;  // 0..1, or negative for none
    bool failed = false;
    std::atomic<bool> finished{false};
};
Status g_status;

void SetStep(const std::string& step, const std::string& detail = {}) {
    std::lock_guard<std::mutex> lock(g_status.mutex);
    g_status.step = step;
    g_status.detail = detail;
    g_status.progress = -1;
    RT_LOGF("setup", "%s %s\n", step.c_str(), detail.c_str());
}

void Fail(const std::string& detail, const std::string& advice) {
    std::lock_guard<std::mutex> lock(g_status.mutex);
    g_status.failed = true;
    g_status.detail = detail;
    g_status.advice = advice;
    RT_LOGF("setup", "failed: %s\n", detail.c_str());
}

std::string Sha256(const fs::path& path) {
    std::string digest;
    try {
        CryptoPP::SHA256 hash;
        CryptoPP::FileSource(path.c_str(), true,
                             new CryptoPP::HashFilter(hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(digest))));
    } catch (const CryptoPP::Exception&) {
        return {};
    }
    return digest;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

using Clock = std::chrono::steady_clock;
double Seconds(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }

uint64_t FreeBytes(const fs::path& path) {
    struct statvfs info {};
    if (statvfs(path.c_str(), &info) != 0) {
        return UINT64_MAX;  // unknown: let the copy itself report a full card
    }
    return uint64_t(info.f_bavail) * info.f_frsize;
}

// --- The disc ------------------------------------------------------------------------------------

bool DiscReady(const fs::path& data) {
    std::error_code error;
    return fs::is_regular_file(data / "sys" / "fst.bin", error) &&
           fs::is_regular_file(data / "sys" / "main.dol", error);
}

// The disc image the player put in the wiicompiled folder: an .iso, or a .wbfs with the .wbf1,
// .wbf2 ... parts a FAT32 card splits it into.
std::vector<fs::path> FindDiscImage(const fs::path& folder) {
    std::error_code error;
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        const std::string extension = Lower(entry.path().extension().string());
        // "._name" files are a Mac's sidecars, not images.
        if (entry.is_regular_file(error) && entry.path().filename().string().rfind("._", 0) != 0 &&
            (extension == ".iso" || extension == ".wbfs")) {
            found.push_back(entry.path());
        }
    }
    if (found.empty()) {
        return {};
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> parts{found.front()};
    if (Lower(parts[0].extension().string()) == ".wbfs") {
        for (int i = 1; i < 10; ++i) {
            fs::path part = parts[0];
            part.replace_extension(".wbf" + std::to_string(i));
            if (!fs::is_regular_file(part, error)) {
                break;
            }
            parts.push_back(part);
        }
    }
    return parts;
}

bool ImportDisc(const fs::path& root, const fs::path& data) {
    const std::vector<fs::path> image = FindDiscImage(root);
    if (image.empty()) {
        Fail("No Mario Kart Wii disc image in the wiicompiled folder.",
             "Put your own PAL Mario Kart Wii disc (RMCP01) on the microSD card as an .iso or .wbfs file, in "
             "the wiicompiled folder, then open the app again. (.rvz isn't supported yet.)");
        return false;
    }
    if (FreeBytes(root) < kDiscBytes) {
        Fail("The microSD card is too full.", "Importing the disc needs about 3 GB free. Make room, then open "
             "the app again.");
        return false;
    }
    SetStep("Importing your disc", image[0].filename().string());
    const auto started = Clock::now();

    const fs::path partial = data.parent_path() / (data.filename().string() + ".partial");
    std::error_code error;
    fs::remove_all(partial, error);
    fs::create_directories(partial, error);

    std::vector<int> fds;
    for (const auto& part : image) {
        const int fd = open(part.c_str(), O_RDONLY);
        if (fd < 0) {
            for (const int open_fd : fds) close(open_fd);
            Fail("Couldn't open " + part.filename().string() + ".", "Check the file on the card, then try again.");
            return false;
        }
        fds.push_back(fd);
    }
    std::atomic<bool> extracting{true};
    std::thread progress([&extracting] {
        while (extracting.load()) {
            const uint64_t total = WiiDiscExtractor_BytesTotal();
            if (total != 0) {
                std::lock_guard<std::mutex> lock(g_status.mutex);
                g_status.progress = float(double(WiiDiscExtractor_BytesDone()) / double(total));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    char message[512] = {};
    const int result = WiiDiscExtractor_ExtractParts(fds.data(), int(fds.size()), partial.c_str(), message,
                                                     int(sizeof message));
    extracting = false;
    progress.join();
    for (const int fd : fds) close(fd);
    {
        const double seconds = Seconds(started);
        const double megabytes = double(WiiDiscExtractor_BytesDone()) / 1e6;
        RT_LOGF("setup", "disc extracted: %.0f MB in %.0f s (%.1f MB/s)\n", megabytes, seconds,
                megabytes / std::max(seconds, 0.001));
    }
    if (result != 0) {
        fs::remove_all(partial, error);
        Fail(message, "Use your own clean, unmodified PAL Mario Kart Wii disc (RMCP01).");
        return false;
    }

    SetStep("Checking your disc");
    if (Sha256(partial / "sys" / "main.dol") != kMainDolSha256 ||
        Sha256(partial / "files" / "rel" / "StaticR.rel") != kStaticRSha256) {
        fs::remove_all(partial, error);
        Fail("This disc isn't a clean PAL Mario Kart Wii, revision 0.",
             "Only an unmodified PAL disc (RMCP01, revision 0) works. Use your own dump of it.");
        return false;
    }
    fs::remove_all(data, error);  // anything left of an older, unfinished copy
    fs::rename(partial, data, error);
    if (error) {
        Fail("Couldn't finish the import: " + error.message() + ".", "Open the app again to retry.");
        return false;
    }
    RT_LOGF("setup", "disc imported from %s\n", image[0].c_str());
    return true;
}

// --- Retro Rewind --------------------------------------------------------------------------------
// The release the app was built for, from Retro Rewind's own server: the full zip of each release
// stays there. Only its RetroRewind6 folder is needed, one run of entries in the zip, so that run
// is streamed and unpacked as it arrives: the zip itself never lands on the card.

constexpr uint64_t kRetroRewindBytes = 5ull << 29;  // 2.2 GB unpacked, with some to spare
constexpr const char* kRetroRewindPrefix = "RetroRewind6/";

std::string RetroRewindUrl() {
    return std::string("https://cdn.update.rwfc.net/RetroRewind/zip/") + KAIHEN_RR_VERSION + "-full.zip";
}

bool RetroRewindReady(const fs::path& folder) {
    if (KAIHEN_RR_CODE_SHA256[0] == '\0') {
        std::error_code error;
        return fs::is_regular_file(folder / "Binaries" / "Code.pul", error);  // a build without a pin
    }
    return Sha256(folder / "Binaries" / "Code.pul") == KAIHEN_RR_CODE_SHA256;
}

struct ZipEntry {
    std::string name;  // under RetroRewind6/, without that prefix
    uint64_t offset;   // of its local header
    uint32_t packed, size, crc;
    uint16_t method;
};

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

bool Fetch(const std::string& url, uint64_t from, uint64_t to, std::vector<uint8_t>& out, uint64_t* total,
           std::string& error) {
    kaihen_https::Get get;
    if (!get.Open(url, from, to, error)) return false;
    out.clear();
    uint8_t buffer[65536];
    int64_t n;
    while ((n = get.Read(buffer, sizeof buffer, error)) > 0) out.insert(out.end(), buffer, buffer + n);
    if (total) *total = get.Total();
    return n == 0;
}

// The zip's directory: where each RetroRewind6 entry is, and where their run ends.
bool ReadZipDirectory(const std::string& url, std::vector<ZipEntry>& entries, uint64_t& run_end, std::string& error) {
    std::vector<uint8_t> probe;
    uint64_t total = 0;
    if (!Fetch(url, 0, 0, probe, &total, error)) return false;
    if (total == UINT64_MAX || total < 22) {
        error = "the server didn't give the file's size";
        return false;
    }
    const uint64_t tail_start = total > 65558 ? total - 65558 : 0;
    std::vector<uint8_t> tail;
    if (!Fetch(url, tail_start, total - 1, tail, nullptr, error)) return false;
    size_t eocd = tail.size() >= 22 ? tail.size() - 22 : 0;
    while (eocd > 0 && Le32(&tail[eocd]) != 0x06054b50) --eocd;
    if (Le32(&tail[eocd]) != 0x06054b50) {
        error = "the download isn't a zip file";
        return false;
    }
    const uint32_t directory_size = Le32(&tail[eocd + 12]), directory_offset = Le32(&tail[eocd + 16]);
    std::vector<uint8_t> directory;
    if (!Fetch(url, directory_offset, uint64_t(directory_offset) + directory_size - 1, directory, nullptr, error)) {
        return false;
    }
    std::vector<uint64_t> all_offsets;
    for (size_t p = 0; p + 46 <= directory.size() && Le32(&directory[p]) == 0x02014b50;) {
        const uint16_t name_length = Le16(&directory[p + 28]), extra = Le16(&directory[p + 30]),
                       comment = Le16(&directory[p + 32]);
        const std::string name(reinterpret_cast<const char*>(&directory[p + 46]), name_length);
        const uint64_t offset = Le32(&directory[p + 42]);
        all_offsets.push_back(offset);
        if (name.compare(0, std::strlen(kRetroRewindPrefix), kRetroRewindPrefix) == 0 &&
            name.size() > std::strlen(kRetroRewindPrefix)) {
            entries.push_back({name.substr(std::strlen(kRetroRewindPrefix)), offset, Le32(&directory[p + 20]),
                               Le32(&directory[p + 24]), Le32(&directory[p + 16]), Le16(&directory[p + 10])});
        }
        p += 46 + name_length + extra + comment;
    }
    if (entries.empty()) {
        error = "the zip has no RetroRewind6 folder";
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const ZipEntry& a, const ZipEntry& b) { return a.offset < b.offset; });
    std::sort(all_offsets.begin(), all_offsets.end());
    const auto next = std::upper_bound(all_offsets.begin(), all_offsets.end(), entries.back().offset);
    run_end = next == all_offsets.end() ? directory_offset : *next;
    return true;
}

// Reads the streamed run exactly.
// Where the download's time goes (logged when it ends).
double g_network_seconds = 0, g_write_seconds = 0;

class RunReader {
public:
    kaihen_https::Get get;
    uint64_t consumed = 0;
    bool Exact(void* out, size_t size, std::string& error) {
        auto* bytes = static_cast<uint8_t*>(out);
        while (size > 0) {
            const auto started = Clock::now();
            const int64_t n = get.Read(bytes, size, error);
            g_network_seconds += Seconds(started);
            if (n <= 0) {
                if (n == 0) error = "the download ended early";
                return false;
            }
            bytes += n;
            size -= size_t(n);
            consumed += uint64_t(n);
        }
        return true;
    }
};

// One entry from the stream into `folder`: its local header, then its data, stored or deflated.
bool UnpackEntry(RunReader& reader, const ZipEntry& entry, const fs::path& folder, std::string& error) {
    uint8_t header[30];
    if (!reader.Exact(header, sizeof header, error)) return false;
    if (Le32(header) != 0x04034b50) {
        error = "the zip's data didn't match its directory";
        return false;
    }
    std::vector<uint8_t> skip(size_t(Le16(header + 26)) + Le16(header + 28));
    if (!skip.empty() && !reader.Exact(skip.data(), skip.size(), error)) return false;

    const fs::path target = folder / fs::path(entry.name);
    std::error_code fs_error;
    if (entry.name.back() == '/') {
        fs::create_directories(target, fs_error);
        return true;
    }
    fs::create_directories(target.parent_path(), fs_error);
    // Written under its own name: a file cut short by an interruption has the wrong size, which
    // the resume check catches, and the card is spared a rename and a delete per file.
    FILE* out = std::fopen(target.c_str(), "wb");
    if (!out) {
        error = "couldn't write " + target.filename().string() + " to the card";
        return false;
    }
    static std::vector<char> file_buffer(1 << 18);
    std::setvbuf(out, file_buffer.data(), _IOFBF, file_buffer.size());
    std::vector<uint8_t> in(1 << 16), unpacked(1 << 18);
    z_stream zip{};
    bool ok = entry.method == 0 || (entry.method == 8 && inflateInit2(&zip, -MAX_WBITS) == Z_OK);
    if (!ok) error = "the zip uses a compression this app can't read";
    uLong crc = crc32(0, nullptr, 0);
    uint64_t left = entry.packed, written = 0;
    while (ok && left > 0) {
        const size_t chunk = size_t(std::min<uint64_t>(left, in.size()));
        if (!reader.Exact(in.data(), chunk, error)) { ok = false; break; }
        left -= chunk;
        if (entry.method == 0) {
            crc = crc32(crc, in.data(), uInt(chunk));
            const auto started = Clock::now();
            ok = std::fwrite(in.data(), 1, chunk, out) == chunk;
            g_write_seconds += Seconds(started);
            written += chunk;
            continue;
        }
        zip.next_in = in.data();
        zip.avail_in = uInt(chunk);
        do {
            zip.next_out = unpacked.data();
            zip.avail_out = uInt(unpacked.size());
            const int code = inflate(&zip, Z_NO_FLUSH);
            if (code != Z_OK && code != Z_STREAM_END) {
                error = "a file in the zip is damaged";
                ok = false;
                break;
            }
            const size_t produced = unpacked.size() - zip.avail_out;
            crc = crc32(crc, unpacked.data(), uInt(produced));
            const auto started = Clock::now();
            ok = std::fwrite(unpacked.data(), 1, produced, out) == produced;
            g_write_seconds += Seconds(started);
            written += produced;
        } while (ok && zip.avail_out == 0);
    }
    if (entry.method == 8) inflateEnd(&zip);
    const auto closing = Clock::now();
    const int closed = std::fclose(out);
    g_write_seconds += Seconds(closing);
    if (closed != 0 && ok) {
        ok = false;
        error = "writing to the card failed (is it full?)";
    }
    if (ok && (written != entry.size || crc != entry.crc)) {
        ok = false;
        error = "a file arrived damaged";
    }
    if (!ok) {
        fs::remove(target, fs_error);
        return false;
    }
    return true;
}

bool DownloadRetroRewind(const fs::path& folder) {
    if (KAIHEN_RR_VERSION[0] == '\0') {
        Fail("No Retro Rewind folder on the card.", "Copy your RetroRewind6 folder to wiicompiled/RetroRewind on "
             "the card, then open the app again.");
        return false;
    }
    const std::string url = RetroRewindUrl();
    SetStep("Getting Retro Rewind " KAIHEN_RR_VERSION, "Connecting to Retro Rewind's server");
    std::vector<ZipEntry> entries;
    uint64_t run_end = 0;
    std::string error;
    if (!ReadZipDirectory(url, entries, run_end, error)) {
        Fail("Couldn't reach Retro Rewind's server: " + error + ".",
             "Connect the console to the internet, then open the app again.");
        return false;
    }
    const fs::path partial = folder.string() + ".partial";
    std::error_code fs_error;
    fs::create_directories(partial, fs_error);
    // An earlier, interrupted download: carry on from its first missing file.
    size_t next = 0;
    while (next < entries.size()) {
        const ZipEntry& entry = entries[next];
        const fs::path target = partial / fs::path(entry.name);
        const bool done = entry.name.back() == '/' ? fs::is_directory(target, fs_error)
                                                   : fs::is_regular_file(target, fs_error) &&
                                                         fs::file_size(target, fs_error) == entry.size;
        if (!done) break;
        ++next;
    }
    uint64_t needed = 0;
    for (size_t i = next; i < entries.size(); ++i) needed += entries[i].size;
    if (FreeBytes(folder.parent_path()) < needed + (256ull << 20)) {
        Fail("The microSD card is too full.", "Retro Rewind needs about 2.5 GB free. Make room, then open the app "
             "again; the download carries on where it stopped.");
        return false;
    }
    const uint64_t run_start = entries.front().offset;
    SetStep("Getting Retro Rewind " KAIHEN_RR_VERSION, "Downloading and unpacking (about 1.9 GB)");
    const auto started = Clock::now();
    const size_t first = next;
    int reported = 0;
    int failures = 0;
    while (next < entries.size()) {
        RunReader reader;
        if (!reader.get.Open(url, entries[next].offset, run_end - 1, error)) {
            if (++failures > 5) break;
            std::this_thread::sleep_for(std::chrono::seconds(2 * failures));
            continue;
        }
        while (next < entries.size()) {
            if (!UnpackEntry(reader, entries[next], partial, error)) break;
            ++next;
            failures = 0;
            const float progress = float(double(entries[next - 1].offset + entries[next - 1].packed - run_start) /
                                         double(run_end - run_start));
            if (int(progress * 10) > reported) {
                reported = int(progress * 10);
                RT_LOGF("setup", "Retro Rewind %d%% after %.0f s (network %.0f s, card %.0f s)\n", reported * 10,
                        Seconds(started), g_network_seconds, g_write_seconds);
            }
            std::lock_guard<std::mutex> lock(g_status.mutex);
            g_status.progress = progress;
        }
        if (next < entries.size()) {
            RT_LOGF("setup", "download interrupted at %s: %s\n", entries[next].name.c_str(), error.c_str());
            if (++failures > 5) break;
            std::this_thread::sleep_for(std::chrono::seconds(2 * failures));
        }
    }
    if (next < entries.size()) {
        Fail("The download stopped: " + error + ".",
             "Check the console's internet connection, then open the app again: it carries on where it stopped.");
        return false;
    }

    {
        const double seconds = Seconds(started);
        const double megabytes = double(entries.back().offset + entries.back().packed - entries[first].offset) / 1e6;
        RT_LOGF("setup", "Retro Rewind downloaded: %.0f MB in %.0f s (%.1f MB/s): network %.0f s, card %.0f s, "
                "%zu files\n", megabytes, seconds, megabytes / std::max(seconds, 0.001), g_network_seconds,
                g_write_seconds, entries.size() - first);
    }
    SetStep("Checking Retro Rewind");
    if (!RetroRewindReady(partial)) {
        Fail("The downloaded Retro Rewind doesn't match this app.", "Open the app again to retry the download.");
        fs::remove_all(partial, fs_error);
        return false;
    }
    // A Retro Rewind folder already on the card (another release): files the new one lacks, such
    // as saved ghosts, move over before the old folder goes.
    if (fs::exists(folder, fs_error)) {
        for (auto it = fs::recursive_directory_iterator(folder, fs_error); it != fs::recursive_directory_iterator();
             it.increment(fs_error)) {
            if (fs_error || !it->is_regular_file(fs_error)) continue;
            const fs::path target = partial / fs::relative(it->path(), folder, fs_error);
            if (!fs::exists(target, fs_error)) {
                fs::create_directories(target.parent_path(), fs_error);
                fs::rename(it->path(), target, fs_error);
            }
        }
        const fs::path old = folder.string() + ".old";
        fs::remove_all(old, fs_error);
        fs::rename(folder, old, fs_error);
        fs::remove_all(old, fs_error);
    }
    fs::create_directories(folder.parent_path(), fs_error);
    fs::rename(partial, folder, fs_error);
    if (fs_error) {
        Fail("Couldn't finish installing Retro Rewind: " + fs_error.message() + ".", "Open the app again to retry.");
        return false;
    }
    RT_LOGF("setup", "Retro Rewind %s installed\n", KAIHEN_RR_VERSION);
    return true;
}

void Work(fs::path root, fs::path data, fs::path retro_rewind) {
    if (!data.empty() && !DiscReady(data) && !ImportDisc(root, data)) {
        return;
    }
    if (!retro_rewind.empty() && !RetroRewindReady(retro_rewind) && !DownloadRetroRewind(retro_rewind)) {
        return;
    }
    g_status.finished = true;
}

// --- The screen ----------------------------------------------------------------------------------

void CenteredText(const std::string& text, float y, float wrap) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).x, wrap);
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - width) * 0.5f), y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

// One screen: a title, lines of text, an optional progress bar, and a line at the bottom.
struct Page {
    std::string title;
    std::vector<std::string> lines;  // the first is the step, the rest details
    float progress = -1;
    std::string footer;
};

void DrawPage(const Page& page) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("kaiHEN setup", nullptr, kFlags)) {
        const float wrap = viewport->Size.x * 0.8f;
        float y = viewport->Size.y * 0.3f;
        ImGui::SetWindowFontScale(1.25f);
        CenteredText(page.title, y, wrap);
        y += ImGui::GetTextLineHeightWithSpacing() * 2;
        ImGui::SetWindowFontScale(1.0f);
        for (const auto& line : page.lines) {
            if (line.empty()) {
                y += ImGui::GetTextLineHeight();  // a gap
                continue;
            }
            CenteredText(line, y, wrap);
            y = ImGui::GetCursorPosY() + 8;
        }
        if (page.progress >= 0) {
            ImGui::SetCursorPos(ImVec2((viewport->Size.x - wrap * 0.6f) * 0.5f, y + 8));
            char label[16];
            std::snprintf(label, sizeof label, "%d%%", int(page.progress * 100));
            ImGui::ProgressBar(page.progress, ImVec2(wrap * 0.6f, 0), label);
        }
        if (!page.footer.empty()) {
            CenteredText(page.footer, viewport->Size.y * 0.85f, wrap);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

std::string ProductName() { return std::string(RuntimeProduct::Active().displayName); }

// The setup screen, from g_status.
void Draw() {
    Page page;
    page.title = ProductName() + ": first-time setup";
    {
        std::lock_guard<std::mutex> lock(g_status.mutex);
        if (g_status.failed) {
            page.lines = {"Setup stopped", g_status.detail, "", g_status.advice};
            page.footer = "Hold + and - to return to hbmenu.";
        } else {
            page.lines = {g_status.step, g_status.detail};
            page.progress = g_status.progress;
            page.footer = "This happens once. Keep the console on and the card in.";
        }
    }
    DrawPage(page);
}

// --- Updates -------------------------------------------------------------------------------------
// B on the game's startup screen opens the update screen (UpdatesFrame). It looks for a newer
// release of this app on GitHub (KAIHEN_UPDATE_REPO) and Retro Rewind's latest version on its
// server. A new app replaces this one's .nro; it brings its matching Retro Rewind at its next start.

// "0.4.1" -> {0, 4, 1}; compares like versions.
std::vector<int> Version(const std::string& text) {
    std::vector<int> parts;
    size_t at = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        parts.push_back(std::atoi(text.c_str() + at));
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (at < text.size() && text[at] == '.') ++at; else break;
    }
    return parts;
}
bool Newer(const std::string& a, const std::string& b) { return Version(a) > Version(b); }

bool FetchText(const std::string& url, std::string& text, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!Fetch(url, 0, UINT64_MAX, bytes, nullptr, error)) return false;
    text.assign(bytes.begin(), bytes.end());
    return true;
}

struct Updates {
    std::mutex mutex;
    enum class Phase { Checking, Checked, Downloading, Updated, Failed } phase = Phase::Checking;
    std::vector<std::string> lines;
    float progress = -1;
    std::string latest;        // a newer app's version, or empty
    std::string download_url;  // its .nro
    std::string sums_url;      // its release's SHA256SUMS
};
Updates g_updates;

void SetUpdates(Updates::Phase phase, std::vector<std::string> lines) {
    std::lock_guard<std::mutex> lock(g_updates.mutex);
    g_updates.phase = phase;
    g_updates.lines = std::move(lines);
    g_updates.progress = -1;
}

void CheckForUpdates() {
    const std::string product = std::string(RuntimeProduct::IsRetroRewind() ? "RetroRewind" : "WiiCompiled");
    std::string error, latest, download_url, sums_url;
    // The releases' assets: .../releases/download/<tag>/<product>-<version>.nro
    std::string releases;
    if (KAIHEN_UPDATE_REPO[0] == '\0' ||
        !FetchText(std::string("https://api.github.com/repos/") + KAIHEN_UPDATE_REPO + "/releases?per_page=20",
                   releases, error)) {
        SetUpdates(Updates::Phase::Failed, {"Couldn't check for updates: " + (error.empty() ? "no address" : error) + ".",
                                            "Check the console's internet connection."});
        return;
    }
    const std::string key = "\"browser_download_url\":";
    const std::string suffix_start = "/" + product + "-";
    for (size_t at = releases.find(key); at != std::string::npos; at = releases.find(key, at + 1)) {
        const size_t begin = releases.find('"', at + key.size()) + 1, end = releases.find('"', begin);
        if (begin == 0) break;
        if (end == std::string::npos) break;
        const std::string url = releases.substr(begin, end - begin);
        const size_t name = url.rfind(suffix_start);
        if (name == std::string::npos || url.size() < 4 || url.compare(url.size() - 4, 4, ".nro") != 0) continue;
        const std::string version = url.substr(name + suffix_start.size(), url.size() - 4 - name - suffix_start.size());
        if (Newer(version, KAIHEN_APP_VERSION) && (latest.empty() || Newer(version, latest))) {
            latest = version;
            download_url = url;
            sums_url = url.substr(0, url.rfind('/')) + "/SHA256SUMS";
        }
    }
    std::vector<std::string> lines;
    if (!latest.empty()) {
        lines = {ProductName() + " " + latest + " is available. You have " KAIHEN_APP_VERSION ".", "",
                 "Press A to download it."};
    } else {
        lines = {"This app is up to date (" KAIHEN_APP_VERSION ")."};
    }
    if (RuntimeProduct::IsRetroRewind() && KAIHEN_RR_VERSION[0] != '\0') {
        std::string versions, newest;
        if (FetchText("https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt", versions, error)) {
            size_t start = 0;
            while (start < versions.size()) {
                size_t end = versions.find('\n', start);
                if (end == std::string::npos) end = versions.size();
                const std::string version = versions.substr(start, versions.find(' ', start) - start);
                if (newest.empty() || Newer(version, newest)) newest = version;
                start = end + 1;
            }
        }
        if (!newest.empty() && Newer(newest, KAIHEN_RR_VERSION) && latest.empty()) {
            lines.push_back("");
            lines.push_back("Retro Rewind " + newest + " is out. It needs a newer version of this app, which isn't "
                            "out yet. This one plays Retro Rewind " KAIHEN_RR_VERSION ".");
        } else if (latest.empty()) {
            lines.push_back("Retro Rewind " KAIHEN_RR_VERSION " is the latest.");
        }
    }
    std::lock_guard<std::mutex> lock(g_updates.mutex);
    g_updates.phase = Updates::Phase::Checked;
    g_updates.lines = std::move(lines);
    g_updates.latest = latest;
    g_updates.download_url = download_url;
    g_updates.sums_url = sums_url;
}

void DownloadUpdate() {
    std::string url, sums_url, latest, error;
    {
        std::lock_guard<std::mutex> lock(g_updates.mutex);
        url = g_updates.download_url;
        sums_url = g_updates.sums_url;
        latest = g_updates.latest;
    }
    const std::string app = kaihen_app_path();
    if (app.empty()) {
        SetUpdates(Updates::Phase::Failed, {"Couldn't find this app's file on the card."});
        return;
    }
    const std::string file_name = url.substr(url.rfind('/') + 1);
    SetUpdates(Updates::Phase::Downloading, {"Downloading " + ProductName() + " " + latest, file_name});
    std::string sums, expected;
    if (!FetchText(sums_url, sums, error)) {
        SetUpdates(Updates::Phase::Failed, {"Couldn't download the update's checksums: " + error + "."});
        return;
    }
    for (size_t at = sums.find(file_name); at != std::string::npos; at = sums.find(file_name, at + 1)) {
        const size_t line = sums.rfind('\n', at) == std::string::npos ? 0 : sums.rfind('\n', at) + 1;
        if (at >= line + 64) expected = Lower(sums.substr(line, 64));
        break;
    }
    if (expected.size() != 64) {
        SetUpdates(Updates::Phase::Failed, {"The update's checksums don't list " + file_name + "."});
        return;
    }
    const std::string partial = app + ".partial";
    FILE* out = std::fopen(partial.c_str(), "wb");
    kaihen_https::Get get;
    if (!out || !get.Open(url, 0, UINT64_MAX, error)) {
        if (out) std::fclose(out);
        std::remove(partial.c_str());
        SetUpdates(Updates::Phase::Failed, {"Couldn't download the update: " + (out ? error : "can't write to the card") + "."});
        return;
    }
    static std::vector<char> file_buffer(1 << 18);
    std::setvbuf(out, file_buffer.data(), _IOFBF, file_buffer.size());
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::vector<uint8_t> buffer(1 << 16);
    uint64_t done = 0;
    const uint64_t length = get.Length();
    int64_t n;
    bool ok = true;
    while ((n = get.Read(buffer.data(), buffer.size(), error)) > 0) {
        mbedtls_sha256_update(&sha, buffer.data(), size_t(n));
        if (std::fwrite(buffer.data(), 1, size_t(n), out) != size_t(n)) {
            ok = false;
            error = "writing to the card failed (is it full?)";
            break;
        }
        done += uint64_t(n);
        if (length != UINT64_MAX && length != 0) {
            std::lock_guard<std::mutex> lock(g_updates.mutex);
            g_updates.progress = float(double(done) / double(length));
        }
    }
    if (ok && n < 0) ok = false;
    if (std::fclose(out) != 0 && ok) {
        ok = false;
        error = "writing to the card failed (is it full?)";
    }
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (ok && expected != hex) {
        ok = false;
        error = "the download doesn't match its checksum";
    }
    if (!ok) {
        std::remove(partial.c_str());
        SetUpdates(Updates::Phase::Failed, {"The update didn't download: " + error + ".", "Try again later."});
        return;
    }
    // The new file takes the app's place; the old one stays until the new one is in.
    const std::string old = app + ".old";
    std::remove(old.c_str());
    if (std::rename(app.c_str(), old.c_str()) != 0 || std::rename(partial.c_str(), app.c_str()) != 0) {
        std::rename(old.c_str(), app.c_str());
        std::remove(partial.c_str());
        SetUpdates(Updates::Phase::Failed, {"Couldn't replace the app's file on the card."});
        return;
    }
    std::remove(old.c_str());
    RT_LOGF("setup", "updated %s to %s\n", app.c_str(), latest.c_str());
    SetUpdates(Updates::Phase::Updated,
               {"Updated to " + latest + ".",
                "To use it, hold + and - to return to hbmenu, restart kaiHEN, and open the app again."
                + std::string(RuntimeProduct::IsRetroRewind() ? " It gets its Retro Rewind then." : "")});
    {
        std::lock_guard<std::mutex> lock(g_updates.mutex);
        g_updates.progress = 1;
    }
}

// The update screen's state on the game thread.
bool g_updates_open = false;
std::thread g_updates_worker;
std::atomic<bool> g_updates_working{false};  // the worker hasn't finished

void StartUpdatesWork(void (*work)()) {
    if (g_updates_worker.joinable()) {
        // A check left while it hung goes on alone; its answer is as good as the new one's.
        if (g_updates_working.load()) g_updates_worker.detach(); else g_updates_worker.join();
    }
    g_updates_working = true;
    g_updates_worker = std::thread([work] {
        work();
        g_updates_working = false;
    });
}

}  // namespace

bool UpdatesOpen() { return g_updates_open; }

void UpdatesFrame(bool startup_screen_visible) {
    const uint32_t buttons = kaihen_setup_buttons();
    if (!g_updates_open) {
        if (startup_screen_visible && (buttons & 2)) {
            RT_LOGF("setup", "update screen\n");
            g_updates_open = true;
            SetUpdates(Updates::Phase::Checking, {});
            StartUpdatesWork(CheckForUpdates);
        }
        return;
    }
    Page page;
    page.title = ProductName() + ": updates";
    Updates::Phase phase;
    bool can_download;
    {
        std::lock_guard<std::mutex> lock(g_updates.mutex);
        phase = g_updates.phase;
        page.lines = phase == Updates::Phase::Checking ? std::vector<std::string>{"Checking for updates..."}
                                                       : g_updates.lines;
        if (phase == Updates::Phase::Downloading) page.progress = g_updates.progress;
        can_download = phase == Updates::Phase::Checked && !g_updates.latest.empty();
    }
    // A check can be left (with no internet, connecting may take a minute); a download can't.
    if (phase == Updates::Phase::Checking) {
        page.footer = "B: back to the game.";
        DrawPage(page);
        if (buttons & 2) g_updates_open = false;
        return;
    }
    const bool busy = phase == Updates::Phase::Downloading;
    page.footer = busy ? "Keep the console on." : "B: back to the game.";
    DrawPage(page);
    if (busy) return;
    if (can_download && (buttons & 1)) {
        SetUpdates(Updates::Phase::Downloading, {"Starting the download..."});
        StartUpdatesWork(DownloadUpdate);
    } else if (buttons & 2) {
        g_updates_open = false;
    }
}


void Run() {
    const fs::path root = RuntimeConfigFile::kaihen_root_directory();
    const fs::path data = RuntimeConfigFile::ResolvedDvdRoot();
    const fs::path retro_rewind =
        RuntimeProduct::IsRetroRewind() ? fs::path(RuntimeConfigFile::RetroRewindRoot()) : fs::path{};
    const bool disc_ready = data.empty() || DiscReady(data);
    const bool retro_rewind_ready = retro_rewind.empty() || RetroRewindReady(retro_rewind);
    if (!disc_ready || !retro_rewind_ready) {
        RT_LOGF("setup", "first-time setup: disc %s%s, Retro Rewind %s%s (app built for %s)\n", data.c_str(),
                disc_ready ? " (ready)" : "", retro_rewind.c_str(), retro_rewind_ready ? " (ready)" : "",
                KAIHEN_RR_VERSION);
        std::thread worker(Work, root, data, retro_rewind);
        while (!g_status.finished.load()) {
            if (kaihen_return_requested()) {
                worker.detach();  // it's past the point of no return only when finished
                settings_overlay::ConsoleReturnToLauncher();
            }
            aurora_update();
            if (aurora_begin_frame()) {
                Draw();
                aurora_end_frame();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }
        worker.join();
        RT_LOGF("setup", "done\n");
    }
}

}  // namespace kaihen_setup
