# Packaging Memento

## How the disk access is kept safe

Memento's window runs as your user. The only program that touches the disk is `ps2gm-helper`, started through `pkexec`
(polkit, one password per session). A program that asks for the administrator password must not live in a place that an
ordinary user (or any program running as that user) can change, otherwise it is a way to become root. So:

- **The helper refuses to run as root** unless it and the two disk tools next to it (`hdl_dump`, `pfsshell`) belong to root
  and no folder above them can be written by anyone else (`ps2gm_path_root_safe`, checked on every start).
- A system install (distribution package, or `scripts/install.sh --system`) satisfies this and also installs a **polkit rule**
  (`io.github.meramadness.Memento.policy`) so the password window says what Memento wants.
  (polkit reads rules only from `/usr/share/polkit-1/actions`, so `scripts/install.sh --system`, which installs into
  `/usr/local`, copies the rule there itself.)
- A copy in your home folder (`scripts/install.sh` without `--system`, or running from the build folder) is **refused for
  pkexec**. For development give your user access to the disk once instead: `sudo setfacl -m u:$USER:rw /dev/sdX`; the helper
  then runs as you and needs no password.

Layout of an installed copy (`$prefix` = `/usr`, `$libdir` = `lib` or `lib64`):

```
$prefix/bin/memento                       -> ../$libdir/memento/ps2gm
$prefix/$libdir/memento/ps2gm             the program
$prefix/$libdir/memento/ps2gm-helper      the root helper (named in the polkit rule)
$prefix/$libdir/memento/hdl_dump, pfsshell
$prefix/$libdir/memento/{fonts,images,locale}
$prefix/share/{applications,icons,polkit-1/actions}/...
```

## Build options (meson_options.txt)

| option | what |
|---|---|
| `-Dtools_dir=DIR` | folder with the built `hdl_dump` and `pfsshell` to install (default: `tools/bin` of the source tree) |
| `-Dpolkit_policy=enabled/disabled/auto` | the polkit rule (auto: when the prefix is under `/usr`) |
| `-Dstdbuf=/full/path/stdbuf` | for systems where `stdbuf` is not in `/usr/bin` (NixOS) |

`hdl_dump` and `pfsshell` are separate GPL programs and are **built from source** at the commits in `tools/VERSIONS.txt`
(`scripts/build-tools.sh` does the same for development). `pfsshell` needs its git submodules (ps2sdk), so a plain tarball
of it is not enough: use git.

## Making the source tarball

```sh
scripts/make-dist.sh [folder]      # memento-<version>.tar.gz: sources only, no build folders, no prebuilt tools
```

## Recipes in `packaging/`

| | file | state |
|---|---|---|
| Gentoo | `gentoo/games-util/memento/memento-0.1.2.ebuild` | the build steps were replayed by hand on Gentoo (tools from the pinned commits, Memento from the tarball with `-Dtools_dir`, staged install, the staged program starts and finds fonts, images and translations). Portage itself could not be run without root: do `ebuild memento-0.1.2.ebuild manifest` and a real `emerge` once |
| Arch / AUR | `arch/PKGBUILD` | **not tested** |
| Nix | `nix/package.nix` | **not tested**; the three `lib.fakeHash` values must be filled in |
| Flatpak | `flatpak/io.github.meramadness.Memento.yml` | see below; **not built yet** |

The recipes point at `github.com/meramadness/memento`: before using them, publish the code there, tag `v0.1.2`, and fill in
the tarball checksums. An AppImage cannot install a root-owned helper, so it would only work with the `setfacl` way.

### Flatpak (Flathub, Steam Deck)

The application ID is `io.github.meramadness.Memento` (Flathub's rule for GitHub-hosted apps), which is why the desktop file,
the icons, the polkit rule and `APP_ID` carry that name; the program, folders and command are still `ps2gm` / `memento`.

A Flatpak has no `pkexec`, so a PS2 hard disk (`/dev/sdX`) is opened another way: the app asks **UDisks** (the system's disk
service, `--system-talk-name=org.freedesktop.UDisks2`) to open the disk. Polkit shows the password window (`auth_admin_keep`:
once per session) and UDisks hands back the open file descriptor (`src/core/udisks.c`). The app starts the helper with that
descriptor as `fd:3` (`backend_start_fd`), the helper checks the first sector for the APA header and passes the same descriptor
on to `hdl_dump` and `pfsshell` (`ps2gm_tools_inherit_fd`). The two tools accept `fd:N` as a disk name through the patches in
`tools/patches/` (one function each: `osal_open_device_for_writing` / `osal_map_device_name` in hdl_dump, `set_atad_device_path`
in pfsshell); `scripts/build-tools.sh` and the Flatpak manifest apply them, the other recipes do not need them. The helper
refuses `fd:N` when it runs as root, and `tests/test_fd.sh` checks it (skipped when the tools are not patched). Inside the sandbox
the program never sees `/dev`, so no `--device=all` is requested. `job_connect` in `src/ui/connect_page.c` does this when
`/.flatpak-info` exists; if UDisks refuses, the message says why (and, when the device node is visible, the `setfacl` alternative).
SD cards, USB drives and MX4SIO cards (folder mode) need no extra step: the manifest allows `/run/media` and the home folder.

```sh
# needs flatpak-builder (from your package manager) and: flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50
cd packaging/flatpak
flatpak-builder --user --install --force-clean build-dir io.github.meramadness.Memento.yml
flatpak run io.github.meramadness.Memento
```

`flatpak-builder` builds `hdl_dump` and `pfsshell` from the same pinned commits as the other recipes. For Flathub the last
module's `dir` source becomes the release tag (`type: git`, `url`, `tag`, `commit`).

### Trying the Gentoo ebuild in your local overlay

```sh
scripts/make-dist.sh /var/cache/distfiles            # needs write access there (portage group)
sudo mkdir -p /var/db/repos/local/games-util/memento
sudo cp packaging/gentoo/games-util/memento/* /var/db/repos/local/games-util/memento/
cd /var/db/repos/local/games-util/memento && sudo ebuild memento-0.1.2.ebuild manifest
sudo emerge -av games-util/memento
```
