# Each test builds tests/<name>_test.cpp (dashes become underscores) with the
# shared library identity setup and links only the modules it exercises.
gio_env = ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local']
python = import('python').find_installation('python3')
# The launcher suite starts its own private bus, never the desktop session bus.
find_program('dbus-daemon')
find_program('glib-compile-schemas')

activation_server = custom_target('launch-activation-server-header',
  input: protocol_root / 'staging/xdg-activation/xdg-activation-v1.xml',
  output: 'launch-activation-server-protocol.h',
  command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

test_table = [
  {'name': 'adaptive-sidebar', 'deps': [model_dep]},
  {'name': 'ws-discovery', 'deps': [fs_dep], 'env': gio_env, 'wrapper': files('ws_discovery_test.py')},
  {'name': 'input-lifetime', 'deps': [noctalia_ui_dep]},
  {'name': 'window-move-gesture', 'deps': [model_dep]},
  {'name': 'tab-workspace', 'deps': [browser_dep]},
  {'name': 'job-routing', 'deps': [browser_dep]},
  {'name': 'operation-activity', 'deps': [app_jobs_dep, event_loop_dep], 'env': gio_env},
  {'name': 'drive-removal', 'deps': [app_jobs_dep, event_loop_dep], 'env': gio_env},
  {'name': 'device-mount', 'deps': [browser_dep], 'env': gio_env},
  {'name': 'archive-service', 'deps': [operations_dep, event_loop_dep], 'timeout': 90, 'env': gio_env},
  {'name': 'thumbnail', 'deps': [thumbnails_dep, imaging_dep, event_loop_dep, dependency('libwebp')],
    'timeout': 90, 'env': gio_env},
  {'name': 'preview-service', 'deps': [thumbnails_dep, ui_dep], 'env': gio_env},
  {'name': 'preview-metadata', 'deps': [thumbnails_dep, imaging_dep, event_loop_dep], 'timeout': 60, 'env': gio_env},
  {'name': 'clipboard', 'deps': [clipboard_dep, dependency('wayland-server')], 'env': gio_env},
  {'name': 'file-payload', 'deps': [clipboard_dep], 'env': gio_env},
  {'name': 'browser-actions', 'deps': [browser_dep, ui_dep], 'env': gio_env},
  {'name': 'empty-trash-service', 'deps': [operations_dep, event_loop_dep], 'env': gio_env},
  {'name': 'restore-service', 'deps': [operations_dep, event_loop_dep], 'env': gio_env},
  {'name': 'trash-browse', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'trash-service', 'deps': [operations_dep, browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'create-service', 'deps': [operations_dep, browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'rename-service', 'deps': [operations_dep, browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'batch-rename-service', 'deps': [operations_dep, event_loop_dep], 'env': gio_env},
  {'name': 'permission-service', 'deps': [operations_dep, event_loop_dep], 'env': gio_env},
  {'name': 'provider-move-history', 'deps': [operations_dep, event_loop_dep], 'timeout': 60, 'env': gio_env},
  {'name': 'provider-copy-history', 'deps': [operations_dep, event_loop_dep], 'timeout': 60, 'env': gio_env, 'wrapper': files('provider_copy_history_test.py')},
  {'name': 'provider-transfer', 'deps': [operations_dep, event_loop_dep], 'env': gio_env},
  {'name': 'transfer-service', 'deps': [operations_dep, event_loop_dep], 'timeout': 90, 'env': gio_env},
  {'name': 'places', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'icon-theme', 'deps': [thumbnails_dep, imaging_dep, system_dep, event_loop_dep], 'wrapper': files('icon_theme_test.py')},
  {'name': 'file-icon', 'deps': [thumbnails_dep, imaging_dep, system_dep, event_loop_dep], 'env': gio_env},
  {'name': 'folder-icon', 'deps': [fs_dep, system_dep, event_loop_dep], 'env': gio_env},
  {'name': 'file-tooltip', 'deps': [ui_dep, fs_dep, event_loop_dep], 'env': gio_env},
  {'name': 'view-preference-writer', 'deps': [config_dep, event_loop_dep], 'env': ['G_DEBUG=fatal-warnings']},
  {'name': 'directory-session', 'deps': [fs_dep, event_loop_dep], 'env': gio_env},
  {'name': 'directory-tree', 'deps': [browser_dep], 'timeout': 60, 'env': gio_env},
  {'name': 'directory-model', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'launch-activation', 'deps': [platform_dep, dependency('wayland-server')], 'sources': [activation_server]},
  {'name': 'extraction-launcher', 'deps': [platform_dep, event_loop_dep], 'env': gio_env},
  {'name': 'program-launcher', 'deps': [platform_dep, event_loop_dep], 'env': gio_env},
  {'name': 'file-launcher', 'deps': [platform_dep, event_loop_dep], 'wrapper': files('file_launcher_test.py')},
  {'name': 'browser-controller', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'miller-columns', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'navigation-services', 'deps': [browser_dep, platform_dep, event_loop_dep], 'env': gio_env},
  {'name': 'folder-totals', 'deps': [fs_dep], 'timeout': 60, 'env': gio_env},
  {'name': 'link-target', 'deps': [fs_dep], 'env': gio_env},
  {'name': 'selection-details', 'deps': [browser_dep, event_loop_dep], 'timeout': 60, 'env': gio_env},
  {'name': 'settings', 'deps': [config_dep, browser_dep], 'env': ['G_DEBUG=fatal-warnings']},
  {'name': 'atomic-file', 'deps': [fs_dep]},
  {'name': 'xdg-dirs', 'deps': [fs_dep]},
  {'name': 'shared-helpers', 'deps': [operations_dep], 'env': gio_env},
  {'name': 'state-store', 'deps': [config_dep], 'env': gio_env},
  {'name': 'folder-views', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'file-filter', 'deps': [fs_dep], 'env': gio_env},
  {'name': 'chooser-policy', 'deps': [browser_dep, event_loop_dep], 'env': gio_env},
  {'name': 'file-chooser-portal', 'deps': [platform_dep, event_loop_dep], 'env': gio_env},
]

foreach entry : test_table
  name = entry['name']
  local_gio = 'GIO_USE_VFS=local' in entry.get('env', [])
  exe = executable(name + '-test',
    'library_test_setup.cpp',
    name.replace('-', '_') + '_test.cpp',
    entry.get('sources', []),
    dependencies: entry['deps'] + [noctalia_ui_dep] + (local_gio ? [gio] : []),
    cpp_args: local_gio ? ['-DLUCIOLE_TEST_LOCAL_GIO'] : [],
    override_options: ['b_ndebug=false'],
  )
  if 'wrapper' in entry
    test(name, python, args: [entry['wrapper'], exe], timeout: entry.get('timeout', 30), env: entry.get('env', []))
  else
    test(name, exe, timeout: entry.get('timeout', 30), env: entry.get('env', []))
  endif
endforeach

test('headless-cli', python,
  args: [files('cli_test.py'), app, meson.project_version()],
  timeout: 30,
  env: ['LUCIOLE_ASSETS_DIR=' + (meson.project_source_root() / 'lib/assets')],
)
