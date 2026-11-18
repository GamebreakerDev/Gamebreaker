# Building from source

Players do not need this: the release zip's launcher does all of it. This page
is for working on the port.

## What you need

- Windows 10 or 11, 64-bit, with Windows PowerShell 5.1 (built in).
- Git.
- Python 3.10 or later on the `py` launcher, with capstone 5:
  `py -3 -m pip install capstone`.
- Your own disc, extracted (below).
- About 4 GB of free space.

Visual Studio is not needed. `port\tools\fetch_toolchain.ps1` downloads the
compiler (llvm-mingw, a Clang for Windows), Ninja and CMake into `toolchain\`,
each checked against the SHA-256 in `setup\tools.lock`. Nothing is installed.
Visual Studio 2022 or later can build a second program for comparison
(`-Compiler msvc`).

## Layout

```
<repository>\
  port\          the port: sources, config, toolkit patch, build scripts
  setup\         the player's setup (the same pipeline, with its own folders)
  launcher\      the launcher window and the small starter program
  xboxrecomp\    the toolkit, cloned here (ignored by git)
  game_files\    your extracted disc (ignored by git)
  toolchain\     the downloaded compiler (ignored by git)
```

A release zip is arranged differently: its main folder holds only
`Gamebreaker.exe` and an `app` folder, and `app` holds everything else (`setup`,
`launcher`, `port` and the project's documents, including the licence and the
third-party notices).

## First build

1. Clone the toolkit at the pinned commit:

   ```bash
   git clone https://github.com/sp00nznet/xboxrecomp
   ```

   ```bash
   git -C xboxrecomp checkout 343b6d8
   ```

   The port carries one patch to the toolkit,
   `port\patches\0001-backend-batch-ext.patch`. `port\regen.ps1` applies it
   (`port\patches\apply.ps1`); applying it again does nothing.

2. Extract your disc into `game_files\`, from inside `xboxrecomp\`:

   ```bash
   py -3 -m tools.xiso unpack "<your image>.iso" -o ..\game_files
   ```

3. Fetch the compiler:

   ```bash
   powershell -ExecutionPolicy Bypass -File port\tools\fetch_toolchain.ps1
   ```

4. Lift and build:

   ```bash
   powershell -ExecutionPolicy Bypass -File port\regen.ps1
   ```

   This checks `game_files\default.xbe` against the pinned hash, runs the
   toolkit's parser, disassembler, function identification, ABI analysis and
   lifter, writes the C into `port\src\recomp\gen\`, and compiles
   `port\build-mingw\gamebreaker.exe`. About 4 minutes on a 16-core PC; the
   compile alone about 20 seconds.

5. Run it from the repository root, so it finds `game_files\`:

   ```bash
   port\build-mingw\gamebreaker.exe
   ```

After a change in `port\src`, `port\regen.ps1 -BuildOnly` compiles without
lifting again. After pulling a change that wraps a new game function (in
`port\src\recomp_manual.c` or `port\config\seed_functions.json`), run the full
`port\regen.ps1` once.

## The pinned executable

The port is built against one exact `default.xbe`: the USA retail release,
title ID `0x45410027`, built with XDK 5233.

```
SHA-256  08389AE31309FACD89EC7A1057CEC32C4308D734D0B8AD9151437AA511CBDCFE
```

Every address in `port\src`, `port\config\seed_functions.json` and the patch
is only right for this file. The scripts refuse any other.

## Switches

The program sets the runtime switches it needs by itself
(`port\config\run_env.txt`, compiled in). The player's settings are environment
variables the launcher sets; they work the same from a prompt:

| Variable | Values |
|---|---|
| `GB_D3D11_FULLSCREEN` | 1 for borderless fullscreen |
| `GB_D3D11_MONITOR` | display number or name |
| `GB_D3D11_RES` | 1 to 4, or `window` (fit) |
| `GB_D3D11_VSYNC` | `auto`, 0 |
| `GB_ASPECT` | `16:9`, `16:10`, `21:9`, `32:9` (unset: 4:3) |
| `GB_ASPECT_2D` | `stretch` (all wide, the launcher's default), `auto` (menus wide); unset: 4:3 |
| `GB_D3D11_ANISO` | 0, 1, 2, 4, 8, 16 |
| `GB_D3D11_MSAA` | 0, 2, 4, 8 |
| `GB_D3D11_ADAPTER` | `high` for the fastest graphics chip; unset: Windows' choice |
| `GB_SHOW_FPS` | 1 for the frame counter |
| `GB_GAME_HZ` | 60, 120 (experimental) |
| `GB_SKIP_INTRO` | 1 to end the intro film early |
| `GB_PAUSE_BACKGROUND` | 0 to keep a match running when the window is not in front (unset: it pauses) |
| `GB_RUMBLE` | 0 for no controller vibration |
| `GB_SONG_ROTATION` | 0 to keep one song instead of changing it each game |
| `GB_AUTO_REPLAY` | 0 for no automatic replays |
| `GB_DEMO_DELAY` | seconds before the demo plays on an idle title screen; 0 never (game: 90) |
| `GB_AUDIO_VOLUME` | 0 to 100, the whole game's sound |
| `GB_VOL_MUSIC`, `GB_VOL_SFX`, `GB_VOL_CROWD`, `GB_VOL_PLAYERS`, `GB_VOL_COMMENTARY` | 0 to 100 for each kind of sound |
| `GB_COMMENTARY` | 0 for no commentary |
| `GB_AUDIO_MONO` | 1 for the same sound in both ears |
| `GB_BG_SOUND` | 0 for silence while the game's window is not in front (default: the sound keeps playing) |
| `GB_AUDIO_TICK`, `GB_AUDIO_LEAD_MS`, `GB_AUDIO_WAIT`, `GB_AUDIO_GUARD` | the crackle fix, set together: 1, 40, 1, 1 (the launcher's default; unset when the game is started by hand) |
| `GB_CHEATS` | comma-separated: `street_kids`, `no_counters`, `gb_trails`, `all_quicks`, `unlimited_turbo`, `easy_2_pointers`, `hard_2_pointers`, `big_heads`, `wnba_ball`, `alternate_ball`, `no_hud`, `explosive_rims` |
| `GB_UNLOCK_ALL` | 1 to unlock every reward in the save, after copying it to `backups` |
| `GB_PADS` | 1 for the four-player controller layer (experimental) |
| `GB_PAD_PRESET`, `GB_PAD_TRIGGER`, `GB_PAD_DEADZONE`, `GB_PAD_RUMBLE`, `GB_PAD_ORDER`, `GB_PAD_KEYBOARD_PLAYER`, `GB_PAD_BACKGROUND` | with `GB_PADS=1`: button layout (`original`, `turbo`), trigger threshold in percent, stick dead zone in percent, rumble strength in percent, player order (`auto`, `slots`), the keyboard's player (0 to 4), pads read while the window is not in front (1) |
| `GB_PAD_CONFIG` | path of a `controls.ini` (the template is `port\config\controls.ini`); without `GB_PADS=1` only its keyboard keys count |
| `GB_SAVE_DIR`, `GB_LOG_DIR` | where saves and `game.log` go |
| `GB_SAVE_BACKUPS` | how many dated copies of the save to keep (default 5, 0 for none) |
| `GB_SCREENSHOT_DIR` | where F12 saves pictures (default: `screenshots` beside the save folder) |

The volumes are a percentage of the game's own level and stack with the
sliders in the game's Options screen. The game options (pause, rumble, songs,
replays, demo) override the game's own setting for the run; leave them unset
to keep the in-game controls in charge.

Alt+Enter or F11 switches between window and fullscreen while it runs. F12
saves a screenshot.

## How the port is put together

- The toolkit lifts every function of the game's program into C. The port
  supplies `main.c` (start-up, kernel and USB set-up), `recomp_manual.c`
  (game functions replaced or wrapped by hand), and its own subsystems:
  `gb_d3d11.c` (the Direct3D 11 renderer, behind the toolkit's back-end
  interface), `gb_overlay.c` (the start screen, corner mark, frame counter and
  screenshots), `gb_bgsound.c` (sound in the background), `gb_pad.c`
  (controllers), `gb_save.c` (save folder, backups,
  the write guard, the log file and the error box).
- `port\config\seed_functions.json` lists function addresses the
  disassembler cannot find by itself (reached only through pointers).
- Fixes belong in `port\`: wrap or replace the game function, or use one of
  the toolkit's switches. A change inside the toolkit is a patch in
  `port\patches\`, and every patch is one more thing to carry; it is the last
  resort.

## The log

`logs\game.log` (beside `game_files\`, or the launcher's `logs` folder) is
short: the version, the PC, the settings in effect, the graphics card,
errors, and how the game ended. Everything else the program and the toolkit
write goes to its error output, which is not kept. To see all of it, start
the program from a prompt with that output redirected:

```bash
port\build-mingw\gamebreaker.exe 2> run.txt
```

## The bring-up loop

When the game reaches code the lifter did not find, `logs\game.log` shows a
line starting `[ICALL] Failed to resolve` (the first twenty). To add those
functions:

1. From inside `xboxrecomp\`:

   ```bash
   py -3 -m tools.seed_from_log ..\logs\game.log ..\game_files\default.xbe --functions tools\disasm\output\functions.json --seeds ..\port\config\seed_functions.json
   ```

2. Look for the pointer table that holds the new address and add its other
   entries in the same pass; disassemble each candidate first, because some
   values in such tables are not code.
3. Run `port\regen.ps1` again.

A first run of something new crashing is normal: the loop is boot, find what
is missing, add it, run again.
