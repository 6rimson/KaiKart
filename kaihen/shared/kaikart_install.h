#pragma once

// KaiKart's install steps, run by the launcher before a game starts (kaihen/launcher): the disc
// imported from an image on the card, and the Retro Rewind release a game build is made for. Each
// runs on a worker thread and reports through a Progress the screen reads.
#include <filesystem>
#include <mutex>
#include <string>

namespace kaikart {

namespace fs = std::filesystem;

struct Progress {
    std::mutex mutex;
    std::string step;      // "Importing your disc"
    std::string detail;    // a file name, or what went wrong
    std::string advice;    // on a problem: what the player should do
    float fraction = -1;   // 0..1, or negative for none
    bool failed = false;
};

// Where log lines go (each ends with a newline); none by default.
void SetLog(void (*log)(const char* line));

// The extracted disc (`data`, e.g. wiicompiled/disc/DATA) is in place.
bool DiscReady(const fs::path& data);
// Imports the disc image found in `root` (.iso, or .wbfs with its split parts) into `data`, checked
// against a clean PAL disc, revision 0. False with `progress.failed` set when it can't.
bool ImportDisc(const fs::path& root, const fs::path& data, Progress& progress);

// `folder` (RetroRewind6) holds the Code.pul with this SHA-256 (uppercase hex); an empty one
// accepts any Code.pul.
bool RetroRewindReady(const fs::path& folder, const std::string& code_sha256);
// Installs Retro Rewind `version` into `folder` from Retro Rewind's server, checked against
// `code_sha256`; resumes an interrupted download and keeps files only the old folder has.
bool InstallRetroRewind(const fs::path& folder, const std::string& version, const std::string& code_sha256,
                        Progress& progress);

// KaiKart's own releases (GitHub, owner/name): each holds KaiKart-<version>.nro,
// <game>-<version>.nro for each game, and SHA256SUMS.
struct Release {
    std::string version;
    std::string download_base;  // https://github.com/<repo>/releases/download/<tag>
};
bool VersionNewer(const std::string& a, const std::string& b);
// The Retro Rewind version in a folder (its version.txt), or empty.
std::string RetroRewindVersionOnCard(const fs::path& folder);
// Retro Rewind releases newer than the folder's, from its server: `installable` is the newest that
// only changes files this app can use as they are (its Code.pul stays the same); `needs_kaikart` the
// first that changes Code.pul, which only a KaiKart release can play. Both empty when it's current.
struct RetroRewindNews {
    std::string installable, needs_kaikart, newest;
};
bool RetroRewindUpdates(const fs::path& folder, RetroRewindNews& news, std::string& error);
// Installs `version` (one RetroRewindUpdates found installable) with Retro Rewind's update files;
// `code_sha256` is the Code.pul it must still have.
bool InstallRetroRewindUpdate(const fs::path& folder, const std::string& version, const std::string& code_sha256,
                              Progress& progress);
// The display version in an .nro's NACP (what hbmenu shows), or empty.
std::string NroVersion(const fs::path& nro);
// The newest release of `repo` with a `name`-<version>.nro newer than `current`; found.version stays
// empty when there's none. False (with `error`) when GitHub can't be asked.
bool NewerRelease(const std::string& repo, const std::string& name, const std::string& current, Release& found,
                  std::string& error);
// Downloads `url` to `path` (through path.partial), checked against the SHA-256 that `sums_url`
// (a SHA256SUMS file) lists for its file name; the old file is replaced only when it checks out.
bool DownloadChecked(const std::string& url, const std::string& sums_url, const fs::path& path, Progress& progress);

}  // namespace kaikart
