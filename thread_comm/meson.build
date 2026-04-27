project('dpa_thread_comm', 'c',
	default_options: ['buildtype=debug', 'c_std=c11'],
	meson_version: '>= 0.61.2'
)

add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language: ['c'])

common_deps = [
	dependency('doca-common'),
	dependency('doca-dpa'),
]

dpa_dep = dependency('doca-dpa')
dpa_lib_dir = dpa_dep.get_variable(pkgconfig : 'libdir')
dpa_program_name = 'dpa_thread_comm_program'
dpa_target_name = 'dpa_thread_comm_dpa'
dpa_device_src = meson.project_source_root() + '/thread_comm_dev.c'
dpa_build_script = meson.project_source_root() + '/build_dpacc.sh'
dpa_mcpu_flag = 'nv-dpa-bf3,nv-dpa-cx7,nv-dpa-cx8'

run_command('bash',
	dpa_build_script,
	meson.project_build_root(),
	dpa_device_src,
	dpa_target_name,
	dpa_program_name,
	dpa_mcpu_flag,
	dpa_lib_dir,
	check: true)

dpa_program_lib_dir = meson.project_build_root() + '/' + dpa_target_name + '/device/build_dpacc'
dpa_program_lib = meson.get_compiler('c').find_library(dpa_program_name,
	dirs: dpa_program_lib_dir,
	static: true)

executable('dpa_thread_comm',
	'main.c',
	dependencies: common_deps + [dependency('libflexio'), dpa_program_lib],
	install: false)
