#!/usr/bin/env python3
"""Put the kaiHEN build and the game's files on a microSD card (kaihen/run.sh sd).

  kaihen/run.sh sd /Volumes/CARD [--nand DIR] [--rr-save FILE] [--disc DIR] [--rr DIR] [--replace-rr]

The card gets switch/<Product>.nro (hbmenu lists it) and wiicompiled/: your extracted disc, the
Retro Rewind folder, the runtime's files, settings (settings.env, UserData/Config.toml) and saves
(UserData/NAND). Files already on the card are kept, so running it again only adds what is missing
(and replaces the .nro). --replace-rr swaps the card's RetroRewind6 folder for yours, after a
Retro Rewind update: files on the card the new version doesn't have, such as saved ghosts, are
kept, and your Retro Rewind save is outside the folder. KAIHEN_PRODUCT=WiiCompiled puts plain Mario Kart Wii there; both share the
wiicompiled folder.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
ASSETS = REPO / "runtime/assets"

# The card's own small files (portable.txt, settings.env, UserData/Config.toml), copied when missing.
CARD = HERE / "card/wiicompiled"


def copy_tree(source, destination):
    """Copy a folder once; a folder already there is kept, never overwritten. The copy goes to
    <name>.partial first and gets its name only when complete, so an interrupted copy (a card
    pulled out) is simply done again next time. On the Mac's own disk it is a clone (no space
    used); a microSD card (exFAT) gets a plain copy."""
    if destination.exists():
        return
    partial = destination.with_name(destination.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f"  {destination}", flush=True)
    cloned = sys.platform == "darwin" and subprocess.run(
        ["cp", "-Rc", str(source), str(partial)], stderr=subprocess.DEVNULL).returncode == 0
    if not cloned:
        shutil.rmtree(partial, ignore_errors=True)
        shutil.copytree(source, partial)
    partial.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("card", type=Path, help="the microSD card, e.g. /Volumes/SWITCH")
    parser.add_argument("--disc", type=Path, default=REPO / "Assets/DATA", help="your extracted RMCP01 disc")
    parser.add_argument("--rr", type=Path, default=REPO / "PulsarPacks/completed/RetroRewind/RetroRewind6",
                        help="your RetroRewind6 folder")
    parser.add_argument("--nand", type=Path, help="a NAND folder to start from (copied once)")
    parser.add_argument("--rr-save", type=Path, help="a Retro Rewind rksys.dat to start from (copied once)")
    parser.add_argument("--replace-rr", action="store_true",
                        help="replace the card's RetroRewind6 folder with yours (after a Retro Rewind update)")
    args = parser.parse_args()

    product = os.environ.get("KAIHEN_PRODUCT", "RetroRewind")
    if product not in ("RetroRewind", "WiiCompiled"):
        raise SystemExit("KAIHEN_PRODUCT is RetroRewind or WiiCompiled")
    retro_rewind = product == "RetroRewind"
    nro = HERE / "dist" / f"{product}.nro"
    if not nro.exists():
        raise SystemExit(f"No {nro}: run kaihen/run.sh setup (or build) first")
    card = args.card.resolve()
    if not card.is_dir():
        raise SystemExit(f"{card} is not a mounted card")
    if sys.platform == "darwin" and (card.parent != Path("/Volumes") or card == Path("/Volumes/Macintosh HD")):
        raise SystemExit(f"{card} is not a card: give the card itself, like /Volumes/SWITCH (ls /Volumes)")
    if not (args.disc / "sys/fst.bin").exists():
        raise SystemExit(f"{args.disc} is not an extracted disc (needs files/ and sys/fst.bin)")
    if retro_rewind and not (args.rr / "Binaries/Code.pul").exists():
        raise SystemExit(f"{args.rr} is not a RetroRewind6 folder")
    if args.rr_save and not retro_rewind:
        raise SystemExit("--rr-save is a Retro Rewind save; plain Mario Kart Wii keeps its save in the NAND")

    root = card / "wiicompiled"
    root.mkdir(exist_ok=True)
    print("Copying the game's files (the first time takes a few minutes):", flush=True)
    # The card's small files (portable mode keeps settings, NAND, caches and logs in UserData), and
    # the runtime's own files.
    for template in CARD.rglob("*"):
        target = root / template.relative_to(CARD)
        if template.is_file() and not target.exists() and not (
                target.name == "settings.env" and (root / "hbxl-env").exists()):
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(template, target)
    for source, name in ((ASSETS / "dsp/dsp_coef.bin", "dsp_coef.bin"), (ASSETS / "certs/cacert.pem", "cacert.pem")):
        shutil.copyfile(source, root / name)
    copy_tree(ASSETS / "wii", root / "wii_bootstrap")
    copy_tree(args.disc, root / "disc/DATA")
    if retro_rewind:
        card_rr = root / "RetroRewind/RetroRewind6"
        if args.replace_rr and card_rr.exists():
            # The new folder is copied in full first, with every file of the old one it doesn't
            # have (such as ghosts you saved); only then is the old one removed.
            new = card_rr.with_name("RetroRewind6.new")
            shutil.rmtree(new, ignore_errors=True)
            copy_tree(args.rr, new)
            for kept in card_rr.rglob("*"):
                target = new / kept.relative_to(card_rr)
                if kept.is_file() and not target.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(kept, target)
            old = card_rr.with_name("RetroRewind6.old")
            shutil.rmtree(old, ignore_errors=True)
            card_rr.rename(old)
            new.rename(card_rr)
            shutil.rmtree(old)
        copy_tree(args.rr, card_rr)
    if args.nand:
        copy_tree(args.nand, root / "UserData/NAND")
    if args.rr_save:
        save = root / "RetroRewind/riivolution/save/RetroWFC/RMCP"
        save.mkdir(parents=True, exist_ok=True)
        if not (save / "rksys.dat").exists():
            shutil.copyfile(args.rr_save, save / "rksys.dat")
    # A card set up for plain Mario Kart Wii by an older build: add Retro Rewind's folder.
    config = root / "UserData/Config.toml"
    if retro_rewind and config.exists() and "retro_rewind_root" not in config.read_text():
        rr_root = 'retro_rewind_root = "/RetroRewind/RetroRewind6"\n'
        text = config.read_text()
        text = text.replace("[paths]\n", "[paths]\n" + rr_root, 1) if "[paths]\n" in text else \
            text.rstrip("\n") + "\n\n[paths]\n" + rr_root
        config.write_text(text)

    # The app last, through a temporary name: a copy that fails (a full card) leaves no broken
    # .nro for hbmenu to list, and the previous one stays.
    switch = card / "switch"
    switch.mkdir(exist_ok=True)
    partial = switch / (nro.name + ".partial")
    try:
        shutil.copyfile(nro, partial)
        partial.replace(switch / nro.name)
    except OSError as error:
        partial.unlink(missing_ok=True)
        raise SystemExit(f"Couldn't copy {nro.name} to the card: {error.strerror}")
    print(f"Copied {nro.name} to {switch}. Eject the card, then start it from hbmenu.")


if __name__ == "__main__":
    main()
