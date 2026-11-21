foreach test_name : [ 'world-test', 'memory-test' ]
	test(test_name, executable(test_name, test_name + '.c', dependencies: poor_world_dep))
endforeach

test('model-test', executable('model-test', 'model-test.c', dependencies: poor_world_dep), timeout: 300)

test('threads-test', executable('threads-test', 'threads-test.c', dependencies: [ poor_world_dep, dependency('threads') ]),
     timeout: 120)

test('fault-test', executable('fault-test', 'fault-test.c', world_sources, c_args: '-DH_WORLD_FAULTS',
			      include_directories: inc, dependencies: poor_base_dep))
