# Third-party notices

Gamebreaker's own code is under the MIT licence in `LICENSE`. That licence
covers the code and documents in this repository only. It grants no rights in
the game this project is for, its program, data, names or marks, which belong
to their owners. Some files derive from the xboxrecomp toolkit and carry its
MIT notice as well (below).

This file lists the work Gamebreaker builds on and the notices that travel with
it.

## xboxrecomp (MIT)

<https://github.com/sp00nznet/xboxrecomp>, used at commit `343b6d8` (v0.17.0).
The toolkit is not part of this repository or of the release zips. Setup
downloads it from its upstream repository at that commit, checks its files
against a pinned fingerprint (`setup/tools.lock`), applies
`port/patches/0001-backend-batch-ext.patch` to a copy and builds it on your
PC. Its own `LICENSE`, `NOTICE` and `LICENSES/` folder come with that
download, into the install folder's `tools\xboxrecomp` and `toolkit` folders.
From a clone, `docs/BUILDING.md` says how to get it.

These files of this repository derive from the toolkit's code and are also
covered by its notice below:

| File | What it takes from the toolkit |
|---|---|
| `port/src/main.c` | Started from `templates/new-game/src/main.c` |
| `port/src/recomp_manual.c` | Started from `templates/new-game/src/recomp_manual.c` |
| `port/CMakeLists.txt` | Started from `templates/new-game/CMakeLists.txt` |
| `port/.gitignore` | The template's file, unchanged |
| `port/patches/0001-backend-batch-ext.patch` | A change to `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c`, `nv2a_vsh_interp.c`, `nv2a_vsh_interp.h` and `xbox_memory_layout.c`; its context lines are the toolkit's code |
| `port/src/gb_d3d11.c` | Its pixel shader follows the toolkit's register-combiner evaluator, `src/kernel/nv2a_combiner.c` |
| `port/src/gb_pad.c`, `port/src/gb_pad_logic.c` | Replace the toolkit's `src/input/xinput_device.c`; the default controller path reproduces that file's Windows code |

The toolkit's notice:

```
MIT License

Copyright (c) 2026 sp00nz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## xemu (LGPL-2.1-or-later), inside the toolkit

<https://github.com/xemu-project/xemu>. Parts of the toolkit's audio chip and
graphics chip models are taken from xemu and keep their own licence, the GNU
Lesser General Public License, version 2.1 or later, and their copyright
notices. The game program built on your PC links them. The toolkit's `NOTICE`
file lists each file and its copyright holders, and its
`LICENSES/LGPL-2.1.txt` holds the licence text; both come with the toolkit
that setup downloads (in the install folder, `toolkit\NOTICE` and
`toolkit\LICENSES\`).

The toolkit also credits xemu as the reference for some of its own
implementations (texture unswizzling, the register combiners); see the second
part of its `NOTICE`. This repository does not contain xemu code.

## Tools setup and the launcher download

Setup and the launcher fetch these from their publishers and check each
against a pinned SHA-256 (`setup/tools.lock`,
`launcher/prototype/fetch-webview2.ps1`). They are not part of this
repository and keep their own licences:

- xboxrecomp, above (checked by the fingerprint of its files instead)
- Python, embeddable package (Python Software Foundation License)
- capstone (BSD-3-Clause)
- llvm-mingw (Apache-2.0 with LLVM exceptions; MinGW-w64 headers and runtime
  under their own permissive licences)
- CMake (BSD-3-Clause)
- Ninja (Apache-2.0)
- Microsoft Edge WebView2 SDK, for the launcher window (Microsoft's licence
  terms for the SDK)

## The game

This project contains no part of the game. It is used with the player's own
copy, read from their own disc. The game, its program, music, artwork, names
and marks belong to their owners. Gamebreaker is not affiliated with or
endorsed by the game's publisher, its developer, the league, its teams or its
players.
