# Known issues

What is known to be wrong or untested. Reports of anything not listed here are
welcome as issues.

## Sound

- **Music and speech crackle if the crackle fix is turned off.** The game
  streams its mix only about 20 ms ahead of playback, and the emulated sound chip
  moves in bursts, so short pieces replay. "Crackle fix" in Settings, Audio is on
  by default and removes it; it delays music and speech by 20 ms.
- The game plays on Windows' default output device.

## Picture

- **On laptops the frame rate can fall below 60 when the machine is hot or
  busy.**
- **One processor core at 100% is normal for now.** The game keeps one core
  fully busy the whole time it runs, however light the scene is.
- **Frame rate 120 is experimental.** The game keeps its normal speed, but busy
  match views may show fewer than 120 pictures a second. The score display's
  meters, effects and most menu animations are unchecked at this rate.
- **"Menus wide" and "All wide" stretch flat screens** to the display's
  width, by design: menus, films, and the rendered player pictures on the
  player selection screens look wider than on a console. "4:3" keeps them in
  shape with black bars at the sides.
- **The Shape 4:3 setting with a wide display** shows menus and films with
  black bars at the sides, by design.

## Controllers

- **Four-player controllers** is experimental and off by default, and has not
  been tried with real controllers.
- **The default keyboard layout is basic.** It works, but it was not designed
  for comfort. The launcher's Keyboard page changes any key.
- PlayStation and Switch controllers work only through a layer that presents
  them as Xbox controllers (for example Steam Input).

## The game

- Only the USA Xbox release is supported. The disc is English only.
- **Rarely, the game can stop responding while the picture carries on.**
  Buttons do nothing and the sound can stop for many seconds. It is more likely
  on a busy PC. The game now guards against it, but what sets it off is not
  known, so it may still happen. Close the game, start it again and report it
  with the file `logs\game.log`.
- The publisher's logo and the legal screen always play at start. "Skip intro
  film" skips the film after them.
- A whole Be A Legend or Street School run and every NBA team are unchecked.
- A match pauses (the game's own pause menu) when another window comes to
  the front; menus keep running. During the camera fly-by before tip-off and
  the end-of-game countdown the pause waits until the game accepts one.
- Of the cheats, only Big Heads, the two balls and No Hud are confirmed working
  in a match.

## Setup and the launcher

- Windows 10, AMD graphics and Intel graphics on their own are untested.
- **The launcher needs an internet connection the first time it starts.** It
  downloads Microsoft's WebView2 package (about 9 MB), which draws its window.
  Without a connection it says so and closes; start it again once connected.
- On Windows 10 the launcher also needs Microsoft's Edge WebView2 Runtime. Windows
  11 includes it.
- **A synced or antivirus-watched install folder can slow or interrupt setup.**
  Programs such as OneDrive can hold a new file for a moment. Setup waits and
  retries; if it still stops, Try again carries on from there. The default
  install folder is not a synced one.
- With Windows' Smart App Control on, setup builds and runs a small test
  program first and stops if Windows blocks it. This is untested with the
  feature on.
- The launcher's update check has not yet delivered a real update.
