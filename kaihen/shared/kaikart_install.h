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

}  // namespace kaikart
