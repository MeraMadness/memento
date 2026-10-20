# Packaging Memento

## How the disk access is kept safe

Memento's window runs as your user. The only program that touches the disk is `ps2gm-helper`, started through `pkexec`
(polkit, one password per session). A program that asks for the administrator password must not live in a place that an
ordinary user (or any program running as that user) can change, otherwise it is a way to become root. So:

- **The helper refuses to run as root** unless it and the two disk tools next to it (`hdl_dump`, `pfsshell`) belong to root
  and no folder above them can be written by anyone else (`ps2gm_path_root_safe`, checked on every start).
- A system install (distribution package, or `scripts/install.sh --system`) satisfies this and also installs a **polkit rule**
  (`io.github.memento.policy`) so the password window says what Memento wants.
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
| Gentoo | `gentoo/games-util/memento/memento-0.1.0.ebuild` | the build steps were replayed by hand on Gentoo (tools from the pinned commits, Memento from the tarball with `-Dtools_dir`, staged install, the staged program starts and finds fonts, images and translations). Portage itself could not be run without root: do `ebuild memento-0.1.0.ebuild manifest` and a real `emerge` once |
| Arch / AUR | `arch/PKGBUILD` | **not tested** |
| Nix | `nix/package.nix` | **not tested**; the three `lib.fakeHash` values must be filled in |

The recipes point at `github.com/meramadness/memento`: before using them, publish the code there, tag `v0.1.0`, and fill in
the tarball checksums. Flathub is not planned: its sandbox has no `pkexec`.
An AppImage cannot install a root-owned helper either, so it would only work with the `setfacl` way.

### Trying the Gentoo ebuild in your local overlay

```sh
scripts/make-dist.sh /var/cache/distfiles            # needs write access there (portage group)
sudo mkdir -p /var/db/repos/local/games-util/memento
sudo cp packaging/gentoo/games-util/memento/* /var/db/repos/local/games-util/memento/
cd /var/db/repos/local/games-util/memento && sudo ebuild memento-0.1.0.ebuild manifest
sudo emerge -av games-util/memento
```
