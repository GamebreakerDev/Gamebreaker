# Making a mod

A mod is a folder of replacement files laid out the way the game's disc is laid
out. Gamebreaker reads each file in it in place of the disc's file with the
same path. No file of Gamebreaker's own is needed, and no program in a mod is
ever run.

How players install and use mods is in [Using mods](MODS.md).

## Where the game's files are

Setup unpacks your disc into the install's `game_files` folder
(`%LOCALAPPDATA%\Gamebreaker\game_files` by default). Every path below is
relative to it. Work from copies; the game reads `game_files` as it is.

## Laying out a mod

Each mod is one folder in the install's `mods` folder. Inside it, put each file
at the path it has on the disc. Folder and file names may be in any case, but
must use plain ASCII letters.

A worked example, a mod that recolours the match HUD:

1. Copy `game_files\fe\HUD.XSH` from your own install and recolour it in a tool
   that edits the game's SHPX textures.
2. Put the result at `mods\Green HUD\fe\HUD.XSH`.
3. Start the game from the launcher and play a match.

```
mods\
  Green HUD\
    fe\
      HUD.XSH          replaces fe\HUD.XSH
```

More of the same shape:

```
mods\My roster\database\xdbplyr.adf    replaces database\xdbplyr.adf
mods\My song\music\rocka\Main.mus      replaces music\rocka\Main.mus
```

A file the disc does not have, an empty file, or a file whose name uses
letters outside plain ASCII is left out, and `logs\game.log` says so.

## What can be replaced

Any file on the disc can be replaced whole, at any size. What the kinds are:

| Kind | Where | Notes |
|---|---|---|
| Textures | `fe\*.xsh` loose; inside the court archives, `xf\xf000001` to `xf000004` and `fe\xafeload.bin` | The SHPX texture format. Each picture inside has a four-letter name (a court's floor is `cort`); keep the names. Keep each picture's header in step with its size and pixel type. To replace a picture without editing these files, see [Replacing a picture wherever it appears](#replacing-a-picture-wherever-it-appears). |
| Courts | `xf\<court>.viv` | An archive of the court's textures and models. The game finds its pieces by name, so a whole court archive copied from another court draws no court. Replace pieces instead. |
| Models | `.o` entries in the court archives, and most entries of `xf\xf000001` and `xf000002` | ELF objects. Not yet tested. |
| Rosters | `database\*.adf` | Tables of records followed by a block of names. In `xdbplyr.adf` a name of another length moves every name after it. |
| Song list | `cscripts\pathsong.cfg` | Plain text, one `pathsong` line a song. Only the songs it lists play. |
| Songs | `music\<song>\` | Four files belong together: `<song>.mpf`, `Main.mus`, `Tricks.mus`, `Whoosh.mus`. |
| Films | `movies\*.nfx` | Keep the original's frame size: a film of another size is drawn at its own size. |
| Other scripts | `cscripts\*.cfg` | Plain text in the game's own command language. Not yet tested. |
| Sound banks | `sfx\*.abk`, `*.ast` | Not yet tested. |
| Commentary | entries of `speech\spchdats.big` | Not yet tested. |
| Text | lines of `locale\xloc_fe.eus` (menus) and `xloc_gm.eus` (screens over a match) | One line at a time; see [Changing one line of text](#changing-one-line-of-text). |
| Menu pictures | pieces of the `fe\xa*.bin` packs | SHPX pictures; see [Menu pictures](#menu-pictures). |
| Fonts, menu layouts, palettes | pieces of `fe\xfontsen.bin`, `fe\xlay_*.bin`, `fe\xpal_*.bin` | Not yet tested. |
| Animation | `anim\*.bin` | Not yet tested. |

A save is never read from a mod: saves live outside the disc's paths.

## Replacing one piece of an archive

Many disc files are archives that hold many pieces: the courts, the players'
models and textures, the loading screens, the commentary, the menus' pictures
and the text. To replace one piece without shipping the whole archive, use the
archive's own path as a folder and put the piece in it:

```
mods\My court\xf\forest.viv\foresto.xsh    a piece by its name
mods\My kit\xf\xf000002\417.xsh            a piece by its number
mods\My menu\fe\xafebg.bin\55.shpx         a menu picture by its number
```

A piece is named by its own name, in any case, or by its number in the
archive counted from 0. For a number only the part of the file name before the
first dot counts, so `417`, `0417`, `417.xsh` and `0417.shpx` all mean piece
417. `xf\xf000001`, `xf\xf000002`, `fe\xafeload.bin` and the picture packs
have no names, so numbers are the only way there. Two mods that replace the
same piece clash however each names it, and the Mods page shows it.

A replaced piece may be larger or smaller than the original. The first time the
game starts with it, Gamebreaker builds a copy of the archive with the new
pieces in the hidden `mods\.merged` folder, and reuses it until a file it was
made from changes. If another mod replaces the whole archive, the pieces are
put into that mod's copy, and must use names that copy has.

Every archive on the disc works this way. There are two kinds:

- BIG archives: every `xf\*.viv` court, `xf\xf000001` to `xf000004`,
  `fe\xafeload.bin` and `speech\spchdats.big`. List them with `big.py`.
- Packs: the other `fe\*.bin` files, `locale\xloc_fe.eus` and `xloc_gm.eus`,
  `credits\xcredits.bin`, `speech\plyrhdrs.big` and `speech\spchhdrs.big`.
  List them with `c0fb.py`. A pack with its new pieces must stay under 16 MB.

A replacement of a picture, a page of text or a font in a pack is checked
before it is used: one that is not of the same kind, or does not hold together
(a picture cut short, say), is left out, and `logs\game.log` says why.

### Finding a piece with big.py

`big.py` lists an archive's pieces and takes one out. It reads only your own
copy of the game and writes only the file you ask for. It ships in the release
folder at `app\port\tools\modkit\big.py`, and in the source repository at
`port\tools\modkit\big.py`. It needs Python 3; setup already fetched one into
the install's `tools\python` folder.

From the install folder, with `<big.py>` standing for the path above:

```
tools\python\python.exe <big.py> list game_files\xf\forest.viv
tools\python\python.exe <big.py> extract game_files\xf\forest.viv foresto.xsh foresto.xsh
tools\python\python.exe <big.py> extract game_files\xf\xf000002 417 417.xsh
```

`list` prints one line a piece: its number, offset, size, name (`-` when it has
none) and kind (texture, model, archive, or the first four bytes). `extract`
takes the archive, the piece by name or number, and the file to write.

### Finding a piece with c0fb.py

`c0fb.py`, beside `big.py`, does the same for packs, and also reads and writes
text and menu pictures. With `<c0fb.py>` standing for its path:

```
tools\python\python.exe <c0fb.py> list game_files\fe\xafebg.bin
tools\python\python.exe <c0fb.py> extract game_files\fe\xafebg.bin 55 55.shpx
tools\python\python.exe <c0fb.py> strings game_files\locale\xloc_fe.eus mainmenu.txt
tools\python\python.exe <c0fb.py> text game_files\locale\xloc_fe.eus mainmenu.txt 1 1.txt
tools\python\python.exe <c0fb.py> picture 55.shpx list
tools\python\python.exe <c0fb.py> picture 55.shpx 0 55.png
tools\python\python.exe <c0fb.py> import 55.shpx 0 55.png new.shpx
```

`list` and `extract` work as in `big.py`; the kinds are picture, text, font and
menu layout. `strings` prints every line of a text page as
`<page>\<line number>: <text>`, or of every page when none is given. `text`
writes one line to a file to edit. `picture <piece> list` lists the images in a
picture piece with their size and type; `picture` with an image number writes
that image as a PNG, and `import` puts an edited PNG of the same size back into
a new copy of the piece.

## Changing one line of text

The game's text is in pages (`mainmenu.txt` and so on) inside the two text
packs. A mod changes one line with a small text file named by the line's
number, in a folder named after the page:

```
mods\My text\locale\xloc_fe.eus\mainmenu.txt\1.txt
```

A worked example, renaming the main menu's "Play It":

1. List the main menu's lines:
   `tools\python\python.exe <c0fb.py> strings game_files\locale\xloc_fe.eus mainmenu.txt`.
   The line `mainmenu.txt\1: Play It` says it is line 1.
2. Make the file `mods\My text\locale\xloc_fe.eus\mainmenu.txt\1.txt` and
   write the new text in it, in UTF-8 (Notepad's default).
3. Start the game and look at the main menu.

A page can also be named by its number in the pack (`3.txt` for
`mainmenu.txt`), as any piece can. Line breaks at the end of the file are left
out; a line break inside it is a line break on the screen, and a file holding
only a line break blanks the line. Keep codes such as `%1S` or `%1d` where the
original has them: the game fills them in. The game's fonts draw English and
the accented letters of Western European languages; other characters may not
show. A line longer than the original is drawn longer and can run past its
box, since the menus do not wrap.

A line is left out, with the reason in the log, when the file is not UTF-8,
holds a control character such as a tab, holds a character outside the game's
16-bit text (an emoji, say), is longer than 2,000 characters, or names a line
past the page's last. Two mods may each change different lines of one page,
and one mod may replace a whole page while another changes a line of it.

## Menu pictures

A menu picture is one piece of a picture pack (`fe\xafebg.bin` holds the menus'
backgrounds), named by its number. To change one:

1. Find it with `c0fb.py list`, and take it out with `extract`.
2. Turn its image into a PNG with `picture <piece> 0 <file>.png`, and edit the
   PNG in any image editor, keeping its size.
3. Put it back with `import`, writing the new piece straight into the mod, for
   example to `mods\My menu\fe\xafebg.bin\55.shpx`.

The PNG route works for the 32-bit images, such as the menu backgrounds;
`picture <piece> list` shows each image's type, and the tool says when an
image is of another kind. A picture piece edited another way must keep its
size field and its list of images in step with its size.

## Replacing a picture wherever it appears

A mod can replace any picture the game draws (a court's floor, a jersey, the
HUD, menu art) without carrying any of the game's files: you supply your own
picture, named by the fingerprint of the one it replaces.

1. In the game, go to the screen you want to change and press
   **Ctrl + F9**. The texture dump switches on: the pictures on screen, and
   every picture the game draws after that, are written once to the
   `texture_dump` folder in the install folder, as `<fingerprint>.png`, and
   `texture_dump\index.txt` lists each one's size, format and when it was
   first drawn. Press **Ctrl + F9** again to switch it off. The launcher's
   Settings, Advanced, **Texture dump** switch starts the game with it on,
   and its **Open folder** button opens the folder.
2. Find the picture. Draw your own at the same shape and any size: the same
   size, or two or four times larger for a sharper result at high
   resolutions. Keep transparency where the original has it.
3. Save it in your mod as `textures\<fingerprint>.png`. Text after the 16
   digits, after a space, `_` or `-`, is for you:
   `textures\bfe1843ca49c34ef court floor.png`. Subfolders inside `textures`
   are fine, up to eight deep.
4. Start the game. `logs\game.log` says
   `[MODS] texture bfe1843ca49c34ef from <your mod>` when the picture is drawn,
   or why it could not be used.

PNG and DDS are accepted. DDS (BC1, BC2, BC3 or BC7, or 32-bit, with your own
mip levels) takes less memory and loads faster, which matters for large
packs; a compressed DDS must be a multiple of 4 wide and high. A picture may be
up to 8192 pixels a side and 256 MB a file. Other files in `textures` are left
out. When two mods replace the same picture, the first in the load order wins,
whatever their labels.

A fingerprint names the picture exactly as the unmodified game loads it. If
another mod replaces a game file that holds that picture, the picture's
fingerprint changes; take your dump with that mod on if yours is meant to go
on top of it. Films, and pictures the game redraws every few frames, cannot
be replaced this way.

## Not possible yet

- Turning the 8-bit menu pictures into PNGs. They can still be replaced as
  whole pieces.
- A piece of an archive that is itself inside an archive.
- Code of any kind.

## Packaging

A mod is shared as a `.zip`. Put the files either at the zip's root, or inside
one top folder that is not itself one of the disc's folders (`fe`, `xf` and so
on). A plain zip of files works.

To give the launcher a name, version, author and picture, add the files a
[Thunderstore](https://thunderstore.io) package carries, at the mod's top:

| File | What the launcher does with it |
|---|---|
| `manifest.json` | Reads `name` (underscores shown as spaces), `version_number` and `description`. On Thunderstore, `name` uses only `A-Z a-z 0-9 _`, the version is three whole numbers (`1.0.0`), and the description is at most 250 characters. An `authors` list is shown if present; a package downloaded from Thunderstore as `Team-Name-1.0.0.zip` shows the team as its author. At most 64 KB. |
| `icon.png` | Shown on the mod's plate. Thunderstore asks for exactly 256 x 256. At most 512 KB. |
| `README.md` | Shown in the mod's details: headings, lists and paragraphs, with links as plain text. The first 48 KB. |

The game skips these at a mod's top, together with `readme.txt`,
`CHANGELOG.md`, `mod.json` and any file starting `LICENSE`.

The installed folder is named `Team-Name` for a Thunderstore download, else
after the manifest's `name`, else after the zip without a trailing version.
Characters other than letters, digits, space, `_`, `.` and `-` become `_`, and
the name is cut at 60 characters.

## Rules a zip must meet to install

- Every path stays inside the mod: no `..`, no drive letters, no `:`, no
  reserved device names, no name ending in a dot or space, at most 200
  characters.
- No programs or scripts: `.exe`, `.dll`, `.bat`, `.cmd`, `.ps1`, `.js`,
  `.vbs`, `.msi`, `.lnk`, `.py`, `.reg`, `.xbe` and similar are refused.
- At most 4 GB unpacked, 2 GB a file and 20,000 files.
- At least one file the game reads, or one picture named by a fingerprint.

## Testing a mod

Put the mod's folder in `mods` by hand, or install its zip on the Mods page,
then press Play. A mod in `mods` with no `load_order.txt` is on; when that file
exists, the launcher keeps it, and a folder it does not list is off until you
turn it on on the page.

After the game closes, read `logs\game.log` in the install folder. Its `[MODS]`
lines say what happened:

```
[MODS] mods folder <folder>; on, first first: Green HUD (1 file), My court (1 file)
[MODS] D:\fe\HUD.xsh from Green HUD
[MODS] xf\forest.viv: 1 entry replaced (<number> from My court); merged archive built (<size> MB, <time> ms)
[MODS] locale\xloc_fe.eus: 1 line of text replaced (3 line 1 from My text); merged pack built (<size> KB, <change> bytes, <time> ms)
```

The first line lists the mods in use, top first. The archive and pack lines
give each replaced piece's number (and line) and its mod, and says `reused` instead of `built` when
nothing changed since the last start. A `from` line appears the first
time the game reads each replaced file, so a file with no such line was never
asked for in that run. When something is left out the line says why:

```
[MODS] <mod>\<path> is empty; not used
[MODS] <mod>\<path>: the disc has no such file and no archive by that name; not used
[MODS] <mod>\<archive>\<piece>: <archive> has no entry by that name or number (it has 16, numbered 0 to 15); not used
[MODS] <mod>\<pack>\<page>\<line>.txt is not used: it is not UTF-8 text
[MODS] <path>: <mod A> comes first, so <mod B>'s copy is not used
[MODS] D:\<path> from <mod> did not open (status 0xC0000001); the disc's file was used
```

The log keeps at most 200 lines of this kind. A file that opens but holds data
the game cannot use is not caught, and can crash the game; the `from` lines show
which mod files were read up to that point.

## Sharing

A mod made from the game's own files contains the publisher's copyrighted
material, and is not yours to share publicly. Share only what you made.
Gamebreaker does not host or link to mods.
