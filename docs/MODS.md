# Using mods

A mod replaces some of the game's files with its own. The game on your disc
and in your install is never changed: turn a mod off and the game reads its own
files again.

Gamebreaker does not host or link to mods. You find them yourself.

## The Mods page

Open the launcher and choose **Mods** in the menu, between Settings and
Updates. The number beside it is how many mods are on.

Each installed mod is a plate on a vertical beam of light, in load order from
the top. A plate shows its place, picture, name, version, author, number of
files and size. Select a plate to see its details beside the stack: its
description, its files, its readme, and the buttons **Turn on** or **Turn
off**, **Open folder** and **Remove**.

A mod's files include single pieces of the game's archives, single lines of
its text, and pictures that replace the game's wherever they appear; a line
shows its number, and a plate counts its pictures beside its files. A file the game will not read is dimmed:
"not in the archive" when the archive has no such piece, "not used" when the
game left it out at its last start (point at it for the reason).

## Installing a mod

A mod comes as a `.zip`. Either choose **Install from file**, or drag the zip
onto the launcher window. The launcher reads the zip first and shows the mod
with **Install** and **Cancel**, or **Replace** if a mod of the same name is
already installed. A replaced mod keeps its place and whether it was on.

A new mod goes to the bottom of the stack, turned on. One zip at a time.

## On and off, and order

Click a plate's contact on the beam, or double-click the plate, to turn it on
or off. A mod that is off slides away from the beam.

The game reads the stack from the top. When two mods change the same file, the
same piece of an archive (named or numbered), the same line of text or the
same picture, the higher one wins, and a pink arc joins the two plates. The lower plate shows how
many of its files are overridden. Drag a plate to move it.

Keyboard: the arrow keys select, Enter turns on or off, Alt+Up and Alt+Down
move a mod, Delete removes it. Controller: A turns on or off, X lifts a plate
so the stick can move it (A or X puts it down, B puts it back), Y removes.

Changes take effect the next time you press Play.

## Turning every mod off

The switch next to the **Mods** title turns all mods off without changing
anything else on the page. The stack dims and the count reads "All off". Turn
it back on and the same mods, in the same order, are used again.

## Removing a mod

Select it and choose **Remove**, then **Remove** again. Its folder goes to the
Recycle Bin. If a file is in use, close the game first.

## The mods folder

Mods live in the `mods` folder of your install, next to `saves`. With the
default install folder that is `%LOCALAPPDATA%\Gamebreaker\mods`. **Open mods
folder** opens it.

Each mod is one folder in it. The order and which mods are on are kept in
`mods\load_order.txt`, which the launcher writes. A hidden `.merged` folder
holds copies of game archives with a mod's pieces put in; it can be deleted at
any time and is made again when needed.

## Saves

Mods never change your save directly. Whenever the set of mods in use differs
from the last start, the game first copies your save to
`saves\backups\<date time>_mods-changed\`. These copies are kept apart from the
five rolling backups and are never deleted for you.

A mod that changes the players (a roster mod) matters for saves: custom teams
and created players refer to players by their place in the roster. A save made
with a roster mod and loaded without it, or with another one, may point at
different players.

To go back to one, close the game, choose **Settings**, **Advanced**, **Restore
backup** in the launcher and pick the copy marked "before mods changed". The save it replaces is
kept as a backup of its own.

## What the page refuses

Nothing in a zip is ever run. A zip is refused, with nothing written, when it:

| Message | Why |
|---|---|
| Unsafe file path. | A file would land outside the mod's own folder. |
| Contains programs. | It holds programs or scripts (`.exe`, `.dll`, `.bat`, `.ps1`, `.js` and the like). |
| Over 4 GB unpacked. | Too large. |
| A file is over 2 GB. | Too large. |
| Over 20,000 files. | Too many files. |
| No files the game reads. | None of its files replaces a game file or picture. |
| Empty zip. | Nothing in it. |
| Not a .zip file. | Only `.zip` files install. |
| Not a zip, or damaged. | It cannot be read as a zip. |
| Damaged zip. | A file in it failed its check while unpacking. |
| Not enough free space. | The drive needs room for the mod plus 256 MB. |

## Back to the unmodified game

Turn the Mods switch off, or turn each mod off, and press Play. To remove mods
for good, remove them on the page or delete their folders from `mods`. If a
save was made with a roster mod, restore the `_mods-changed` copy as above.

## Reporting a problem with a mod

First turn all mods off and check whether the problem is still there. If it
only happens with a mod, report it to the mod's maker.

If it is a problem with Gamebreaker itself, attach `logs\game.log` from your
install, or the launcher's Settings, Advanced, **Copy logs** output. Its lines
that begin `[MODS]` list the mods in use, every file the game read from a mod,
and every file it left out with the reason. Say which mods you had on. Do not
attach the mods or any of the game's files.
