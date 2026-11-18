# Gamebreaker

A native Windows version of *NBA Street Vol. 2* (original Xbox, 2003), made by
recompiling the original Xbox program. The game's own machine code is
translated into C and compiled for the PC on your machine, from your own disc.
It plays the original game: its code, its data, its timing.

As far as we found, this is the first recompilation of this game's original
code, and the first PC project built from the Xbox version.

This repository contains no part of the game. You need your own copy.

## Status

Early. Played by hand on two PCs: a whole match at 60 frames a second with a
controller on a desktop, with rumble, sound, pausing, saving, loading and
screenshots all checked, and the keyboard on a laptop that had no developer
tools installed, where the frame rate sometimes fell below 60.

Checked by scripted runs on the desktop: every game mode (Pick Up Game, NBA
Challenge, Be A Legend, Street School), Create A Baller and Create Team, all
eleven courts, whole matches through the post-match screens, replays and the
pause menu; a two-against-two match driven by four test controllers; saving,
quitting and loading; the cheats and Unlock everything; widescreen and
ultrawide; 120 frames a second; pausing when the window loses focus; the volume
settings, measured on the game's output; setup from a release zip on a PC path
without developer tools.

Not yet tested: four real controllers at once, a whole Be A Legend or Street
School run, Windows 10, and AMD graphics. Known problems are listed in
[docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).

## What you need

- Windows 10 (version 1809 or later) or Windows 11, 64-bit.
- About 3.5 GB of free disk space, and an internet connection for the first
  start and setup (about 270 MB: the launcher's window component and the build
  tools).
- A graphics card with Direct3D 11.
- On Windows 10, Microsoft's Edge WebView2 Runtime, if it is not already there.
  Windows 11 includes it.
- Your own copy of the USA Xbox release, dumped from your own disc as a disc
  image (`.iso`) or an extracted folder. Setup checks its `default.xbe`:

  ```
  SHA-256  08389AE31309FACD89EC7A1057CEC32C4308D734D0B8AD9151437AA511CBDCFE
  ```

  Other releases are not supported. Please do not ask where to get disc
  images.

## Install

1. Download the latest release zip from the Releases page and extract it.
   The folder holds `Gamebreaker.exe` and an `app` folder; keep them together.
2. Start `Gamebreaker.exe`.
3. Choose an install folder, then your disc image. Setup checks the disc
   before it downloads anything.
4. Press Download for the build tools, then wait for the build: 3 to 10
   minutes, using every processor core.
5. Play.

Nothing is installed into Windows: no registry entries, no shortcuts, no
administrator rights. To remove it, delete the install folder (by default
`%LOCALAPPDATA%\Gamebreaker`) and the extracted zip. Saves are in the install
folder's `saves` folder.

The programs are not signed. On first start Windows may show "Windows
protected your PC" (choose More info, then Run anyway) or "Open File - Security
Warning" (choose Run).

If `Gamebreaker.exe` will not start, `app\Gamebreaker.cmd` opens the same
launcher.

## Settings

In the launcher, before starting the game.

| Setting | Values | Default |
|---|---|---|
| Screen | Window, Fullscreen (borderless) | Fullscreen |
| Display | Primary display, or any display | Primary display |
| Resolution | 1x to 4x, Fit | Fit |
| Shape | 4:3, 16:10, 16:9, 21:9, 32:9 | The display's shape |
| Menus and HUD | All wide, Menus wide, 4:3 | All wide |
| Texture filtering | Off, Trilinear, 4x, 8x, 16x | 16x |
| Anti-aliasing | Off, 2x, 4x, 8x | 4x |
| Vertical sync | On, Off | On |
| Frame rate | 60, 120 (experimental) | 60 |
| Frame counter | On, Off | Off |
| Skip intro film | On, Off | On |
| Pause when in background | On, Off | On |
| Change song each game | On, Off | On |
| Demo when idle | Off, 30 s, 90 s, 3 min | 90 s |
| Volume, Music, Effects, Crowd, Player voices, Commentary volume | 0 to 100 % | 100 % |
| Commentary | On, Off | On |
| Mono | On, Off | Off |
| Sound in background | On, Off | On |
| Crackle fix | On, Off | On |
| Rumble | On, Off | On |
| Four-player controllers | On, Off (experimental) | Off |
| Cheats | On, Off, then each of the game's twelve cheats | Off |
| Graphics chip | Windows' choice, Fastest | Windows' choice |
| Check for updates daily | On, Off | On |

The volumes are a share of the game's own level and add to the sliders in
the game's Options screen. With four-player controllers on, the Controls
page also sets the button layout, trigger threshold, stick dead zone, each
player's rumble strength, player order and which player the keyboard drives.

In widescreen the game draws more of the court at the sides. "All wide"
stretches menus, films and the score display to the screen's width. "Menus
wide" keeps the score display in shape, and "4:3" keeps all of them in shape
with black bars at the sides.

At 120 frames a second the game runs at its normal speed, but busy match views
may show fewer pictures than that.

## Cheats

The launcher's Cheats page offers the twelve cheats the game has built in, so
no code has to be entered. They apply from the next game set up, and the game
refuses them in Street School, as on the console. Unlock everything unlocks
every reward in your save, after keeping a copy of the save in the `backups`
folder.

## Controls

An Xbox-type controller is read directly, with the console's layout. On the
keyboard:

| Key | Button |
|---|---|
| Enter | START |
| Backspace | BACK |
| Z, X, C, V | A, B, X, Y |
| Arrow keys | D-pad |
| W, A, S, D | Left stick |
| Q, E | White, Black |
| 1, 3 | Left and right trigger |

These are the default keys. The launcher's Keyboard page changes any of them,
for the player the keyboard drives, and resets them to these.

## Saves and screenshots

The game saves as on the console (Options, Game Data). After the first save,
Autosave keeps it up to date. The last five versions of the save are kept as
backups; the launcher's Advanced page opens the save folder and restores a
backup.

F12 saves the picture on screen as a PNG in the install folder's
`screenshots` folder; the launcher's Advanced page opens it.

## How it works

[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) translates the game's x86
program into C ahead of time. That C is compiled into a Windows program that
links the toolkit's runtime in place of the Xbox's kernel, graphics and sound
hardware. This project adds the game-specific fixes, a Direct3D 11 renderer,
controller support, saves, and the launcher that does all of this on your PC.

The translated code is derived from the game, so it is never published: setup
generates it from your own disc and compiles it locally.

What changed in each release: [CHANGELOG.md](CHANGELOG.md). Building from source:
[docs/BUILDING.md](docs/BUILDING.md). Contributing:
[CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet and
  contributors (MIT): the recompiler and runtime this port is built on.
- [xemu](https://github.com/xemu-project/xemu) (LGPL-2.1-or-later): parts of
  the toolkit's sound and graphics chip models come from it.

Details and licence texts: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Legal

Gamebreaker is an unofficial fan project. It is not affiliated with or
endorsed by Electronic Arts, EA Canada, the NBA, its teams or its players, or
Microsoft. *NBA Street* and all related names and marks belong to their
owners. This repository and its releases contain no game code, artwork, audio
or data. Rights holders with a concern can open an issue.

## Licence

MIT, for this repository's own code: see [LICENSE](LICENSE). It grants no
rights in the game.

---

Built with AI assistance (Claude, by Anthropic) under human direction.
