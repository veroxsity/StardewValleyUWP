# Changelog

## 0.1.19.0, 2026-11-14

First public release.

- Stardew Valley 1.6.15 runs on an Xbox in Developer Mode, on FNA with the patches in `patches`. Controller, sound and saves work.
- No game files in the package. The launcher signs in to Steam with a QR code and downloads the game from your account into LocalState. A game folder copied into `LocalState\game` works too.
- SMAPI 4.5.2 is included. Mods load from `LocalState\Mods`.
- Xbox Paths ships with the app. Without it the game can't find its saves or load mod images on the console.
- Mods screen for searching CurseForge, browsing the popular list and installing with A. Mods whose authors block app installs are marked.
- The CurseForge key is pasted from a phone through a page on the console's local address. The page only runs while that screen is open. Uploading `curseforge.json` with the Device Portal also works.
- Installed mods list with delete. Mods the game needs on the console get a warning first.
- A blurred farm picture sits behind the launcher menu, and screenshots you take in game take turns with it.
- `stardew-host.log` and `stardew-stdio.log` in LocalState for when something goes wrong.

Nightly builds of the latest code are on the Releases page, built straight from this repo with no game files involved.

Only tested on a Series S. Leaving the app and coming back hasn't been tested, so save first. The README has the full list of what does and doesn't work.
