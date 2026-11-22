# Changelog

What changes for a player in each release, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

## 0.1.2 - 2026-11-22

- Fixed: in 0.1.1 a controller was not read at all unless "Four-player
  controllers" was on in the launcher's Settings, Controls, and it did not
  rumble. Controllers work again with that setting off, as in 0.1.0.
- Fixed: the Mods page's list no longer shows a scroll bar that comes and goes
  by itself.
- Fixed: on the Updates page the version and Check now no longer jump into
  place after the page opens.
- A mod made only of pictures no longer says "0 files" on the Mods page.
- The game now opens in a window by default; Fullscreen is in Settings,
  Display. If you chose Fullscreen yourself before, it stays.
- The launcher's Mods page marks each picture the game drew at its last start,
  and counts them on the mod's plate.
- Keyboard keys go by their place on the keyboard, not the letter printed on
  them, so the default keys sit in the same places on French, German and other
  layouts. The Keyboard page shows your own keyboard's labels. Keys you changed
  before stay on the same keys.
- The Keyboard page tells left and right Shift and Ctrl apart, and Esc can now
  be given to a button (click elsewhere to cancel a change).
- While the game is in front, Windows' Sticky Keys shortcut (Shift pressed five
  times) is switched off, so a turbo on Shift cannot open its box; it comes back
  when you leave the game. Sticky Keys itself, if you use it, is left alone.

## 0.1.1 - 2026-11-21

- New default keyboard layout, made for playing: W A S D move; K pass, L
  shoot, J trick move or steal, I block or rebound (the controller's own
  diamond); Left Shift is one turbo, Space two, and the semicolon key all
  three; U strategy; Q and E pocket and cancel the Gamebreaker; Enter pauses;
  Esc or Backspace goes back. Keys you changed before are kept. The launcher's
  Keyboard page shows the turbo keys as "1 turbo", "2 turbos" and "3 turbos".
- Updates finish sooner: after an update the launcher recompiles only the files
  that changed, not the whole game. The first update after 0.1.0 still
  recompiles everything once.
- The game program's Properties show its release version.
- Mods: files in a `mods` folder beside the saves are read in place of the
  disc's, one folder per mod, in the order `mods\load_order.txt` lists them.
  The save is copied to the backups first whenever the mods in use change.
- The launcher has a Mods page: install a mod from its .zip (a file picker, or
  drop the zip on the window), switch mods on and off, set their order, see
  which files each one replaces and which mods clash, remove a mod, and turn
  all mods off for the next start. Zips that would put files outside the mod's
  folder or that contain programs are refused.
- Matches need less processor time: the graphics work done on the processor
  works out each corner of a 3D model once per draw instead of every time the
  draw uses it. The picture is the same. Slower processors hold 60 frames a
  second more often.
- On processors with performance and efficiency cores (recent Intel laptops and
  desktops), the game keeps its busiest work on the performance cores and asks
  Windows not to slow it down to save power. Other processors are unchanged.
- On a PC whose only graphics are built into the processor, the launcher starts
  with Resolution 1x, Anti-aliasing 2x and Texture filtering 4x. Values you
  chose yourself are kept, and PCs with a graphics card keep Fit, 4x and 16x.
- Mods can replace a single piece of one of the game's archives (for example
  one texture of a court) without carrying the whole archive:
  `mods\<mod>\xf\forest.viv\foresto.xsh`, or the piece's number for archives
  without names (`mods\<mod>\xf\xf000002\417.xsh`).
  Package files such as `manifest.json`, `icon.png` and `README.md` are
  skipped, and `logs\game.log` says why any file of a mod was not used.
- Mods can change one menu picture or one line of on-screen text without
  carrying the whole pack it lives in: `mods\<mod>\fe\xafescrn.bin\0.shpx`
  for a picture, `mods\<mod>\locale\xloc_fe.eus\mainmenu.txt\1.txt` holding
  the new text for a line. Two mods can change different lines of the same
  pack.
- Mods can replace any picture the game draws without touching its files: a
  PNG or DDS in the mod's `textures` folder, named by the picture's
  fingerprint, is drawn in its place, at any size (four times the original
  looks sharp at higher resolutions). To find the one to replace, press
  Ctrl + F9 in the game: every picture the game draws from then on is
  written once to a `texture_dump` folder under its fingerprint, until
  Ctrl + F9 again. The launcher's Settings, Advanced, "Texture dump" starts
  the game with it on.
- The Mods page lists single pieces, menu pictures, lines of text and pictures
  named by fingerprint among a mod's files, installs a mod of pictures only,
  and shows a clash when two mods change the same piece, line or picture,
  whether each names it by name or by number. A piece the archive does not
  have, or one the game left out at its last start, is dimmed with the reason.
- The game no longer keeps a processor core fully busy while it waits for the
  picture to be drawn: in a match it uses about half the processor time it
  did, at 60 and at 120 frames a second, which helps laptops stay cool. The
  frame rate is unchanged.
- With the crackle fix on, the sound recovers at once if Windows holds back the
  game's sound service, instead of falling to a tenth of its pace until the
  hold ends.
- Launcher: the Check now button on the Updates page, and the update notice
  beside the ring, no longer flicker when the pointer rests on them.
- Launcher: on a laptop with two graphics chips, the "This PC" list names the
  chip the game draws on (the dedicated one, unless Windows has stopped it),
  not the built-in one.
- Launcher: Restore backup also lists the copies of the save made before a
  change of mods, marked "before mods changed".
- Launcher: the menu keeps the current page selected while a confirmation
  (such as removing a mod) is open.
- Launcher: when an install, download or build finishes, the ring runs back to
  empty and its colour fades back, instead of switching at once.

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
