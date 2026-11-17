#pragma once

// The games' start check, between starting graphics and booting the game (runtime/src/main.cpp):
// the disc is on the card and, in the Retro Rewind build, the Retro Rewind release it is made for.
// The KaiKart launcher sets these up; when one is missing, a screen says to open KaiKart and waits
// for + and - to return to hbmenu. Returns at once when everything is in place.
namespace kaihen_setup {
void Run();
}
