bench_args = [ '-D_GNU_SOURCE' ]

foreach name : [ 'scene', 'memory' ]
	benchmark(name, executable(name, name + '.c', c_args: bench_args, dependencies: poor_world_dep), timeout: 0)
endforeach

benchmark('readers', executable('readers', 'readers.c', c_args: bench_args,
				dependencies: [ poor_world_dep, dependency('threads') ]), timeout: 0)
