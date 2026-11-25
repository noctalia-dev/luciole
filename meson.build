project('luciole', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=3',
    'werror=true',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')
scanner = find_program('wayland-scanner')
protocol_root = wayland_protocols.get_variable('pkgdatadir')
protocol_sources = []
protocol_headers = []
foreach protocol : [
  ['xdg-decoration-unstable-v1', 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
  ['xdg-foreign-unstable-v2', 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'],
  ['xdg-dialog-v1', 'staging/xdg-dialog/xdg-dialog-v1.xml'],
]
  header = custom_target(protocol[0] + '-header',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  source = custom_target(protocol[0] + '-source',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
  protocol_sources += source
  protocol_headers += header
endforeach
alias_target('generated-headers', protocol_headers)

gio = dependency('gio-2.0', version: '>=2.76')
gio_unix = dependency('gio-unix-2.0', version: '>=2.76')
curl = dependency('libcurl', version: '>=7.85')
xml = dependency('libxml-2.0', version: '>=2.9')
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version: '>=2.42')
if not cpp.has_header('stb/stb_image_resize2.h')
  error('system stb/stb_image_resize2.h is required')
endif
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')
xkbcommon = dependency('xkbcommon')

if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Recent GLib headers use __COUNTER__, which clang 23 rejects as a C2y extension under
# -Wpedantic -Werror. lib/ adds GLib with -I and its own -Wpedantic after project arguments,
# so mark the GLib include directory as a system directory instead (-isystem wins over -I).
add_project_arguments('-isystem', dependency('glib-2.0').get_variable(pkgconfig: 'includedir') / 'glib-2.0', language: 'cpp')

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: 'cpp')
endif

if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
  add_project_link_arguments('-Wl,--gc-sections', '-Wl,--as-needed', language: 'cpp')
else
  add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
endif

subdir('lib')

build_config = configuration_data()
build_config.set_quoted('LUCIOLE_VERSION', meson.project_version())
build_config.set_quoted('LUCIOLE_BUILD_TYPE', get_option('buildtype'))
build_config.set_quoted('LUCIOLE_COMPILER', cpp.get_id() + ' ' + cpp.version())
build_config.set_quoted('LUCIOLE_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'luciole/assets')
build_info = configure_file(
  input: 'src/app/build_info.h.in',
  output: 'luciole_build_info.h',
  configuration: build_config,
)
# An explicit packager revision wins; otherwise git describes the checkout at build time.
# Only cli.cpp and about_controller.cpp include this header, so a new commit rebuilds just them.
git = find_program('git', required: false)
if get_option('build_revision') == 'unknown' and git.found()
  build_revision = vcs_tag(
    input: 'src/app/build_revision.h.in',
    output: 'luciole_build_revision.h',
    command: [git, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  build_revision = configure_file(
    input: 'src/app/build_revision.h.in',
    output: 'luciole_build_revision.h',
    configuration: {'VCS_TAG': get_option('build_revision')},
  )
endif
fs = import('fs')
completion_config = configuration_data()
completion_config.set('BASH_COMPLETIONS', fs.read('assets/completions/luciole.bash'))
completion_config.set('ZSH_COMPLETIONS', fs.read('assets/completions/_luciole'))
completion_config.set('FISH_COMPLETIONS', fs.read('assets/completions/luciole.fish'))
completions = configure_file(
  input: 'src/app/completion_scripts.h.in',
  output: 'luciole_completions.h',
  configuration: completion_config,
)
app_icon = configure_file(
  input: 'src/app/app_icon.h.in',
  output: 'luciole_app_icon.h',
  configuration: {'APP_ICON': fs.read('assets/icons/dev.noctalia.luciole.svg')},
)
src_inc = include_directories('src')

# One static library per module. A module's dependencies list only the modules
# its sources include, following the layering in CONTRIBUTING.md. Modules that
# must not see lib/ (fs, model, config, browser, operations) never depend on
# noctalia_ui_dep, so including a lib/ header from them fails to compile.
model_dep = declare_dependency(include_directories: src_inc)

event_loop_lib = static_library('luciole-event-loop',
  files('src/event_loop/glib_poll_source.cpp'),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
event_loop_dep = declare_dependency(link_with: event_loop_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

libarchive = dependency('libarchive')
liblzma = dependency('liblzma')
fs_lib = static_library('luciole-fs',
  files(
    'src/fs/archive_type.cpp',
    'src/fs/atomic_file.cpp',
    'src/fs/background_job.cpp',
    'src/fs/cancel_token.cpp',
    'src/fs/custom_icons.cpp',
    'src/fs/directory_entries.cpp',
    'src/fs/directory_session.cpp',
    'src/fs/directory_watch.cpp',
    'src/fs/drive_monitor.cpp',
    'src/fs/entry_access.cpp',
    'src/fs/file_error.cpp',
    'src/fs/file_icon_source.cpp',
    'src/fs/file_filter.cpp',
    'src/fs/file_metadata.cpp',
    'src/fs/file_query.cpp',
    'src/fs/filename.cpp',
    'src/fs/folder_icon.cpp',
    'src/fs/folder_totals.cpp',
    'src/fs/folder_totals_walk.cpp',
    'src/fs/gio_interop.cpp',
    'src/fs/heap_trim.cpp',
    'src/fs/interned_text.cpp',
    'src/fs/link_target.cpp',
    'src/fs/location.cpp',
    'src/fs/location_mount.cpp',
    'src/fs/mount_interaction.cpp',
    'src/fs/native_listing.cpp',
    'src/fs/system_mounts.cpp',
    'src/fs/systemd_mounts.cpp',
    'src/fs/system_mount_monitor.cpp',
    'src/fs/network_listing.cpp',
    'src/fs/ws_discovery.cpp',
    'src/fs/ws_discovery_metadata.cpp',
    'src/fs/ws_discovery_protocol.cpp',
    'src/fs/transfer_location.cpp',
    'src/fs/posix_fd.cpp',
    'src/fs/recursive_search.cpp',
    'src/fs/trash_info.cpp',
    'src/fs/trash_availability.cpp',
    'src/fs/xdg_dirs.cpp',
  ),
  include_directories: src_inc,
  dependencies: [gio, gio_unix, libarchive, liblzma, curl, xml],
)
fs_dep = declare_dependency(link_with: fs_lib, include_directories: src_inc, dependencies: [gio, gio_unix, libarchive, liblzma, curl, xml])

config_lib = static_library('luciole-config',
  files(
    'src/config/diagnostic.cpp',
    'src/config/folder_view_store.cpp',
    'src/config/keybinds.cpp',
    'src/config/settings.cpp',
    'src/config/shared_bookmarks.cpp',
    'src/config/state_store.cpp',
    'src/config/toml_decoder.cpp',
    'src/config/view_preference_writer.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, tomlplusplus, xkbcommon],
)
config_dep = declare_dependency(link_with: config_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, xkbcommon],
)

browser_lib = static_library('luciole-browser',
  files(
    'src/browser/action_registry.cpp',
    'src/browser/browser_controller.cpp',
    'src/browser/browser_mount.cpp',
    'src/browser/chooser_policy.cpp',
    'src/browser/directory_model.cpp',
    'src/browser/directory_tree.cpp',
    'src/browser/entry_details.cpp',
    'src/browser/folder_views.cpp',
    'src/browser/selection_details.cpp',
    'src/browser/job_routing.cpp',
    'src/browser/location_presentation.cpp',
    'src/browser/miller_columns.cpp',
    'src/browser/path_completion.cpp',
    'src/browser/places.cpp',
    'src/browser/selection_tools.cpp',
    'src/browser/tab_workspace.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep],
)
browser_dep = declare_dependency(link_with: browser_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep],
)

libacl = dependency('libacl')
operations_lib = static_library('luciole-operations',
  files(
    'src/operations/archive_icons.cpp',
    'src/operations/archive_input.cpp',
    'src/operations/archive_provider_process.cpp',
    'src/operations/archive_rar_input.cpp',
    'src/operations/archive_sevenzip.cpp',
    'src/operations/archive_xz_input.cpp',
    'src/operations/archive_service.cpp',
    'src/operations/archive_reader.cpp',
    'src/operations/archive_support.cpp',
    'src/operations/archive_writer.cpp',
    'src/operations/batch_rename_service.cpp',
    'src/operations/create_service.cpp',
    'src/operations/create_receipt.cpp',
    'src/operations/move_receipt.cpp',
    'src/operations/move_sequence.cpp',
    'src/operations/copy_receipt.cpp',
    'src/operations/copied_move_receipt.cpp',
    'src/operations/copy_history_tree.cpp',
    'src/operations/creation_stage.cpp',
    'src/operations/history_mount.cpp',
    'src/operations/empty_trash_service.cpp',
    'src/operations/permission_accounts.cpp',
    'src/operations/permission_jobs.cpp',
    'src/operations/permission_service.cpp',
    'src/operations/permission_target.cpp',
    'src/operations/permission_walk.cpp',
    'src/operations/posix_acl.cpp',
    'src/operations/rename_service.cpp',
    'src/operations/rename_receipt.cpp',
    'src/operations/restore_service.cpp',
    'src/operations/template_catalog.cpp',
    'src/operations/transfer_copy.cpp',
    'src/operations/transfer_service.cpp',
    'src/operations/provider_transfer.cpp',
    'src/operations/provider_copy_proof.cpp',
    'src/operations/provider_copy_receipt.cpp',
    'src/operations/transfer_staging.cpp',
    'src/operations/trash_item.cpp',
    'src/operations/trash_service.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive, liblzma, dependency('threads')],
)
operations_dep = declare_dependency(link_with: operations_lib, include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive, liblzma, dependency('threads')],
)

libpng = dependency('libpng')
imaging_lib = static_library('luciole-imaging',
  files(
    'src/imaging/pixbuf_image_decoder.cpp',
    'src/imaging/png_decoder.cpp',
    'src/imaging/raster_decoder.cpp',
    'src/imaging/svg_rasterizer.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, gdk_pixbuf, libpng, dependency('librsvg-2.0')],
)
imaging_dep = declare_dependency(link_with: imaging_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, gdk_pixbuf],
)

system_lib = static_library('luciole-system',
  files(
    'src/system/freedesktop_key_file.cpp',
    'src/system/icon_resolver.cpp',
    'src/system/icon_theme_poll_source.cpp',
  ),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
system_dep = declare_dependency(link_with: system_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

thumbnails_lib = static_library('luciole-thumbnails',
  files(
    'src/thumbnails/file_icon_cache.cpp',
    'src/thumbnails/preview_service.cpp',
    'src/thumbnails/preview_metadata.cpp',
    'src/thumbnails/metadata_probe.cpp',
    'src/thumbnails/metadata_parse.cpp',
    'src/thumbnails/metadata_worker.cpp',
    'src/thumbnails/provider_process.cpp',
    'src/thumbnails/thumbnail_codec.cpp',
    'src/thumbnails/thumbnail_provider.cpp',
    'src/thumbnails/thumbnail_service.cpp',
    'src/thumbnails/thumbnail_store.cpp',
    'src/thumbnails/thumbnail_worker.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, imaging_dep, system_dep, noctalia_ui_dep, libpng, dependency('nlohmann_json')],
)
thumbnails_dep = declare_dependency(link_with: thumbnails_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, imaging_dep, noctalia_ui_dep],
)

wayland_client = dependency('wayland-client')
clipboard_lib = static_library('luciole-clipboard',
  files(
    'src/clipboard/file_payload.cpp',
    'src/clipboard/text_clipboard.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)
clipboard_dep = declare_dependency(link_with: clipboard_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)

platform_lib = static_library('luciole-platform',
  files(
    'src/platform/desktop_integration.cpp',
    'src/platform/file_chooser_portal.cpp',
    'src/platform/extraction_launcher.cpp',
    'src/platform/file_launcher.cpp',
    'src/platform/launch_activation.cpp',
    'src/platform/launch_context.cpp',
    'src/platform/program_launcher.cpp',
    'src/platform/terminal_launcher.cpp',
    'src/platform/terminal_command.cpp',
  ),
  protocol_sources,
  protocol_headers,
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client, gio_unix, dependency('wayland-cursor')],
)
platform_dep = declare_dependency(link_with: platform_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client, gio_unix],
  sources: protocol_headers,
)

ui_lib = static_library('luciole-ui',
  files(
    'src/ui/browser/action_shortcuts.cpp',
    'src/ui/browser/file_icon.cpp',
    'src/ui/browser/file_tooltip.cpp',
    'src/ui/browser/file_view.cpp',
    'src/ui/browser/file_error_text.cpp',
    'src/ui/browser/places_sidebar.cpp',
    'src/ui/browser/sidebar_tree_view.cpp',
    'src/ui/controls/acl_editor.cpp',
    'src/ui/controls/archive_view.cpp',
    'src/ui/controls/bulk_acl_editor.cpp',
    'src/ui/controls/card_style.cpp',
    'src/ui/controls/operations_view.cpp',
    'src/ui/controls/file_preview_view.cpp',
    'src/ui/controls/file_metadata_view.cpp',
    'src/ui/controls/folder_totals_view.cpp',
    'src/ui/controls/glyph_picker.cpp',
    'src/ui/controls/icon_picker.cpp',
    'src/ui/controls/miller_view.cpp',
    'src/ui/controls/permission_editor.cpp',
    'src/ui/controls/permission_results_view.cpp',
    'src/ui/controls/recursive_permission_editor.cpp',
    'src/ui/controls/selection_contour.cpp',
    'src/ui/controls/tooltip_presenter.cpp',
    'src/ui/dialogs/archive_dialog.cpp',
    'src/ui/dialogs/batch_rename_dialog.cpp',
    'src/ui/dialogs/dialog_chrome.cpp',
    'src/ui/dialogs/link_dialog.cpp',
    'src/ui/dialogs/name_dialog.cpp',
    'src/ui/dialogs/mount_dialog.cpp',
    'src/ui/dialogs/settings_dialog.cpp',
    'src/ui/palette_file.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep, tomlplusplus],
)
ui_dep = declare_dependency(link_with: ui_lib, include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep],
)

app_jobs_lib = static_library('luciole-app-jobs',
  files('src/app/operation_hub.cpp', 'src/app/job_activity.cpp', 'src/app/drive_removal.cpp'),
  include_directories: src_inc,
  dependencies: [browser_dep, operations_dep],
)
app_jobs_dep = declare_dependency(link_with: app_jobs_lib, include_directories: src_inc,
  dependencies: [browser_dep, operations_dep],
)

app = executable('luciole',
  files(
    'src/app/application.cpp',
    'src/app/cli.cpp',
    'src/app/file_chooser_session.cpp',
    'src/app/file_manager_service.cpp',
    'src/app/instance_service.cpp',
    'src/app/library_setup.cpp',
    'src/app/main_loop.cpp',
    'src/app/page/about_controller.cpp',
    'src/app/page/active_popup.cpp',
    'src/app/page/application_chooser_controller.cpp',
    'src/app/page/archive_controller.cpp',
    'src/app/page/batch_rename_controller.cpp',
    'src/app/page/browser_page.cpp',
    'src/app/page/create_controller.cpp',
    'src/app/page/details_header_controller.cpp',
    'src/app/page/dialog_dropdown.cpp',
    'src/app/page/program_controller.cpp',
    'src/app/page/drag_drop_controller.cpp',
    'src/app/page/file_keyboard_controller.cpp',
    'src/app/page/folder_icon_controller.cpp',
    'src/app/page/folder_view_controller.cpp',
    'src/app/page/location_bar_controller.cpp',
    'src/app/page/menu_controller.cpp',
    'src/app/page/mount_controller.cpp',
    'src/app/page/preview_controller.cpp',
    'src/app/page/properties_controller.cpp',
    'src/app/page/rename_controller.cpp',
    'src/app/page/settings_controller.cpp',
    'src/app/page/shortcuts_controller.cpp',
    'src/app/page/sidebar_controller.cpp',
    'src/app/page/sidebar_drawer.cpp',
    'src/app/page/status_bar_controller.cpp',
    'src/app/page/submenu_controller.cpp',
    'src/app/page/transfer_controller.cpp',
    'src/app/page/operations_controller.cpp',
    'src/app/page/trash_controller.cpp',
    'src/app/palette_watcher.cpp',
    'src/app/startup_trace.cpp',
    'src/app/workspace_application.cpp',
    'src/main.cpp',
  ),
  build_info,
  build_revision,
  completions,
  app_icon,
  include_directories: include_directories('src', '.'),
  dependencies: [app_jobs_dep, ui_dep, clipboard_dep, platform_dep, imaging_dep, system_dep, event_loop_dep, browser_dep,
    config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep, wayland_client, dependency('threads')],
  install: true,
)

configure_file(
  input: 'assets/dbus/dev.noctalia.luciole.FileManager1.service.in',
  output: 'dev.noctalia.luciole.FileManager1.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1/services',
)

configure_file(
  input: 'assets/dbus/org.freedesktop.impl.portal.desktop.luciole.service.in',
  output: 'org.freedesktop.impl.portal.desktop.luciole.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1/services',
)
install_data('assets/portal/luciole.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal/portals')

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  subdir('tests')
endif
if get_option('benchmarks')
  subdir('tools/bench')
endif

install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/luciole')
install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'luciole/assets')
install_data('assets/completions/luciole.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'luciole',
)
install_data('assets/completions/_luciole', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/luciole.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
install_data('assets/desktop/dev.noctalia.luciole.desktop', install_dir: get_option('datadir') / 'applications')
install_data('assets/desktop/dev.noctalia.luciole-chooser.desktop', install_dir: get_option('datadir') / 'applications')
install_data('assets/icons/dev.noctalia.luciole.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
