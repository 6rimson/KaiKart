#pragma once

#include "isa/big_endian.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DvdFstContract {

struct RegisteredFile {
    std::filesystem::path hostPath;
    std::string dvdPath;
    uint32_t size = 0;
    uint32_t discOffsetWords = 0;
};

struct IndexedEntry {
    std::filesystem::path hostPath;
    std::string dvdPath;
    uint32_t size = 0;
    uint32_t discOffsetWords = 0;
    uint32_t parentIndex = 0;
    uint32_t subtreeEnd = 0;
    bool isDirectory = false;
};

struct Image {
    std::vector<IndexedEntry> entries;
    std::map<std::string, int32_t> pathToEntry;
    std::vector<uint8_t> bytes;
};

struct GuestPlacement {
    uint32_t address = 0;
    uint32_t reservedArenaHi = 0;
};

inline std::string CanonicalizePath(const std::string& input) {
    std::string path = input;
    std::replace(path.begin(), path.end(), '\\', '/');

    std::vector<std::string> components;
    size_t cursor = 0;
    while (cursor < path.size()) {
        while (cursor < path.size() && path[cursor] == '/') {
            ++cursor;
        }
        const size_t start = cursor;
        while (cursor < path.size() && path[cursor] != '/') {
            ++cursor;
        }
        if (start == cursor) {
            continue;
        }

        std::string component = path.substr(start, cursor - start);
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
            continue;
        }
        components.push_back(std::move(component));
    }

    std::string canonical = "/";
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            canonical.push_back('/');
        }
        canonical += components[i];
    }
    return canonical;
}

inline std::string NormalizeLookupPath(const std::string& input) {
    std::string path = CanonicalizePath(input);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return path;
}

namespace Detail {

struct TreeNode {
    std::string name;
    std::map<std::string, std::unique_ptr<TreeNode>> children;
    std::optional<RegisteredFile> file;
};

inline std::vector<std::string> Components(const std::string& canonicalPath) {
    std::vector<std::string> result;
    size_t cursor = canonicalPath == "/" ? canonicalPath.size() : 1;
    while (cursor < canonicalPath.size()) {
        const size_t slash = canonicalPath.find('/', cursor);
        const size_t end = slash == std::string::npos ? canonicalPath.size() : slash;
        result.push_back(canonicalPath.substr(cursor, end - cursor));
        cursor = end + 1;
    }
    return result;
}

inline std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline uint32_t EmitTree(const TreeNode& directory,
                         uint32_t directoryIndex,
                         const std::string& directoryPath,
                         Image& image,
                         std::vector<std::string>& names) {
    for (const auto& [lookupName, childPointer] : directory.children) {
        (void)lookupName;
        const TreeNode& child = *childPointer;
        const std::string childPath = directoryPath == "/"
            ? "/" + child.name
            : directoryPath + "/" + child.name;
        const uint32_t index = static_cast<uint32_t>(image.entries.size());

        if (!child.children.empty()) {
            if (child.file.has_value()) {
                throw std::runtime_error("DVD FST path is both a file and a directory: " + childPath);
            }
            image.entries.push_back({{}, childPath, 0, 0, directoryIndex, 0, true});
            names.push_back(child.name);
            image.pathToEntry.emplace(NormalizeLookupPath(childPath), static_cast<int32_t>(index));
            image.entries[index].subtreeEnd = EmitTree(child, index, childPath, image, names);
            continue;
        }

        if (!child.file.has_value()) {
            throw std::runtime_error("DVD FST contains an empty implicit node: " + childPath);
        }
        const RegisteredFile& file = *child.file;
        image.entries.push_back({file.hostPath, childPath, file.size, file.discOffsetWords,
                                 directoryIndex, index + 1, false});
        names.push_back(child.name);
        image.pathToEntry.emplace(NormalizeLookupPath(childPath), static_cast<int32_t>(index));
    }
    return static_cast<uint32_t>(image.entries.size());
}

} // namespace Detail

inline Image BuildImage(const std::vector<RegisteredFile>& registrations) {
    // Overlay scanning deliberately registers later mappings last. Collapse those
    // mappings before assigning FST indices so one guest path has one stable entry.
    std::map<std::string, RegisteredFile> filesByPath;
    for (RegisteredFile file : registrations) {
        file.dvdPath = CanonicalizePath(file.dvdPath);
        if (file.dvdPath == "/") {
            throw std::runtime_error("DVD FST cannot register the root as a file");
        }
        filesByPath[NormalizeLookupPath(file.dvdPath)] = std::move(file);
    }

    Detail::TreeNode root;
    for (const auto& [lookupPath, file] : filesByPath) {
        (void)lookupPath;
        Detail::TreeNode* node = &root;
        const std::vector<std::string> components = Detail::Components(file.dvdPath);
        for (const std::string& component : components) {
            const std::string key = Detail::Lowercase(component);
            auto& child = node->children[key];
            if (!child) {
                child = std::make_unique<Detail::TreeNode>();
                child->name = component;
            }
            node = child.get();
        }
        node->name = components.back();
        node->file = file;
    }

    Image image;
    image.entries.push_back({{}, "/", 0, 0, 0, 0, true});
    image.pathToEntry.emplace("/", 0);
    std::vector<std::string> names(1);
    image.entries[0].subtreeEnd = Detail::EmitTree(root, 0, "/", image, names);

    if (image.entries.size() > std::numeric_limits<uint32_t>::max() / 12u) {
        throw std::runtime_error("DVD FST contains too many entries");
    }

    const size_t entriesSize = image.entries.size() * 12u;
    std::vector<uint8_t> stringTable(1, 0);
    std::vector<uint32_t> nameOffsets(image.entries.size(), 0);
    for (size_t i = 1; i < names.size(); ++i) {
        if (stringTable.size() > 0x00FFFFFFu) {
            throw std::runtime_error("DVD FST name table exceeds the Wii 24-bit offset limit");
        }
        nameOffsets[i] = static_cast<uint32_t>(stringTable.size());
        stringTable.insert(stringTable.end(), names[i].begin(), names[i].end());
        stringTable.push_back(0);
    }

    image.bytes.assign(entriesSize + stringTable.size(), 0);
    for (size_t i = 0; i < image.entries.size(); ++i) {
        const IndexedEntry& entry = image.entries[i];
        const uint32_t typeAndName = (entry.isDirectory ? 0x01000000u : 0u) | nameOffsets[i];
        const uint32_t word1 = entry.isDirectory ? entry.parentIndex : entry.discOffsetWords;
        const uint32_t word2 = entry.isDirectory ? entry.subtreeEnd : entry.size;
        BigEndian::Write32(image.bytes.data(), i * 12u + 0u, typeAndName);
        BigEndian::Write32(image.bytes.data(), i * 12u + 4u, word1);
        BigEndian::Write32(image.bytes.data(), i * 12u + 8u, word2);
    }
    std::copy(stringTable.begin(), stringTable.end(), image.bytes.begin() + entriesSize);
    return image;
}

inline std::optional<GuestPlacement> ReserveBelowArena(uint32_t arenaLo,
                                                        uint32_t arenaHi,
                                                        size_t byteCount) {
    constexpr uint32_t kAlignment = 32;
    if (byteCount == 0 || byteCount > std::numeric_limits<uint32_t>::max() || arenaHi <= arenaLo) {
        return std::nullopt;
    }
    const uint32_t size = static_cast<uint32_t>(byteCount);
    if (size > arenaHi - arenaLo) {
        return std::nullopt;
    }
    const uint32_t unaligned = arenaHi - size;
    const uint32_t address = unaligned & ~(kAlignment - 1u);
    if (address < arenaLo || static_cast<uint64_t>(address) + size > arenaHi) {
        return std::nullopt;
    }
    return GuestPlacement{address, address};
}

} // namespace DvdFstContract

namespace DvdReadContract {

inline constexpr int32_t kInterruptTransferComplete = 1;
inline constexpr int32_t kInterruptDriveError = 2;

struct LowReadCompletion {
    int32_t returnValue;
    int32_t callbackResult;
};

inline constexpr LowReadCompletion CompletionFor(bool succeeded) noexcept {
    return succeeded ? LowReadCompletion{1, kInterruptTransferComplete}
                     : LowReadCompletion{0, kInterruptDriveError};
}

enum class HostReadFailure : uint8_t {
    None,
    MissingFile,
    BadOffset,
    ShortRead,
};

inline constexpr const char* Describe(HostReadFailure failure) noexcept {
    switch (failure) {
    case HostReadFailure::None:
        return "no error";
    case HostReadFailure::MissingFile:
        return "host file is missing or cannot be opened";
    case HostReadFailure::BadOffset:
        return "read offset is outside the host file";
    case HostReadFailure::ShortRead:
        return "host file did not contain the complete requested range";
    }
    return "unknown host read error";
}

// Read into private storage first and publish it only after the complete host
// range has been obtained. Callers can therefore leave a guest DMA destination
// untouched for every failure, including a host file truncated after indexing.
#if defined(MKW_KAIHEN)
// kaiHEN: game files on the microSD card. Opening one takes about 60 ms, and the card reads about
// 16 MB/s, so the menu videos (THP), read straight through a few MB at a time, cost the game
// 75-200 ms every half second: all its threads wait on a disc read. So:
// - the last few game files stay open, with their size;
// - a file read straight through (a read that starts where the last one ended) gets its next
//   piece read ahead by a background thread, and the game's next read takes it from memory.
// The disc's files never change while the game runs.
namespace OpenGameFiles {
inline constexpr size_t kMaxOpen = 8;
inline constexpr size_t kBufferBytes = 64 * 1024;
inline constexpr uint64_t kMinAhead = 256 * 1024;
inline constexpr uint64_t kMaxAhead = 8 * 1024 * 1024;

struct File {
    std::string path;
    std::FILE* file = nullptr;
    uint64_t size = 0;
    uint64_t lastUse = 0;
    uint64_t lastEnd = UINT64_MAX;  // where the last read ended
};

// One piece being read ahead, or read: for the file that is being read straight through.
struct Ahead {
    enum class State { Empty, Wanted, Reading, Ready } state = State::Empty;
    std::string path;
    uint64_t offset = 0;
    uint64_t length = 0;
    std::vector<uint8_t> data;
};

struct Shared {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<File> files;
    Ahead ahead;
    bool workerStarted = false;
    bool workerStop = false;     // StopWorker: the worker ends
    bool workerRunning = false;  // until it has
};

inline Shared& State() {
    static Shared shared;
    return shared;
}

inline std::FILE* OpenBuffered(const std::string& path, uint64_t& size) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (!handle) {
        return nullptr;
    }
    std::setvbuf(handle, nullptr, _IOFBF, kBufferBytes);
    const off_t end = fseeko(handle, 0, SEEK_END) == 0 ? ftello(handle) : -1;
    if (end < 0) {
        std::fclose(handle);
        return nullptr;
    }
    size = static_cast<uint64_t>(end);
    return handle;
}

// Under the lock: the open file for a path, opened (and the least recently used one closed) if needed.
inline File* Get(const std::filesystem::path& hostPath) {
    static uint64_t uses = 0;
    std::vector<File>& files = State().files;
    const std::string path = hostPath.string();
    for (File& file : files) {
        if (file.path == path) {
            file.lastUse = ++uses;
            return &file;
        }
    }
    uint64_t size = 0;
    std::FILE* handle = OpenBuffered(path, size);
    if (!handle) {
        return nullptr;
    }
    if (files.size() >= kMaxOpen) {
        auto oldest = std::min_element(files.begin(), files.end(),
                                       [](const File& a, const File& b) { return a.lastUse < b.lastUse; });
        std::fclose(oldest->file);
        files.erase(oldest);
    }
    files.push_back(File{path, handle, size, ++uses});
    return &files.back();
}

// The read-ahead thread, with handles of its own so it never moves the game's file positions. It
// reads without the lock, so the game's other reads go on meanwhile.
inline void Worker() {
    Shared& shared = State();
    std::string openPath;
    std::FILE* handle = nullptr;
    std::unique_lock<std::mutex> lock(shared.lock);
    for (;;) {
        shared.changed.wait(lock, [&] { return shared.workerStop || shared.ahead.state == Ahead::State::Wanted; });
        if (shared.workerStop) {
            if (handle) {
                std::fclose(handle);
            }
            shared.workerRunning = false;
            shared.changed.notify_all();
            return;  // closed at a return (kaihen_platform.c, __wrap_pthread_detach)
        }
        Ahead& ahead = shared.ahead;
        ahead.state = Ahead::State::Reading;
        const std::string path = ahead.path;
        const uint64_t offset = ahead.offset;
        const uint64_t length = ahead.length;
        lock.unlock();
        std::vector<uint8_t> data(length);
        bool ok = true;
        if (path != openPath) {
            if (handle) {
                std::fclose(handle);
            }
            uint64_t size = 0;
            handle = OpenBuffered(path, size);
            openPath = handle ? path : std::string();
        }
        ok = handle && fseeko(handle, static_cast<off_t>(offset), SEEK_SET) == 0 &&
             std::fread(data.data(), 1, length, handle) == length;
        if (handle && !ok) {
            std::clearerr(handle);
        }
        lock.lock();
        if (ahead.state == Ahead::State::Reading && ahead.path == path && ahead.offset == offset) {
            if (ok) {
                ahead.data = std::move(data);
                ahead.state = Ahead::State::Ready;
            } else {
                ahead.state = Ahead::State::Empty;
            }
        }
        shared.changed.notify_all();
    }
}

// Under the lock: asks for the piece after a read of a file being read straight through.
inline void ReadAhead(const File& file, uint64_t length) {
    Shared& shared = State();
    if (file.lastEnd >= file.size) {
        return;
    }
    const uint64_t want = std::min(std::max<uint64_t>(length, kMinAhead), kMaxAhead);
    Ahead& ahead = shared.ahead;
    if (ahead.state == Ahead::State::Reading) {
        return;  // one at a time; the next read asks again
    }
    ahead.state = Ahead::State::Wanted;
    ahead.path = file.path;
    ahead.offset = file.lastEnd;
    ahead.length = std::min(want, file.size - file.lastEnd);
    ahead.data.clear();
    if (!shared.workerStarted) {
        shared.workerStarted = true;
        shared.workerRunning = true;
        std::thread(Worker).detach();
    }
    shared.changed.notify_all();
}

// Under the lock: copies [offset, offset + length) out of the piece read ahead, waiting for it if
// it is still being read. False when the piece doesn't hold the whole range.
inline bool TakeAhead(std::unique_lock<std::mutex>& lock, const std::string& path, uint64_t offset,
                      uint32_t length, std::vector<uint8_t>& out) {
    Shared& shared = State();
    Ahead& ahead = shared.ahead;
    const auto covers = [&] {
        return ahead.state != Ahead::State::Empty && ahead.path == path && offset >= ahead.offset &&
               offset + length <= ahead.offset + ahead.length;
    };
    if (!covers()) {
        return false;
    }
    shared.changed.wait(lock, [&] {
        return ahead.state == Ahead::State::Ready || ahead.state == Ahead::State::Empty || !covers();
    });
    if (ahead.state != Ahead::State::Ready || !covers()) {
        return false;
    }
    const auto begin = ahead.data.begin() + static_cast<std::ptrdiff_t>(offset - ahead.offset);
    out.assign(begin, begin + length);
    return true;
}
// Ends the read-ahead thread (waiting at most a second), for a return to hbmenu: a thread left
// waiting on `changed` would still be a waiter at that address when the game is loaded again in
// the same process, and could take the next launch's wake-up and lock.
inline void StopWorker() {
    Shared& shared = State();
    std::unique_lock<std::mutex> lock(shared.lock);
    if (!shared.workerRunning) {
        return;
    }
    shared.workerStop = true;
    shared.changed.notify_all();
    shared.changed.wait_for(lock, std::chrono::seconds(1), [&] { return !shared.workerRunning; });
}

} // namespace OpenGameFiles
#endif

inline bool ReadExact(const std::filesystem::path& hostPath,
                      uint64_t offset,
                      uint32_t length,
                      std::vector<uint8_t>& destination,
                      HostReadFailure& failure) {
    failure = HostReadFailure::None;
#if defined(MKW_KAIHEN)
    {
        OpenGameFiles::Shared& shared = OpenGameFiles::State();
        std::unique_lock<std::mutex> lock(shared.lock);
        OpenGameFiles::File* file = OpenGameFiles::Get(hostPath);
        if (!file) {
            failure = HostReadFailure::MissingFile;
            return false;
        }
        if (offset >= file->size) {
            failure = HostReadFailure::BadOffset;
            return false;
        }
        if (static_cast<uint64_t>(length) > file->size - offset) {
            failure = HostReadFailure::ShortRead;
            return false;
        }
        const std::string path = file->path;
        const bool straight = offset == file->lastEnd;
        std::vector<uint8_t> staged;
        if (!OpenGameFiles::TakeAhead(lock, path, offset, length, staged)) {
            // The lock was let go while waiting: the open file may have been closed meanwhile.
            file = OpenGameFiles::Get(hostPath);
            if (!file) {
                failure = HostReadFailure::MissingFile;
                return false;
            }
            staged.resize(length);
            if (fseeko(file->file, static_cast<off_t>(offset), SEEK_SET) != 0) {
                failure = HostReadFailure::BadOffset;
                return false;
            }
            if (length != 0 && std::fread(staged.data(), 1, length, file->file) != length) {
                std::clearerr(file->file);
                failure = HostReadFailure::ShortRead;
                return false;
            }
        } else {
            file = OpenGameFiles::Get(hostPath);
            if (!file) {
                failure = HostReadFailure::MissingFile;
                return false;
            }
        }
        file->lastEnd = offset + length;
        if (straight && length != 0) {
            OpenGameFiles::ReadAhead(*file, length);
        }
        destination = std::move(staged);
        return true;
    }
#endif

    std::ifstream file(hostPath, std::ios::binary);
    if (!file.is_open()) {
        failure = HostReadFailure::MissingFile;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0 ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        offset >= static_cast<uint64_t>(fileSize)) {
        failure = HostReadFailure::BadOffset;
        return false;
    }

    const uint64_t remaining = static_cast<uint64_t>(fileSize) - offset;
    if (static_cast<uint64_t>(length) > remaining) {
        failure = HostReadFailure::ShortRead;
        return false;
    }

    std::vector<uint8_t> staged(length);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        failure = HostReadFailure::BadOffset;
        return false;
    }

    if (length != 0) {
        file.read(reinterpret_cast<char*>(staged.data()),
                  static_cast<std::streamsize>(length));
        if (file.gcount() != static_cast<std::streamsize>(length)) {
            failure = HostReadFailure::ShortRead;
            return false;
        }
    }

    destination = std::move(staged);
    return true;
}

} // namespace DvdReadContract
