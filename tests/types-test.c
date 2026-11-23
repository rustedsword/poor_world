/* SPDX-License-Identifier: MIT */
#ifdef NDEBUG
#error "Tests must not be built with NDEBUG defined, they rely on assert()."
#endif

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static uint64_t fault_countdown;

static bool
fault(void)
{
	return fault_countdown != 0 && --fault_countdown == 0;
}

#define H_WORLD_FAULT() fault()

#include "poor_world.h"

typedef struct {
	char name[40];
	struct poor_world_node hook;
	uint64_t weight;
} node;

typedef struct {
	uint32_t generation;
	struct poor_world_version hook;
} version;

struct leaf {
	struct poor_world_node node;
	uint32_t values[5];
};

struct stamp {
	struct poor_world_version version;
	char note[100];
};

poor_world_declare(world, node, hook, version, hook, 4);
poor_world_declare(shelf, struct leaf, node, struct stamp, version, 3);
poor_world_define(world);
poor_world_define(shelf);

static_assert(world_h_node_header != shelf_h_node_header && world_h_node_children != shelf_h_node_children
	      && world_h_version_bytes != shelf_h_version_bytes && world_h_version_hook != shelf_h_version_hook);

constexpr size_t SLAB = 4096;
constexpr uint32_t DEPTH = 6;
constexpr uint32_t ROUNDS = 400;
constexpr uint32_t SLOTS = 64;
constexpr uint32_t MATCHES = 64;

static void
fill_node(node *item, uint64_t mark)
{
	for (size_t i = 0; i < sizeof(item->name); i++)
		item->name[i] = (char)(mark + i);
	item->weight = mark * 1000 + item->hook.handle;
}

static bool
same_node(const node *item, uint64_t mark)
{
	for (size_t i = 0; i < sizeof(item->name); i++) {
		if (item->name[i] != (char)(mark + i))
			return false;
	}
	return item->weight == mark * 1000 + item->hook.handle;
}

static void
fill_leaf(struct leaf *item, uint64_t mark)
{
	for (uint32_t i = 0; i < 5; i++)
		item->values[i] = (uint32_t)mark * 7 + i + item->node.handle;
}

static bool
same_leaf(const struct leaf *item, uint64_t mark)
{
	for (uint32_t i = 0; i < 5; i++) {
		if (item->values[i] != (uint32_t)mark * 7 + i + item->node.handle)
			return false;
	}
	return true;
}

/*
 * One tree of type 'name': a chain of DEPTH nodes, a spare leaf that moves between them and one extra leaf.
 * Every round changes the deepest node of the chain, with an allocation made to fail on the way.
 * 'deep' is the mark of the deepest node in the published version, 'held' the one in the version the hold has.
 */
#define SCENARIO(name, item_type, hook, version_type, version_hook, fill, same)						\
	struct name##_run {												\
		name tree;												\
		struct poor_world_hold hold;										\
		struct poor_world_diff_scratch scratch;									\
		uint32_t slots[SLOTS];											\
		struct poor_world_diff_match matches[MATCHES];								\
		uint32_t chain[DEPTH];											\
		uint32_t spare;												\
		uint32_t extra;												\
		uint32_t fresh;												\
		uint64_t deep;												\
		uint64_t held;												\
		uint64_t epoch;												\
		uint64_t failures;											\
		size_t fed;												\
	};														\
	static void name##_grab(struct name##_run *run)									\
	{														\
		const size_t bytes = name##_wanted(&run->tree);								\
															\
		++run->failures;											\
		if (bytes != 0) {											\
			void *memory = aligned_alloc(SLAB, bytes);							\
															\
			assert(memory != nullptr);									\
			name##_feed(&run->tree, memory, bytes);								\
			run->fed += bytes;										\
		}													\
	}														\
	static uint32_t name##_add(struct name##_run *run, uint32_t parent, uint32_t index, uint64_t mark)		\
	{														\
		item_type *item;											\
															\
		while ((item = name##_create(&run->tree, parent, index)) == nullptr)					\
			name##_grab(run);										\
		fill(item, mark);											\
		return item->hook.handle;										\
	}														\
	static const item_type *name##_find(const item_type *parent, uint32_t handle)					\
	{														\
		for (uint32_t i = 0; i < parent->hook.child_count; i++) {						\
			const item_type *child = name##_child(parent, i);						\
															\
			if (child->hook.handle == handle)								\
				return child;										\
		}													\
		return nullptr;												\
	}														\
	static void name##_check(const struct name##_run *run, const version_type *snapshot, uint64_t deep)		\
	{														\
		const item_type *item = name##_root(snapshot);								\
															\
		for (uint32_t i = 0; i < DEPTH; i++) {									\
			assert(item != nullptr && item->hook.handle == run->chain[i]);					\
			assert(same(item, i + 1 < DEPTH ? 1 : deep));							\
			if (i + 1 < DEPTH)										\
				item = name##_find(item, run->chain[i + 1]);						\
		}													\
	}														\
	static void name##_start(struct name##_run *run)								\
	{														\
		name##_init(&run->tree, SLAB);										\
		name##_diff_scratch_init(&run->scratch, SLOTS, run->slots, MATCHES, run->matches);			\
		run->hold = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;						\
		run->failures = 0;											\
		run->fed = 0;												\
		name##_begin(&run->tree);										\
		run->chain[0] = name##_add(run, POOR_WORLD_NO_HANDLE, 0, 1);						\
		for (uint32_t i = 1; i < DEPTH; i++)									\
			run->chain[i] = name##_add(run, run->chain[i - 1], 0, 1);					\
		run->spare = name##_add(run, run->chain[0], 1, 1);							\
		run->extra = POOR_WORLD_NO_HANDLE;									\
		run->deep = 1;												\
		run->epoch = 1;												\
		name##_publish(&run->tree);										\
		name##_check(run, name##_current(&run->tree), 1);							\
	}														\
	static void name##_open(struct name##_run *run, uint32_t round)							\
	{														\
		item_type *item;											\
															\
		name##_check(run, name##_latest(&run->tree, &run->hold), run->deep);					\
		run->held = run->deep;											\
		name##_begin(&run->tree);										\
		fault_countdown = round % (DEPTH + 2);									\
		if (round % 2 != 0) {											\
			while (!name##_move(&run->tree, run->spare, run->chain[round % (DEPTH - 1)], 0))		\
				name##_grab(run);									\
		}													\
		while ((item = name##_edit(&run->tree, run->chain[DEPTH - 1])) == nullptr)				\
			name##_grab(run);										\
		fault_countdown = 0;											\
		fill(item, round);											\
	}														\
	static void name##_change(struct name##_run *run, uint32_t round)						\
	{														\
		const uint32_t parent = run->chain[round / 2 % (DEPTH - 1)];						\
															\
		if (run->extra != POOR_WORLD_NO_HANDLE) {								\
			while (!name##_destroy(&run->tree, run->extra))							\
				name##_grab(run);									\
		}													\
		run->fresh = name##_add(run, parent, name##_node(&run->tree, parent)->hook.child_count, round);		\
		fault_countdown = 1 + round % 3;									\
		if (round % 2 == 0) {											\
			while (!name##_move(&run->tree, run->spare, run->chain[round % (DEPTH - 1)], 0))		\
				name##_grab(run);									\
		}													\
		fault_countdown = 0;											\
	}														\
	static void name##_close(struct name##_run *run, uint32_t round)						\
	{														\
		const version_type *held = name##_held(&run->tree, &run->hold);						\
		const version_type *current;										\
		struct poor_world_diff diff;										\
		bool changed = false;											\
															\
		if (round % 3 == 0) {											\
			name##_abort(&run->tree);									\
		} else {												\
			name##_publish(&run->tree);									\
			run->extra = run->fresh;									\
			run->deep = round;										\
			++run->epoch;											\
		}													\
		current = name##_current(&run->tree);									\
		name##_check(run, held, run->held);									\
		name##_check(run, current, run->deep);									\
		assert(current->version_hook.epoch == run->epoch && current->version_hook.handle_bound <= SLOTS);	\
		assert(current->version_hook.node_count == DEPTH + 1 + (run->extra != POOR_WORLD_NO_HANDLE));		\
		assert(same(name##_node(&run->tree, run->spare), 1));							\
		name##_diff_begin(&diff, &run->scratch, name##_root(held), name##_root(current), false);		\
		while (name##_diff_next(&diff)) {									\
			const item_type *was = name##_diff_old(&diff), *is = name##_diff_new(&diff);			\
															\
			assert(was == nullptr || is == nullptr || was->hook.handle == is->hook.handle);			\
			changed = changed || (is != nullptr && is->hook.handle == run->chain[1]);			\
		}													\
		assert(changed == (round % 3 != 0));									\
	}														\
	static void name##_finish(struct name##_run *run)								\
	{														\
		size_t bytes;												\
															\
		name##_release(&run->tree, &run->hold);									\
		name##_reclaim(&run->tree);										\
		assert(name##_stats(&run->tree).versions == 1);								\
		assert(name##_stats(&run->tree).nodes == name##_current(&run->tree)->version_hook.node_count);		\
		assert(run->failures >= ROUNDS / 2);									\
		name##_fini(&run->tree);										\
		for (void *memory; (memory = name##_take(&run->tree, &bytes)) != nullptr; run->fed -= bytes)		\
			free(memory);											\
		assert(run->fed == 0);											\
	}														\
	static_assert(sizeof(struct name##_run) != 0)

SCENARIO(world, node, hook, version, hook, fill_node, same_node);
SCENARIO(shelf, struct leaf, node, struct stamp, version, fill_leaf, same_leaf);

static struct world_run first;
static struct shelf_run second;

int
main(void)
{
	world_start(&first);
	shelf_start(&second);
	for (uint32_t round = 2; round < ROUNDS; round++) {
		world_open(&first, round);
		shelf_open(&second, round + 1);
		shelf_change(&second, round + 1);
		world_change(&first, round);
		world_close(&first, round);
		shelf_close(&second, round + 1);
	}
	world_finish(&first);
	shelf_finish(&second);
	return 0;
}
