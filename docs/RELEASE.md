# Release checklist

Short, in the order to do it. Version in these examples: `0.1.0`.

## 1. The code is ready
- [ ] `tests/run_tests.sh` ends with `ALL TESTS PASSED`.
- [ ] Tried on the real console: install a game and play it, the covers show in OPL, a PS1 game, the Saves tab with a copy of a card.
- [ ] `scripts/update-po.sh`, then every language shows `0 untranslated` (`python3 scripts/po-fill.py <lang>`).
- [ ] The version is right in `meson.build` (`version:`); it also appears in Settings > About.
- [ ] README and `docs/` describe what the program does now (new features, new tabs).
- [ ] No leftovers: build folders, the empty `docs/opl-ref` folder, test files (`.gitignore` covers the build folders).

## 2. Legal and honest
- [ ] `LICENSE` (GPL-3.0-or-later) and `NOTICE.md` are current: every bundled or used thing is listed with its license.
- [ ] The README says Memento contains no games and does not help find any.
- [ ] Anything in `data/images/` and `data/icons/` is yours to publish.

## 3. Publish
- [ ] `git init`, first commit, push to `github.com/meramadness/memento` (the recipes already point there).
- [ ] Screenshots in the README (Library, Transfers, Saves).
- [ ] Tag and release:
  ```sh
  git tag -a v0.1.0 -m "Memento 0.1.0" && git push --tags
  ```
  Attach the source tarball made by `scripts/make-dist.sh` to the GitHub release (or use the automatic tag archive).

## 4. Packages (only once the tag exists)
- [ ] Gentoo: `ebuild memento-0.1.0.ebuild manifest` (the checksum changes when the tarball comes from GitHub), then a real `emerge`.
- [ ] Arch: put the real tarball checksum in `PKGBUILD` (`updpkgsums`), build in a clean chroot, then submit to the AUR if you want.
- [ ] Nix: fill in the three `lib.fakeHash` values by building once.
- [ ] Each recipe: bump the version when you release the next one.

## 5. After
- [ ] Say in the release notes what was tested on real hardware and what was not.
- [ ] Note that the non-Italian translations are machine-made and need a native speaker's review.
