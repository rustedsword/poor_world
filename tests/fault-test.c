/* SPDX-License-Identifier: MIT */
#include "ops.h"

#include "../src/internal.h"

constexpr uint32_t STEPS = 30000;

static tree t;
static struct provider p;

static void
close_transaction(bool publish, uint64_t begun)
{
	h_world_fault_countdown = rng() % 4;
	if (publish) {
		poor_world_publish(&t);
		h_world_fault_countdown = 0;
		assert(poor_world_stats(&t).nodes == poor_world_current(&t)->version.node_count);
	} else {
		poor_world_abort(&t);
		h_world_fault_countdown = 0;
		assert(hash_of(poor_world_current(&t)) == begun);
	}
	validate(poor_world_current(&t));
	check_index(&t, poor_world_current(&t));
	collect(poor_world_current(&t));
}

static void
check_block_sizes(void)
{
	for (size_t bytes = 1; bytes < 1 << 22; bytes += 1 + bytes / 97) {
		const uint32_t order = block_order(bytes);

		assert(block_bytes(order) >= bytes && block_bytes(order) % BLOCK_UNIT == 0);
		assert(order == 0 || block_bytes(order - 1) < bytes);
		assert(block_bytes(order) - bytes < BLOCK_UNIT || block_bytes(order) - bytes <= bytes / 4);
	}
	assert(block_order((size_t)40 << 30) < H_WORLD_ORDERS);
}

int
main(void)
{
	uint64_t injected = 0, starved = 0, abandoned = 0, begun = 0;
	bool open = false;

	check_block_sizes();

	poor_world_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t step = 0; step < STEPS; step++) {
		const struct item *root;
		struct op op;
		uint64_t before, nodes;
		uint32_t created = POOR_WORLD_NO_HANDLE;

		if (!open) {
			begun = hash_of(poor_world_current(&t));
			poor_world_begin(&t);
			open = true;
		}
		op = pick_op(&t, 300);
		before = hash_of(poor_world_next(&t));
		root = poor_world_root(TREE, poor_world_next(&t));
		nodes = poor_world_stats(&t).nodes;
		for (uint64_t countdown = 1;; countdown++) {
			const uint64_t faults = h_world_faults;
			bool done;

			h_world_fault_countdown = countdown;
			done = try_op(&t, &op, &created);
			h_world_fault_countdown = 0;
			if (done) {
				assert(h_world_faults == faults);
				break;
			}
			assert(hash_of(poor_world_next(&t)) == before);
			assert(poor_world_root(TREE, poor_world_next(&t)) == root);
			assert(poor_world_stats(&t).nodes == nodes);
			validate(poor_world_next(&t));
			check_index(&t, poor_world_next(&t));
			if (h_world_faults == faults) {
				assert(poor_world_wanted(&t) != 0 && provide(&t, &p));
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
		collect(poor_world_next(&t));
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
		poor_world_current(&t)->version.node_count);
	fflush(stdout);
	assert(injected > STEPS && starved > 100);
	poor_world_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
	return 0;
}
