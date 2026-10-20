# NOT TESTED (written on Gentoo, no Nix here). Use it like any package:
#   pkgs.callPackage ./package.nix { }
# and add it to environment.systemPackages (the password window needs the polkit rule under /run/current-system/sw/share/polkit-1).
# The three hashes are placeholders: build once, copy the "got:" hash from each error message.
{ lib
, stdenv
, fetchFromGitHub
, meson
, ninja
, pkg-config
, gettext
, wrapGAppsHook4
, libadwaita
, gtk4
, curl
, gdk-pixbuf
, lz4
, zlib
, fontconfig
, polkit
, coreutils
}:

let
  hdl-dump-src = fetchFromGitHub {
    owner = "ps2homebrew";
    repo = "hdl-dump";
    rev = "32c296c69cf9c263fcbe035004aa28c345b3b279";
    hash = lib.fakeHash;
  };
  pfsshell-src = fetchFromGitHub {
    owner = "ps2homebrew";
    repo = "pfsshell";
    rev = "8c92467b3d715c3698f1f8ce63a8a07e214d6c73";
    fetchSubmodules = true; # ps2sdk and others
    hash = lib.fakeHash;
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "memento";
  version = "0.1.0";

  src = fetchFromGitHub {
    owner = "meramadness";
    repo = "memento";
    rev = "v${finalAttrs.version}";
    hash = lib.fakeHash;
  };

  nativeBuildInputs = [ meson ninja pkg-config gettext wrapGAppsHook4 ];
  buildInputs = [ libadwaita gtk4 curl gdk-pixbuf lz4 zlib fontconfig ];

  # the disk tools are built first (by the same commands as scripts/build-tools.sh), then handed to Memento's build
  preConfigure = ''
    mkdir -p $NIX_BUILD_TOP/tools
    cp -r ${hdl-dump-src} $NIX_BUILD_TOP/hdl-dump && chmod -R u+w $NIX_BUILD_TOP/hdl-dump
    make -C $NIX_BUILD_TOP/hdl-dump RELEASE=yes IIN_OPTICAL_MMAP=no
    cp $NIX_BUILD_TOP/hdl-dump/hdl_dump $NIX_BUILD_TOP/tools/
    cp -r ${pfsshell-src} $NIX_BUILD_TOP/pfsshell && chmod -R u+w $NIX_BUILD_TOP/pfsshell
    meson setup --buildtype=release $NIX_BUILD_TOP/pfsshell-build $NIX_BUILD_TOP/pfsshell
    ninja -C $NIX_BUILD_TOP/pfsshell-build
    cp $NIX_BUILD_TOP/pfsshell-build/pfsshell $NIX_BUILD_TOP/tools/
  '';

  mesonFlags = [
    "-Dtools_dir=/build/tools" # $NIX_BUILD_TOP/tools, made in preConfigure
    "-Dpolkit_policy=enabled"
    "-Dstdbuf=${coreutils}/bin/stdbuf" # the helper runs under pkexec, whose PATH has no Nix store paths
  ];

  meta = {
    description = "Manage the games on a PS2 hard disk or SSD (OPL and POPStarter)";
    homepage = "https://github.com/meramadness/memento";
    license = with lib.licenses; [ gpl3Plus ofl mit gpl2Only ];
    platforms = lib.platforms.linux;
    mainProgram = "memento";
  };
})
