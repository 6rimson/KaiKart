// The games' start check (kaihen_setup.h). The KaiKart launcher (kaihen/launcher) imports the disc
// and installs Retro Rewind before it starts a game; a game started some other way, or a card
// changed by hand, may still lack them. Instead of failing deep in the game, it says so here and
// waits for + and - to return to hbmenu.
#include "kaihen_setup.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <cryptopp/filters.h>
#include <cryptopp/files.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha.h>
#include <imgui.h>

// The Retro Rewind release this build's translation was made from (kaihen/build.sh writes it).
#if __has_include("kaihen_build_info.h")
#include "kaihen_build_info.h"
#endif
#ifndef KAIHEN_RR_VERSION
#define KAIHEN_RR_VERSION ""
#define KAIHEN_RR_CODE_SHA256 ""
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern "C" int kaihen_return_requested(void);
namespace settings_overlay {
[[noreturn]] void ConsoleReturnToLauncher() noexcept;  // settings_overlay.cpp
}

namespace kaihen_setup {
namespace {

namespace fs = std::filesystem;

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

bool DiscReady(const fs::path& data) {
    std::error_code error;
    return fs::is_regular_file(data / "sys" / "fst.bin", error) &&
           fs::is_regular_file(data / "sys" / "main.dol", error);
}

bool RetroRewindReady(const fs::path& folder) {
    if (KAIHEN_RR_CODE_SHA256[0] == '\0') {
        std::error_code error;
        return fs::is_regular_file(folder / "Binaries" / "Code.pul", error);  // a build without a pin
    }
    return Sha256(folder / "Binaries" / "Code.pul") == KAIHEN_RR_CODE_SHA256;
}

void CenteredText(const std::string& text, float y, float wrap) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).x, wrap);
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - width) * 0.5f), y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

void DrawMessage(const std::string& title, const std::vector<std::string>& lines) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("KaiKart check", nullptr, kFlags)) {
        const float wrap = viewport->Size.x * 0.8f;
        float y = viewport->Size.y * 0.3f;
        ImGui::SetWindowFontScale(1.25f);
        CenteredText(title, y, wrap);
        y += ImGui::GetTextLineHeightWithSpacing() * 2;
        ImGui::SetWindowFontScale(1.0f);
        for (const auto& line : lines) {
            CenteredText(line, y, wrap);
            y = ImGui::GetCursorPosY() + 12;
        }
        CenteredText("Hold + and - to quit.", viewport->Size.y * 0.85f, wrap);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

}  // namespace

void Run() {
    const std::string title = std::string(RuntimeProduct::Active().displayName) + " isn't ready yet";
    std::vector<std::string> missing;
    const fs::path data = RuntimeConfigFile::ResolvedDvdRoot();
    const fs::path retro_rewind =
        RuntimeProduct::IsRetroRewind() ? fs::path(RuntimeConfigFile::RetroRewindRoot()) : fs::path{};
    if (!data.empty() && !DiscReady(data)) {
        missing.push_back("Your disc isn't set up on the card.");
    }
    if (!retro_rewind.empty() && !RetroRewindReady(retro_rewind)) {
        missing.push_back(std::string("This build plays Retro Rewind ") + KAIHEN_RR_VERSION +
                          ", which isn't on the card.");
    }
    if (missing.empty()) {
        return;
    }
    for (const auto& line : missing) RT_LOGF("setup", "not ready: %s\n", line.c_str());
    missing.push_back("Open KaiKart from hbmenu and choose the game there: it sets these up first.");
    for (;;) {
        if (kaihen_return_requested()) {
            settings_overlay::ConsoleReturnToLauncher();
        }
        aurora_update();
        if (aurora_begin_frame()) {
            DrawMessage(title, missing);
            aurora_end_frame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}

}  // namespace kaihen_setup
