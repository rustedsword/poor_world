project(
	'poor_world', 'c',
	version: '0.1.0',
	license: 'MIT',
	license_files: 'LICENSE',
	meson_version: '>= 1.4.0',
	default_options: [
		'warning_level=2',
		'buildtype=debugoptimized',
		'c_std=c23',
	]
)

pkgconfig = import('pkgconfig')

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wstrict-prototypes',
		'-Wmissing-prototypes',
		'-Wundef',
		'-fvisibility=hidden',
		# Array views of no elements are variable length arrays of zero length
		'-fno-sanitize=vla-bound',
	]),
	language: 'c'
)

# An installed poor_base is used if it is new enough to have poor_fan.h, the submodule otherwise
if cc.has_header('poor_fan.h')
	poor_base_dep = dependency('poor_base')
else
	poor_base_dep = subproject('poor_base').get_variable('poor_base_dep')
endif

inc = include_directories('include')

subdir('src')

if get_option('tests')
	subdir('tests')
endif

if get_option('bench')
	subdir('bench')
endif

if get_option('examples')
	subdir('examples')
endif
