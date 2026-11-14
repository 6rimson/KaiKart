// kaiHEN: no Steam on the console. Stands in for runtime/src/steam_input_layout.cpp.
#include "steam_input_layout.h"

namespace SteamInputLayout {
void Initialize(bool, uint32_t) {}
void HandleEvents(const AuroraEvent*) {}
void Release() noexcept {}
}  // namespace SteamInputLayout
