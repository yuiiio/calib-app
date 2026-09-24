project('calib-gamma', 'c', default_options: ['c_std=c11'])

wayland_client = dependency('wayland-client')
wayland_scanner = dependency('wayland-scanner', native: true)
math_dep = meson.get_compiler('c').find_library('m')

# ローカルの protocol ディレクトリから XML を指定
gamma_xml = files('protocol/wlr-gamma-control-unstable-v1.xml')

scanner_prog = find_program(wayland_scanner.get_variable('wayland_scanner'))

gamma_header = custom_target(
  'gamma-client-header',
  output: 'wlr-gamma-control-unstable-v1-client-protocol.h',
  input: gamma_xml,
  command: [scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'],
)

gamma_code = custom_target(
  'gamma-client-code',
  output: 'wlr-gamma-control-unstable-v1-protocol.c',
  input: gamma_xml,
  command: [scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable('calib-gamma',
  'calib-gamma.c',
  gamma_header,
  gamma_code,
  dependencies: [wayland_client, math_dep],
  install: false
)
