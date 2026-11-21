world_sources = files('memory.c', 'world.c', 'diff.c')

world = library(
	'poor_world',
	sources: world_sources,
	include_directories: inc,
	version: meson.project_version(),
	soversion: '0',
	dependencies: poor_base_dep,
	install: true
)

poor_world_dep = declare_dependency(
	link_with: world,
	include_directories: inc,
	dependencies: poor_base_dep,
)

pkgconfig.generate(
	world,
	name: 'poor_world',
	description: 'Versioned tree for one writer and many readers, without allocations',
	version: meson.project_version(),
	filebase: 'poor_world',
	requires: [ 'poor_base' ]
)

meson.override_dependency('poor_world', poor_world_dep)

install_headers('../include/poor_world.h')
