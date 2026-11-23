foreach test_name : [ 'world-test', 'memory-test', 'types-test' ]
	test(test_name, executable(test_name, test_name + '.c', dependencies: poor_world_dep))
endforeach

test('model-test', executable('model-test', 'model-test.c', dependencies: poor_world_dep), timeout: 300)

test('threads-test', executable('threads-test', 'threads-test.c', dependencies: [ poor_world_dep, dependency('threads') ]),
     timeout: 120)

test('fault-test', executable('fault-test', 'fault-test.c', dependencies: poor_world_dep))
