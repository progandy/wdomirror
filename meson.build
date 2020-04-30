project(
	'wdomirror',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.54.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wno-missing-braces',
	'-Wno-unused-parameter',
]), language: 'c')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.17')

subdir('protocol')

mirror_files = [ 'main.c' ]
mirror_deps = [
	wayland_client,
	wayland_protos,
	client_protos,
]


proto_inc = include_directories('protocol')

executable(
  meson.project_name(),
  mirror_files,
  dependencies: mirror_deps,
	include_directories: proto_inc,
  install: true
)
