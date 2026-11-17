// KaiKart's install steps (kaikart_install.h): the disc imported from an image on the card, and the
// Retro Rewind release a game build is made for, from Retro Rewind's own server. Moved here from
// the games' first-launch setup so the launcher can run them before a game starts.
#include "kaikart_install.h"

#include "kaihen_https.h"
#include "wii_disc_extractor.h"

#include <mbedtls/sha256.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace kaikart {
namespace {

// A clean PAL Mario Kart Wii, revision 0 (the builds' translation is made from it).
constexpr const char* kMainDolSha256 = "80D18895B39C63BD80F457398BFCBB91B7D16AC116A41A88967E954080155B05";
constexpr const char* kStaticRSha256 = "16D9D146112541FEFEA701ECB5BC1A496F9D50E4A752FBB5B6778E7C6399F67D";
constexpr uint64_t kDiscBytes = 3ull << 30;  // room the extracted disc needs, with some to spare

Progress* g_progress = nullptr;
std::string g_rr_version, g_rr_sha256;
void (*g_log)(const char* line) = nullptr;

void Log(const char* format, ...) {
    if (!g_log) return;
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    g_log(line);
}

void SetStep(const std::string& step, const std::string& detail = {}) {
    std::lock_guard<std::mutex> lock(g_progress->mutex);
    g_progress->step = step;
    g_progress->detail = detail;
    g_progress->fraction = -1;
    Log("%s %s\n", step.c_str(), detail.c_str());
}

void Fail(const std::string& detail, const std::string& advice) {
    std::lock_guard<std::mutex> lock(g_progress->mutex);
    g_progress->failed = true;
    g_progress->detail = detail;
    g_progress->advice = advice;
    Log("failed: %s\n", detail.c_str());
}

std::string Sha256(const fs::path& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::vector<unsigned char> buffer(1 << 18);
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) mbedtls_sha256_update(&sha, buffer.data(), n);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (failed) return {};
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", digest[i]);
    return hex;
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

bool DiscReadyAt(const fs::path& data) {
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

bool ImportDiscImage(const fs::path& root, const fs::path& data) {
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
    SetStep("Importing " + image[0].filename().string());
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
                std::lock_guard<std::mutex> lock(g_progress->mutex);
                g_progress->fraction = float(std::min(1.0, double(WiiDiscExtractor_BytesDone()) / double(total)));
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
        Log("disc extracted: %.0f MB in %.0f s (%.1f MB/s)\n", megabytes, seconds,
                megabytes / std::max(seconds, 0.001));
    }
    if (result != 0) {
        fs::remove_all(partial, error);
        Fail(message, "Use your own clean, unmodified PAL Mario Kart Wii disc (RMCP01).");
        return false;
    }

    SetStep("Checking the disc");
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
    Log("disc imported from %s\n", image[0].c_str());
    return true;
}

// --- Retro Rewind --------------------------------------------------------------------------------
// The release the app was built for, from Retro Rewind's own server: the full zip of each release
// stays there. Only its RetroRewind6 folder is needed, one run of entries in the zip, so that run
// is streamed and unpacked as it arrives: the zip itself never lands on the card.

constexpr uint64_t kRetroRewindBytes = 5ull << 29;  // 2.2 GB unpacked, with some to spare
constexpr const char* kRetroRewindPrefix = "RetroRewind6/";

std::string RetroRewindUrl() {
    return std::string("https://cdn.update.rwfc.net/RetroRewind/zip/") + g_rr_version + "-full.zip";
}

bool RetroRewindReadyAt(const fs::path& folder) {
    if (g_rr_sha256.empty()) {
        std::error_code error;
        return fs::is_regular_file(folder / "Binaries" / "Code.pul", error);  // a build without a pin
    }
    return Sha256(folder / "Binaries" / "Code.pul") == g_rr_sha256;
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
    std::vector<uint8_t> buffer(65536);
    int64_t n;
    while ((n = get.Read(buffer.data(), buffer.size(), error)) > 0) out.insert(out.end(), buffer.data(), buffer.data() + n);
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
    if (g_rr_version.empty()) {
        Fail("No Retro Rewind folder on the card.", "Copy your RetroRewind6 folder to wiicompiled/RetroRewind on "
             "the card, then open the app again.");
        return false;
    }
    const std::string url = RetroRewindUrl();
    SetStep("Connecting to Retro Rewind's server");
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
    SetStep("Downloading (about 1.9 GB)");
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
                Log("Retro Rewind %d%% after %.0f s (network %.0f s, card %.0f s)\n", reported * 10,
                        Seconds(started), g_network_seconds, g_write_seconds);
            }
            std::lock_guard<std::mutex> lock(g_progress->mutex);
            g_progress->fraction = progress;
        }
        if (next < entries.size()) {
            Log("download interrupted at %s: %s\n", entries[next].name.c_str(), error.c_str());
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
        Log("Retro Rewind downloaded: %.0f MB in %.0f s (%.1f MB/s): network %.0f s, card %.0f s, "
                "%zu files\n", megabytes, seconds, megabytes / std::max(seconds, 0.001), g_network_seconds,
                g_write_seconds, entries.size() - first);
    }
    SetStep("Checking the download");
    if (!RetroRewindReadyAt(partial)) {
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
    Log("Retro Rewind %s installed\n", g_rr_version.c_str());
    return true;
}

// --- Retro Rewind's small updates ------------------------------------------------------------------
// Retro Rewind publishes a zip per release (RetroRewindVersion.txt: "version url path description")
// and the files each release removes (RetroRewindDelete.txt: "version path"), which is how Wheel
// Wizard updates. From the card's version to the app's: the removals, then each zip unpacked over
// the folder. Anything that doesn't line up leaves the full download to do the job.

constexpr const char* kVersionList = "https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt";
constexpr const char* kDeleteList = "https://update.rwfc.net/RetroRewind/RetroRewindDelete.txt";

// "6.13.5" -> {6, 13, 5}; compares like versions.
std::vector<int> VersionParts(const std::string& text) {
    std::vector<int> parts;
    size_t at = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        parts.push_back(std::atoi(text.c_str() + at));
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (at < text.size() && text[at] == '.') ++at; else break;
    }
    return parts;
}

std::string CardRetroRewindVersion(const fs::path& folder) {
    FILE* file = std::fopen((folder / "version.txt").c_str(), "rb");
    if (!file) return {};
    char text[64] = {};
    const size_t n = std::fread(text, 1, sizeof text - 1, file);
    std::fclose(file);
    std::string version(text, n);
    while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) version.pop_back();
    while (!version.empty() && std::isspace(static_cast<unsigned char>(version.front()))) version.erase(0, 1);
    return version;
}

bool FetchString(const std::string& url, std::string& text, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!Fetch(url, 0, UINT64_MAX, bytes, nullptr, error)) return false;
    text.assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<std::vector<std::string>> Lines(const std::string& text) {
    std::vector<std::vector<std::string>> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> words;
        size_t at = start;
        while (at < end) {
            while (at < end && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
            const size_t word = at;
            while (at < end && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
            if (at > word) words.push_back(text.substr(word, at - word));
        }
        if (!words.empty()) lines.push_back(std::move(words));
        start = end + 1;
    }
    return lines;
}

// True when the folder is now the app's release (Code.pul checked); false leaves it to the full
// download, which replaces the folder anyway.
bool UpdateRetroRewind(const fs::path& folder) {
    const std::string from = CardRetroRewindVersion(folder);
    const auto from_parts = VersionParts(from), to_parts = VersionParts(g_rr_version);
    if (from_parts.empty() || from_parts >= to_parts) return false;
    Log("Retro Rewind update from %s to %s\n", from.c_str(), g_rr_version.c_str());
    SetStep("Connecting to Retro Rewind's server");
    std::string versions, deletions, error;
    if (!FetchString(kVersionList, versions, error) || !FetchString(kDeleteList, deletions, error)) {
        Log("Retro Rewind update lists: %s\n", error.c_str());
        return false;
    }
    std::vector<std::pair<std::vector<int>, std::string>> zips;  // version, url
    for (const auto& line : Lines(versions)) {
        if (line.size() < 2) continue;
        const auto parts = VersionParts(line[0]);
        if (parts > from_parts && parts <= to_parts) zips.push_back({parts, line[1]});
    }
    std::sort(zips.begin(), zips.end());
    if (zips.empty() || zips.back().first != to_parts) {
        Log("Retro Rewind update: no zip chain reaches %s\n", g_rr_version.c_str());
        return false;
    }
    for (auto& zip : zips) {
        // The list still names an old address for some; Wheel Wizard swaps it the same way.
        const std::string legacy = "http://update.rwfc.net:8000/";
        if (zip.second.compare(0, legacy.size(), legacy) == 0) {
            zip.second = "https://update.rwfc.net/" + zip.second.substr(legacy.size());
        }
        if (zip.second.compare(0, 8, "https://") != 0) return false;
    }
    // The removals, inside the folder that holds RetroRewind6 only.
    const fs::path root = folder.parent_path();
    std::error_code fs_error;
    for (const auto& line : Lines(deletions)) {
        if (line.size() < 2) continue;
        const auto parts = VersionParts(line[0]);
        if (!(parts > from_parts && parts <= to_parts)) continue;
        std::string relative = line[1];
        while (!relative.empty() && (relative[0] == '/' || relative[0] == '\\')) relative.erase(0, 1);
        if (relative.empty() || relative.find("..") != std::string::npos) continue;
        fs::remove_all(root / fs::path(relative), fs_error);
    }
    SetStep("Downloading the updates");
    for (size_t z = 0; z < zips.size(); ++z) {
        const std::string& url = zips[z].second;
        std::vector<ZipEntry> entries;
        uint64_t run_end = 0;
        int failures = 0;
        bool done = false;
        while (!done && failures <= 3) {
            entries.clear();
            if (!ReadZipDirectory(url, entries, run_end, error)) {
                ++failures;
                continue;
            }
            RunReader reader;
            if (!reader.get.Open(url, entries.front().offset, run_end - 1, error)) {
                ++failures;
                continue;
            }
            size_t next = 0;
            while (next < entries.size() && UnpackEntry(reader, entries[next], folder, error)) ++next;
            done = next == entries.size();
            if (!done) ++failures;
        }
        if (!done) {
            Log("Retro Rewind update %s failed: %s\n", url.c_str(), error.c_str());
            return false;
        }
        std::lock_guard<std::mutex> lock(g_progress->mutex);
        g_progress->fraction = float(z + 1) / float(zips.size());
    }
    SetStep("Checking the update");
    const bool ok = RetroRewindReadyAt(folder);
    Log("Retro Rewind update to %s %s\n", g_rr_version.c_str(), ok ? "done" : "didn't match; full download");
    return ok;
}

}  // namespace

// --- KaiKart's own releases --------------------------------------------------------------------------

bool VersionNewer(const std::string& a, const std::string& b) { return VersionParts(a) > VersionParts(b); }

std::string NroVersion(const fs::path& nro) {
    // NRO: "NRO0" at 0x10 and the NRO's size at 0x18; its assets follow ("ASET", then the icon's and
    // the NACP's offset and size); the NACP's display version is at 0x3060.
    FILE* file = std::fopen(nro.c_str(), "rb");
    if (!file) return {};
    std::string version;
    uint8_t header[0x20];
    if (std::fread(header, 1, sizeof header, file) == sizeof header && std::memcmp(header + 0x10, "NRO0", 4) == 0) {
        const uint32_t size = Le32(header + 0x18);
        uint8_t assets[0x38];
        if (std::fseek(file, long(size), SEEK_SET) == 0 && std::fread(assets, 1, sizeof assets, file) == sizeof assets &&
            std::memcmp(assets, "ASET", 4) == 0) {
            uint64_t nacp = 0;
            std::memcpy(&nacp, assets + 0x18, 8);
            char text[16] = {};
            if (std::fseek(file, long(size + nacp + 0x3060), SEEK_SET) == 0 && std::fread(text, 1, 15, file) > 0) {
                version.assign(text);
            }
        }
    }
    std::fclose(file);
    return version;
}

bool NewerRelease(const std::string& repo, const std::string& name, const std::string& current, Release& found,
                  std::string& error) {
    std::string releases;
    if (repo.empty() ||
        !FetchString("https://api.github.com/repos/" + repo + "/releases?per_page=20", releases, error)) {
        if (repo.empty()) error = "no update address in this build";
        return false;
    }
    // Each release's assets: "browser_download_url":".../releases/download/<tag>/<name>-<version>.nro"
    const std::string key = "\"browser_download_url\":", prefix = "/" + name + "-";
    found = {};
    for (size_t at = releases.find(key); at != std::string::npos; at = releases.find(key, at + 1)) {
        const size_t quote = releases.find('"', at + key.size());
        if (quote == std::string::npos) break;
        const size_t begin = quote + 1, end = releases.find('"', begin);
        if (end == std::string::npos) break;
        const std::string url = releases.substr(begin, end - begin);
        const size_t file = url.rfind(prefix);
        if (file == std::string::npos || url.size() < 4 || url.compare(url.size() - 4, 4, ".nro") != 0) continue;
        const std::string version = url.substr(file + prefix.size(), url.size() - 4 - file - prefix.size());
        if (VersionNewer(version, current) && (found.version.empty() || VersionNewer(version, found.version))) {
            found.version = version;
            found.download_base = url.substr(0, file);
        }
    }
    return true;
}

bool DownloadChecked(const std::string& url, const std::string& sums_url, const fs::path& path, Progress& progress) {
    g_progress = &progress;
    const std::string file_name = url.substr(url.rfind('/') + 1);
    SetStep("Downloading " + file_name);
    std::string sums, expected, error;
    if (!FetchString(sums_url, sums, error)) {
        Fail("Couldn't get the release's checksums: " + error + ".", "Check the console's internet connection.");
        return false;
    }
    for (const auto& line : Lines(sums)) {
        if (line.size() >= 2 && (line[1] == file_name || line[1] == "*" + file_name) && line[0].size() == 64) {
            expected = line[0];
        }
    }
    if (expected.empty()) {
        Fail("The release's checksums don't list " + file_name + ".", "Try again later.");
        return false;
    }
    for (auto& c : expected) c = char(std::toupper(static_cast<unsigned char>(c)));
    const fs::path partial = path.string() + ".partial";
    std::error_code fs_error;
    fs::create_directories(path.parent_path(), fs_error);
    FILE* out = std::fopen(partial.c_str(), "wb");
    kaihen_https::Get get;
    if (!out || !get.Open(url, 0, UINT64_MAX, error)) {
        if (out) std::fclose(out);
        std::remove(partial.c_str());
        Fail("Couldn't download " + file_name + ": " + (out ? error : "can't write to the card") + ".",
             "Check the console's internet connection.");
        return false;
    }
    static std::vector<char> file_buffer(1 << 18);
    std::setvbuf(out, file_buffer.data(), _IOFBF, file_buffer.size());
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::vector<uint8_t> buffer(1 << 16);
    const uint64_t length = get.Length();
    uint64_t done = 0;
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
            std::lock_guard<std::mutex> lock(progress.mutex);
            progress.fraction = float(double(done) / double(length));
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
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", digest[i]);
    if (ok && expected != hex) {
        ok = false;
        error = "it doesn't match its checksum";
    }
    if (!ok) {
        std::remove(partial.c_str());
        Fail(file_name + " didn't download: " + error + ".", "Try again later.");
        return false;
    }
    // The new file takes the old one's place; the old one stays until the new one is in.
    const std::string old = path.string() + ".old";
    std::remove(old.c_str());
    const bool had_old = std::rename(path.c_str(), old.c_str()) == 0;
    if (std::rename(partial.c_str(), path.c_str()) != 0) {
        if (had_old) std::rename(old.c_str(), path.c_str());
        std::remove(partial.c_str());
        Fail("Couldn't put " + file_name + " in place on the card.", "Try again.");
        return false;
    }
    std::remove(old.c_str());
    Log("downloaded %s to %s\n", file_name.c_str(), path.c_str());
    return true;
}

void SetLog(void (*log)(const char* line)) { g_log = log; }

bool DiscReady(const fs::path& data) { return DiscReadyAt(data); }

bool ImportDisc(const fs::path& root, const fs::path& data, Progress& progress) {
    g_progress = &progress;
    const bool ok = ImportDiscImage(root, data);
    if (ok) {
        std::lock_guard<std::mutex> lock(progress.mutex);
        progress.fraction = 1;
    }
    return ok;
}

bool RetroRewindReady(const fs::path& folder, const std::string& code_sha256) {
    g_rr_sha256 = code_sha256;
    return RetroRewindReadyAt(folder);
}

bool InstallRetroRewind(const fs::path& folder, const std::string& version, const std::string& code_sha256,
                        Progress& progress) {
    g_progress = &progress;
    g_rr_version = version;
    g_rr_sha256 = code_sha256;
    if (UpdateRetroRewind(folder)) return true;
    {
        std::lock_guard<std::mutex> lock(progress.mutex);
        progress.fraction = -1;
    }
    return DownloadRetroRewind(folder);
}

}  // namespace kaikart
