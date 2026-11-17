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

}  // namespace

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
    return DownloadRetroRewind(folder);
}

}  // namespace kaikart
