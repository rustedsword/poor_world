/* SPDX-License-Identifier: MIT */
#include <stdint.h>

static uint64_t fault_countdown;
static uint64_t fault_count;

static bool
fault(void)
{
	if (fault_countdown == 0 || --fault_countdown != 0)
		return false;
	++fault_count;
	return true;
}

#define H_WORLD_FAULT() fault()

#include "ops.h"

constexpr uint32_t STEPS = 30000;

static tree t;
static struct provider p;

static void
close_transaction(bool publish, uint64_t begun)
{
	fault_countdown = rng() % 4;
	if (publish) {
		tree_publish(&t);
		fault_countdown = 0;
		assert(tree_stats(&t).nodes == tree_current(&t)->version.node_count);
	} else {
		tree_abort(&t);
		fault_countdown = 0;
		assert(hash_of(tree_current(&t)) == begun);
	}
	validate(tree_current(&t));
	check_index(&t, tree_current(&t));
	collect(tree_current(&t));
}

static void
check_block_sizes(void)
{
	for (size_t bytes = 1; bytes < 1 << 22; bytes += 1 + bytes / 97) {
		const uint32_t order = h_world_block_order(bytes);

		assert(h_world_block_bytes(order) >= bytes && h_world_block_bytes(order) % H_WORLD_BLOCK_UNIT == 0);
		assert(order == 0 || h_world_block_bytes(order - 1) < bytes);
		assert(h_world_block_bytes(order) - bytes < H_WORLD_BLOCK_UNIT
		       || h_world_block_bytes(order) - bytes <= bytes / 4);
	}
	assert(h_world_block_order((size_t)40 << 30) < H_WORLD_ORDERS);
}

static void
check_failed_first_create(void)
{
	tree_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	tree_begin(&t);
	assert(tree_create(&t, POOR_WORLD_NO_HANDLE, 0) == nullptr && provide(&t, &p));
	fault_countdown = 2;
	assert(tree_create(&t, POOR_WORLD_NO_HANDLE, 0) == nullptr && fault_countdown == 0);
	tree_publish(&t);
	drain(&t, &p);
	assert(p.outstanding == 0 && tree_stats(&t).bytes == 0);
	tree_fini(&t);
}

int
main(void)
{
	uint64_t injected = 0, starved = 0, abandoned = 0, begun = 0;
	bool open = false;

	check_block_sizes();
	check_failed_first_create();

	tree_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t step = 0; step < STEPS; step++) {
		const struct item *root;
		struct op op;
		uint64_t before, nodes;
		uint32_t created = POOR_WORLD_NO_HANDLE;

		if (!open) {
			begun = hash_of(tree_current(&t));
			tree_begin(&t);
			open = true;
		}
		op = pick_op(&t, 300);
		before = hash_of(tree_next(&t));
		root = tree_root(tree_next(&t));
		nodes = tree_stats(&t).nodes;
		for (uint64_t countdown = 1;; countdown++) {
			const uint64_t faults = fault_count;
			bool done;

			fault_countdown = countdown;
			done = try_op(&t, &op, &created);
			fault_countdown = 0;
			if (done) {
				assert(fault_count == faults);
				break;
			}
			assert(hash_of(tree_next(&t)) == before);
			assert(tree_root(tree_next(&t)) == root);
			assert(tree_stats(&t).nodes == nodes);
			validate(tree_next(&t));
			check_index(&t, tree_next(&t));
			if (fault_count == faults) {
				assert(tree_wanted(&t) != 0 && provide(&t, &p));
				++starved;
				--countdown;
				continue;
			}
			++injected;
			if (rng() % 16 == 0) {
				++abandoned;
				break;
			}
		}
		collect(tree_next(&t));
		if (rng() % 8 == 0) {
			close_transaction(rng() % 4 != 0, begun);
			open = false;
		}
		if (rng() % 8 == 0)
			drain(&t, &p);
	}
	if (open)
		close_transaction(true, begun);
	println("injected ", injected, ", starved ", starved, ", abandoned ", abandoned, ", nodes ",
		tree_current(&t)->version.node_count);
	fflush(stdout);
	assert(injected > STEPS && starved > 100);
	tree_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
	return 0;
}
