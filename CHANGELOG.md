# Changelog

What changes for a player in each release, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

## 0.1.0 - 2026-11-18

The first public release.

### Game

- The original Xbox game (USA release) as a native Windows program, built on
  your PC from your own disc.
- Pick Up Game, NBA Challenge, Be A Legend and Street School, with Create A
  Baller, Create Team, Rewards and all eleven courts.
- 60 frames a second, at the game's own speed.
- Skip intro film, on by default. The publisher's logo and the legal notice
  still play.
- A match pauses when another window comes to the front, on by default.
- Options for changing the song each game and for how long the title screen
  waits before the demo plays.
- The project's start screen, and its mark in the corner of the title, menu and
  loading screens.
- A message with the reason when the game stops on an error.

### Display

- Borderless fullscreen by default, or a resizable window, on any display.
  Alt+Enter or F11 switches between them.
- Internal resolution from 1x to 4x, or Fit to the display (the default).
- Screen shapes 4:3, 16:10, 16:9, 21:9 and 32:9. The wide shapes show more of the
  court at the sides. The default follows the display.
- Menus and HUD: All wide (the default), Menus wide, or 4:3.
- Texture filtering up to 16x (default 16x) and anti-aliasing up to 8x (default
  4x).
- 120 frames a second, experimental. 60 is the default.
- Frame counter, off by default.
- F12 saves a screenshot.

### Controls

- Keyboard play. The Keyboard page in Settings sets the key for each game button,
  per player, and resets to the defaults.
- Xbox-type controllers, with rumble on by default.
- Four-player controllers, experimental and off by default, tested with simulated
  controllers only: button layout, trigger threshold, stick dead zone, each
  player's rumble strength and player order.

### Audio

- Volume settings for the whole game, music, effects, crowd, player voices and
  commentary.
- Commentary can be turned off. Mono output.
- Sound in background, on by default. Turn it off to silence the game while its
  window is not in front.
- Crackle fix, on by default. It delays music and speech by 20 ms.

### Saves

- Saves are kept in the install folder, with the last five versions as backups.
- A save in progress finishes before the game closes.
- The launcher opens the save folder and restores a backup.

### Cheats

- The game's twelve built-in cheats as switches, off by default. The game does not
  apply them in Street School.
- Unlock everything unlocks every reward, after a confirmation, and keeps a copy
  of your save first.

### Launcher and updates

- The first run checks your disc, downloads the build tools (about 260 MB), builds
  the game and starts it. Nothing is installed into Windows.
- The launcher closes while the game runs and opens again when it ends.
- Advanced settings move the install folder, check its files, rebuild the game,
  copy the logs for a bug report and open the screenshots folder.
- Checks for a new release once a day, which can be turned off, and downloads only
  the files that changed. A release whose file list is not signed by the project
  is refused.

### Known issues

See [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).
