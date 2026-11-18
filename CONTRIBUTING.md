# Contributing

Bug reports, measurements and fixes are welcome.

## Never post game content

Not in commits, pull requests or issues:

- the disc image, extracted game files, `default.xbe`;
- the C code the lifter generates (`port\src\recomp\gen\`), the toolkit's
  analysis output, or anything else derived from the game's program;
- save files, memory dumps, frame or audio captures, video;
- links to disc images or other copies of the game.

The project builds everything from each player's own disc, and stays
publishable only as long as it holds nothing of the game.

## Reporting a problem

Say which screen, what you did just before, and the exact text of any message.
Include the launcher's Settings, Advanced, Copy logs output as text, and your
Windows version, processor and graphics card. A screenshot of a problem is
fine in an issue.

## Code

- **One exact game version.** The port is built against the USA release with
  the `default.xbe` hash in [docs/BUILDING.md](docs/BUILDING.md). Every
  address in the code is for that file.
- **Fixes go in `port\`.** Wrap or replace a game function in
  `port\src\recomp_manual.c`, or add to the port's own files. A change to the
  toolkit is a patch file in `port\patches\` and needs a good reason; fixes
  that belong in the toolkit are better sent to it upstream.
- **Where code comes from.** This repository is MIT. Do not copy code from
  GPL projects (for example Cxbx-Reloaded or the GPL parts of xemu). Code
  from MIT, BSD or similar projects keeps its notice; add it to
  `THIRD-PARTY-NOTICES.md`.
- **One topic per pull request.** Say what it fixes, how you checked it
  (before and after, on what PC), and what you did not check.
- **New behaviour is off by default** until it has been played, unless it
  fixes something broken.
- **The game's log is for bug reports.** `logs\game.log` holds the version,
  the settings in effect, the graphics card and the error that ended the
  game. Probes, traces and test switches you needed for a fix stay out of
  the pull request.
- Text players read (the launcher, messages): plain labels and facts; no
  middle-dot separators, no filler lines.

## Know what you are sending

Run a change before you send it, and make sure you understand it. If you
cannot explain a change, it will not be merged.

## Licence

By contributing you agree that your contribution is released under the
project's MIT licence.
