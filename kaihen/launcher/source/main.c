// The KaiKart launcher: the one app in hbmenu. It shows the games, and starts the chosen one's
// build in wiicompiled/apps/ through kaiHEN's loader (ounce-hbloader), which loads it once the
// launcher exits, the way hbmenu starts apps. Launcher, then game, is a single game launch.
//
// This first version proves the hand-off: a text menu on libnx's console. Setup and updates move
// in next (kaikart plan, kaihen-port/V0.4.0-PLAN.md).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#ifndef KAIKART_VERSION
#define KAIKART_VERSION "dev"
#endif

typedef struct {
    const char *title;
    const char *path;
} Game;

static const Game kGames[] = {
    {"Retro Rewind", "sdmc:/wiicompiled/apps/RetroRewind.nro"},
    {"Original Mario Kart Wii", "sdmc:/wiicompiled/apps/WiiCompiled.nro"},
};
enum { kGameCount = sizeof kGames / sizeof kGames[0], kRows = kGameCount + 1 };

static void draw(int selected, const char *message) {
    printf("\x1b[2J\x1b[1;1H");
    printf("\n  KaiKart %s\n\n", KAIKART_VERSION);
    for (int i = 0; i < kRows; i++) {
        const char *title = i < kGameCount ? kGames[i].title : "Return to hbmenu";
        printf("  %s %s\n", i == selected ? ">" : " ", title);
    }
    printf("\n  %s\n", message);
    printf("\n\n  Up/Down: move   A: choose\n");
    consoleUpdate(NULL);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    int selected = 0;
    const char *message = envHasNextLoad() ? "" : "kaiHEN's loader can't start another app from here.";
    draw(selected, message);
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down == 0) {
            svcSleepThread(16 * 1000 * 1000);
            continue;
        }
        if (down & HidNpadButton_AnyUp) selected = (selected + kRows - 1) % kRows;
        if (down & HidNpadButton_AnyDown) selected = (selected + 1) % kRows;
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_A) {
            if (selected == kGameCount) break;
            struct stat st;
            if (stat(kGames[selected].path, &st) != 0) {
                message = "That game isn't on the card yet (wiicompiled/apps).";
            } else if (R_FAILED(envSetNextLoad(kGames[selected].path, kGames[selected].path))) {
                message = "kaiHEN's loader didn't accept the game.";
            } else {
                printf("\n  Starting %s...\n", kGames[selected].title);
                consoleUpdate(NULL);
                break;
            }
        }
        draw(selected, message);
    }
    consoleExit(NULL);
    return 0;
}
