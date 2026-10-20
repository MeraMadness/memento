# Project layout

Where things are, and where to look when you want to change something.

```
ps2gm/
  meson.build          what gets built (add new .c files to ui_sources / core_sources)
  src/
    core/              no window code at all: the logic, testable without a screen
      vcd.c            PS1: read .cue files, find the game ID, convert BIN -> POPStarter .VCD
      tools.c          wrappers for hdl_dump and pfsshell (the programs that touch the disk)
      art.c            download covers/art, convert to the PNG sizes OPL wants, your own pictures
      gamecfg.c        per-game OPL settings in the game's .cfg (compatibility modes, DMA, video mode, startup file)
      redump.c         Redump hash lists: download, zip/DAT reading, MD5 of an image, lookup
      ziso.c           ZSO files: read the game out of one, unpack, compress (all cores), pad for hdl_dump
      cheats.c         check a cheat list the way OPL reads it; set keys in a game's .cfg
      mcfs.c           memory card images: list, import/export .psu, delete (shared with ../savemgr, keep both in step)
      backend.c        talks to the helper program, reads the library (games, art, free space)
      ops.c            the real actions: install PS2/PS1 game, remove, update art
    helper/helper.c    the only program that runs as administrator (pkexec); a fixed list of commands
    ui/                the window (GTK4 + libadwaita)
      app.h            shared state (struct App) and the list of functions shared between files
      main.c           the window: top bar, pages, bottom tabs, program start
      i18n.c           translations (gettext) and the language setting
      common.c         toast, error dialog, size text, "row with a button"
      jobs.c           short jobs (remove, read, save...) in a thread behind a progress dialog; installs use transfers.c
      topbar.c         top bar (name, disk chip with free space, refresh) and the bottom tab bar
      transfers.c      Transfers tab and the install queue (background, one at a time, cancel where it is safe)
      verify.c         check an image against the Redump lists (Add page row, Settings group)
      saves_page.c     Saves tab: virtual memory cards (open one of the disk or a file, export/import/delete saves)
      settings_page.c  Settings tab: language, about and licenses
      style.c          the look: Catppuccin Mocha colors, Cinzel font loading, CSS classes - change colors and sizes here
      connect_page.c   Devices tab: choose the drive, connect, disconnect
      library_page.c   Library tab: cover grid or list, search/filters/sort, details panel of the selected game
      add_page.c       Add game (opened from the Library): install a PS2 ISO/ZSO or a PS1 BIN/CUE game
      apps_page.c      the Homebrew tab: homebrew on the APPS partition (add, update keeping the old one, rename, remove)
      settings_dialog.c  OPL settings of a PS2 game: compatibility modes, disk speed, video mode
      art_dialog.c     the art dialog of one game: download all, or change a single picture
      cover_dialog.c   the cover carousel: pick a front cover from several, or add your own
      cheats_dialog.c  edit the cheat list of a PS2 game (OPL's .cht), switch the cheat engine on
      script.c         test-only: PS2GM_SCRIPT=... drives the window without a person
    tools/             small command line programs (ps2gm-vcd, ps2gm-art) for testing the core
  data/                the icon (data/icons), the Cinzel font (data/fonts, OFL), the pixel-art pictures (data/images) and the menu entry template (io.github.memento.desktop.in)
  po/                  translations: ps2gm.pot (all texts), <lang>.po (it, es, fr, de, pt_BR)
  tests/               run_tests.sh runs everything; make_disk.sh builds a fake PS2 disk file
  scripts/             build-tools.sh (hdl_dump/pfsshell), dev-run.sh (build and start), install.sh / uninstall.sh
                       (icon + menu entry for your user), update-po.sh + po-fill.py (translations)
  tools/bin/           the hdl_dump and pfsshell programs the app uses
```

## Rule of thumb

- **Looks** (text, order of rows, buttons, spacing): `src/ui/*_page.c`.
- **What an action does** (what gets copied where): `src/core/ops.c`.
- **How a file is converted or downloaded**: `src/core/vcd.c`, `src/core/art.c`.
- **What the app is allowed to write on the disk**: `src/helper/helper.c` (`location_allowed`).

The window code never touches the disk. It calls `ops_*` / `backend_*` from inside a job
(`run_job`), so the window keeps responding while the work runs.

## How a button works (example: "Install" for a PS2 game)

1. `add_page.c`: `build_add_page()` creates the row and connects the button to `on_install_ps2`.
2. `on_install_ps2()` copies the values from the widgets into a small struct and calls `run_job(...)`.
3. `jobs.c` shows the progress dialog and runs `job_install_ps2()` in a thread.
4. `job_install_ps2()` calls `ops_install_ps2()` in `core/ops.c`, which asks the helper to do the work.
5. When it finishes, `after_change()` (in `library_page.c`) shows "Done" and reads the disk again.

## Adding a new button or page

- New button on an existing page: write `on_xyz` in that page's file, connect it where the row is built.
- New page: add `src/ui/xyz_page.c` with `GtkWidget *build_xyz_page(void)`, declare it in `app.h`,
  list the file in `meson.build` (`ui_sources`), and add it to the stack in `main.c` (`activate`).
- New action that needs the disk: add it to `core/ops.c`; if it needs a new kind of write,
  allow it in `helper.c` and add a test in `tests/`.

## Before you trust a change

```
ninja -C build && tests/run_tests.sh <path to the psx-vcd reference tool>
```

The window itself can be checked without clicking, see the comment at the top of `src/ui/script.c`:
`PS2GM_SCRIPT="connect=disk.img;page=add;shot=out.png;quit" build/ps2gm`.

## Translations

Every text the user sees is written as `_("English text")` in the code (or `N_()` in tables). The program starts in the
system language, or the one chosen with the globe button in the title bar (saved in `~/.config/ps2gm/language`).

- **Fix or improve a translation:** edit `po/<lang>.po` (the `msgstr` lines), then `ninja -C build` (it builds the
  `.mo` files into `build/locale`) and start the program with that language to look at it.
- **After adding or changing a text in the code:** run `scripts/update-po.sh`, then `scripts/po-fill.py <lang>` to see what is
  still untranslated in a language, and `scripts/po-fill.py <lang> new.tsv` to add translations from a list of
  `English<TAB>translation` lines. (Do not use plain `msgmerge` output as it is: it pre-fills new texts with "fuzzy" guesses
  from similar old ones, which are wrong more often than not. `po-fill.py` ignores them.) A text without a translation
  shows in English.
- **Add a language:** copy `po/ps2gm.pot` to `po/<code>.po` (for example `po/nl.po`), fill in the header and the texts,
  add `{"nl", "Nederlands"}` to the list in `src/ui/i18n.c`.
- Use `_()` for texts with a number in them as `g_dngettext(GETTEXT_PACKAGE, "%u game", "%u games", n)`, so every
  language gets its own plural rule. Never build a sentence out of pieces; translate whole sentences.
- Try a language without changing the system: `LANGUAGE=de build/ps2gm`.
