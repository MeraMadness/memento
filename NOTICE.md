# Third-party material

Memento itself is free software: Copyright (C) 2026 meramadness, under the GNU General Public License, version 3 or
later (see `LICENSE`).
It bundles or uses the following; their own licenses apply to them. `hdl_dump` and `pfsshell` are GPL programs that Memento
starts as separate programs (they are not linked into it), and their source is available from the addresses in `tools/VERSIONS.txt`.

| What | Where | License |
|---|---|---|
| **hdl_dump** (ps2homebrew fork, see `tools/VERSIONS.txt`) | `tools/bin/hdl_dump`, started as a separate program | GPL-2.0 |
| **pfsshell** (ps2homebrew, see `tools/VERSIONS.txt`) | `tools/bin/pfsshell`, started as a separate program | GPL-2.0 |
| **Cinzel** font, Copyright 2020 The Cinzel Project Authors | `data/fonts/` (text in `data/fonts/OFL.txt`) | SIL Open Font License 1.1 |
| **Catppuccin** Mocha color palette | colors in `src/ui/style.c` | MIT, see below |
| GTK 4, libadwaita, GLib | linked | LGPL-2.1-or-later |
| libcurl, zlib, liblz4 (library part), fontconfig, GdkPixbuf | linked | curl / zlib / BSD-2-Clause / MIT-style / LGPL-2.1 |
| PS1 conversion rules ported from **psx-vcd** (leji-a), which follows cue2pops v2.0 | `src/core/vcd.c` | MIT OR Apache-2.0 |

## Data it downloads (nothing of this is included)

- Game covers and art: the `ps2-covers` repository and the OPL Manager art archive on archive.org, only when you ask for art.
- Disc checksums: the Redump lists from redump.org, only when you use "Check the image" or update them in Settings.
  Only hashes and titles are read; no game data is downloaded.

Memento does not contain, download or help find games. You need your own legally obtained disc images.

## Pictures

The pixel-art pictures in `data/images/` and the logo in `data/icons/` were drawn by the project author.

## Catppuccin license

MIT License

Copyright (c) 2021 Catppuccin

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
