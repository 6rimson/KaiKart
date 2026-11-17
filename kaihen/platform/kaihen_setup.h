#pragma once

// First-launch setup for the kaiHEN apps, between starting graphics and booting the game
// (runtime/src/main.cpp): the disc imported from an image on the card and, in the Retro Rewind app,
// the Retro Rewind pack. Returns at once when everything is in place. Shows a progress screen
// while it works; on a problem the player has to fix, it says what to do and waits for + and - to
// return to hbmenu.
namespace kaihen_setup {
void Run();

// The update screen, over the game (settings_overlay.cpp calls these each frame, on the game
// thread): B while the startup screen shows opens it; it checks GitHub for a newer release of this
// app and Retro Rewind's server for its latest version, and can download the new app.
bool UpdatesOpen();
void UpdatesFrame(bool startup_screen_visible);
}
