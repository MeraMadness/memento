# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit git-r3 meson toolchain-funcs xdg

DESCRIPTION="Manage the games on a PS2 hard disk or SSD (OPL and POPStarter)"
HOMEPAGE="https://github.com/meramadness/memento"
SRC_URI="https://github.com/meramadness/memento/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"

# the disk tools are built from source at the commits in tools/VERSIONS.txt (pfsshell needs its git submodules)
HDL_DUMP_COMMIT="32c296c69cf9c263fcbe035004aa28c345b3b279"
PFSSHELL_COMMIT="8c92467b3d715c3698f1f8ce63a8a07e214d6c73"

# Memento GPL-3+, Catppuccin colors MIT, Cinzel font OFL-1.1, hdl_dump and pfsshell GPL-2 (separate programs it runs)
LICENSE="GPL-3+ OFL-1.1 MIT GPL-2"
SLOT="0"
KEYWORDS="~amd64"

RDEPEND="
	>=gui-libs/libadwaita-1.7:1
	gui-libs/gtk:4
	net-misc/curl
	x11-libs/gdk-pixbuf:2
	app-arch/lz4:=
	sys-libs/zlib
	media-libs/fontconfig
	sys-auth/polkit
	sys-apps/coreutils
"
DEPEND="${RDEPEND}"
BDEPEND="
	sys-devel/gettext
	virtual/pkgconfig
	dev-vcs/git
"

src_unpack() {
	default

	# the two disk tools: plain git checkouts next to the unpacked source
	EGIT_REPO_URI="https://github.com/ps2homebrew/hdl-dump.git"
	EGIT_COMMIT="${HDL_DUMP_COMMIT}"
	EGIT_CHECKOUT_DIR="${WORKDIR}/hdl-dump"
	EGIT_SUBMODULES=( '-*' )
	git-r3_src_unpack

	EGIT_REPO_URI="https://github.com/ps2homebrew/pfsshell.git"
	EGIT_COMMIT="${PFSSHELL_COMMIT}"
	EGIT_CHECKOUT_DIR="${WORKDIR}/pfsshell"
	EGIT_SUBMODULES=( '*' )
	git-r3_src_unpack
}

src_configure() {
	local emesonargs=(
		-Dtools_dir="${WORKDIR}/tools"
		-Dpolkit_policy=enabled
	)
	meson_src_configure
}

src_compile() {
	mkdir "${WORKDIR}/tools" || die

	# hdl_dump: needs nothing but a C compiler
	emake -C "${WORKDIR}/hdl-dump" RELEASE=yes IIN_OPTICAL_MMAP=no CC="$(tc-getCC)"
	cp "${WORKDIR}/hdl-dump/hdl_dump" "${WORKDIR}/tools/" || die

	# pfsshell: its own meson project, built apart from Memento's
	meson setup --buildtype=release "${WORKDIR}/pfsshell-build" "${WORKDIR}/pfsshell" || die
	meson compile -C "${WORKDIR}/pfsshell-build" || die
	cp "${WORKDIR}/pfsshell-build/pfsshell" "${WORKDIR}/tools/" || die

	meson_src_compile
}

pkg_postinst() {
	xdg_pkg_postinst
	elog "Start it from the application menu or with: memento"
	elog "Memento asks for your administrator password once per session (polkit) to reach the PS2 disk."
}
