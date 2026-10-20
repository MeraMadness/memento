project('ps2gm', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'])

# where "meson install" puts things (scripts/install.sh uses the prefix ~/.local)
prefix = get_option('prefix')
libdir = get_option('libdir')
appdir = prefix / libdir / 'memento' # (scripts/install.sh passes --libdir=lib)
bindir = prefix / get_option('bindir')

if get_option('stdbuf') != ''
  add_project_arguments('-DPS2GM_STDBUF="' + get_option('stdbuf') + '"', language: 'c')
endif
add_project_arguments('-DGETTEXT_PACKAGE="ps2gm"', '-DAPP_VERSION="' + meson.project_version() + '"', language: 'c')

glib = dependency('glib-2.0', version: '>=2.70')
gio = dependency('gio-2.0')
adw = dependency('libadwaita-1', version: '>=1.7') # AdwWrapBox
curl = dependency('libcurl')
pixbuf = dependency('gdk-pixbuf-2.0')
lz4 = dependency('liblz4') # compresses games to ZSO
zlib = dependency('zlib') # reads the zip of a Redump list
fontconfig = dependency('fontconfig') # loads the bundled heading font

core_sources = files(
  'src/core/vcd.c',
  'src/core/tools.c',
  'src/core/art.c',
  'src/core/cheats.c',
  'src/core/gamecfg.c',
  'src/core/ziso.c',
  'src/core/mcfs.c',
  'src/core/redump.c',
  'src/core/backend.c',
  'src/core/ops.c',
)

core = static_library('ps2gm-core', core_sources,
  include_directories: include_directories('src/core'),
  dependencies: [glib, gio, curl, pixbuf, lz4, zlib])

core_dep = declare_dependency(
  link_with: core,
  include_directories: include_directories('src/core'),
  dependencies: [glib, gio, curl, pixbuf, lz4, zlib])

executable('ps2gm-art', 'src/tools/arttool.c', dependencies: core_dep)
executable('ps2gm-vcd', 'src/tools/vcdtool.c', dependencies: core_dep)
executable('ps2gm-ziso', 'src/tools/zisotool.c', dependencies: core_dep)
executable('ps2gm-redump', 'src/tools/redumptool.c', dependencies: core_dep)

test_tools = executable('test_tools', 'tests/test_tools.c', dependencies: core_dep)

executable('ps2gm-helper', 'src/helper/helper.c', dependencies: core_dep,
  install: true, install_dir: appdir)

test_backend = executable('test_backend', 'tests/test_backend.c', dependencies: core_dep)

test_ops = executable('test_ops', 'tests/test_ops.c', dependencies: core_dep)

# the ZSO tests make their reference files with liblz4's plain compressor, apart from the code under test
test_ziso = executable('test_ziso', 'tests/test_ziso.c', dependencies: core_dep)

test_vmc = executable('test_vmc', 'tests/test_vmc.c', dependencies: core_dep)

test_redump = executable('test_redump', 'tests/test_redump.c', dependencies: [core_dep, zlib])

ui_sources = files(
  'src/ui/main.c',
  'src/ui/common.c',
  'src/ui/jobs.c',
  'src/ui/i18n.c',
  'src/ui/connect_page.c',
  'src/ui/library_page.c',
  'src/ui/add_page.c',
  'src/ui/apps_page.c',
  'src/ui/art_dialog.c',
  'src/ui/cover_dialog.c',
  'src/ui/cheats_dialog.c',
  'src/ui/settings_dialog.c',
  'src/ui/topbar.c',
  'src/ui/transfers.c',
  'src/ui/verify.c',
  'src/ui/saves_page.c',
  'src/ui/settings_page.c',
  'src/ui/style.c',
  'src/ui/script.c',
)

executable('ps2gm', ui_sources, dependencies: [core_dep, adw, fontconfig],
  install: true, install_dir: appdir)

# the translations: po/<lang>.po -> build/locale/<lang>/LC_MESSAGES/ps2gm.mo (the program looks in "locale" next to itself)
custom_target('locale',
  output: 'locale.stamp',
  command: [find_program('sh'), files('scripts/build-locale.sh'), meson.project_source_root() / 'po',
            meson.project_build_root() / 'locale', '@OUTPUT@'],
  build_always_stale: true,
  build_by_default: true)

# ---- installing: program + helper + disk tools in one folder, a "memento" command, the icon and the menu entry
tools_dir = get_option('tools_dir') != '' ? get_option('tools_dir') : meson.project_source_root() / 'tools' / 'bin'
meson.add_install_script('scripts/install-tools.sh', tools_dir, libdir / 'memento')
install_symlink('memento', pointing_to: '../' + libdir + '/memento/ps2gm', install_dir: bindir)
meson.add_install_script('scripts/install-locale.sh', libdir / 'memento' / 'locale')
install_data('data/images/ps2-console.png', 'data/images/ps2-controller.png', 'data/images/ps2-disc.png', install_dir: appdir / 'images')
install_data('data/fonts/Cinzel[wght].ttf', 'data/fonts/OFL.txt', install_dir: appdir / 'fonts')
install_subdir('data/icons/hicolor', install_dir: prefix / 'share' / 'icons')

desktop_conf = configuration_data()
desktop_conf.set('BINDIR', bindir)
configure_file(input: 'data/io.github.memento.desktop.in', output: 'io.github.memento.desktop',
  configuration: desktop_conf, install: true, install_dir: prefix / 'share' / 'applications')

# the polkit rule: the password window says what Memento wants, and one password lasts for the session. Only for a system
# install (a copy in a home folder is refused by the helper anyway, see root_trusts_install in src/helper/helper.c)
use_policy = get_option('polkit_policy').enabled() or (get_option('polkit_policy').auto() and prefix.startswith('/usr'))
if use_policy
  policy_conf = configuration_data()
  policy_conf.set('HELPER', appdir / 'ps2gm-helper')
  configure_file(input: 'data/io.github.memento.policy.in', output: 'io.github.memento.policy',
    configuration: policy_conf, install: true, install_dir: prefix / get_option('datadir') / 'polkit-1' / 'actions')
endif
