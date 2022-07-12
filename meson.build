project(
  'wljoywake',
  'c',
  version: '0.1.0',
  license: 'GPLv2',
  meson_version: '>=0.58.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    ],
  )

add_project_arguments(
  [
    '-Wl,--exclude-libs=ALL',
    '-DWLJOYWAKE_VERSION="@0@"'.format(meson.project_version()),
    ],
  language: 'c',
  )

scanner = find_program('wayland-scanner')
scanner_private_code = generator(scanner, output: '@BASENAME@-protocol.c', arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
scanner_client_header = generator(scanner, output: '@BASENAME@-client-protocol.h', arguments: ['client-header', '@INPUT@', '@OUTPUT@'])

wl_protocols = dependency('wayland-protocols')
wl_protocol_dir = wl_protocols.get_variable('pkgdatadir')

protocols_src = [scanner_private_code.process(wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml')]
protocols_headers = [scanner_client_header.process(wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml')]

wl_client = dependency('wayland-client')
lib_protocols = static_library('protocols', protocols_src + protocols_headers, dependencies: wl_client)
protocols_dep = declare_dependency(link_with: lib_protocols, sources: protocols_headers)

udev = dependency('libudev')

executable(
  'wljoywake',
  ['main.c'],
  dependencies: [wl_client, udev, protocols_dep],
  install: true,
  )
