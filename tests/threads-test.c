/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdatomic.h>

#include "ops.h"

constexpr uint32_t TRANSACTIONS = 20000;
constexpr uint32_t READERS = 4;
constexpr uint32_t SLOTS = 1 << 14;

struct reader {
	pthread_t thread;
	uint64_t seed;
	uint64_t versions;
	uint64_t changes;
	uint32_t slots[SLOTS];
	struct poor_world_diff_match matches[SLOTS];
};

static tree t;
static struct provider p;
static struct reader readers[READERS];
static atomic_bool stop;

static uint64_t
changes_between(struct poor_world_diff_scratch *scratch, const struct item *old, const struct item *next)
{
	struct poor_world_diff diff;
	uint64_t changes = 1;

	assert(old->node.id == next->node.id);
	tree_diff_begin(&diff, scratch, old, next, false);
	while (tree_diff_next(&diff)) {
		const struct item *o = tree_diff_old(&diff), *n = tree_diff_new(&diff);

		if (o != nullptr && n != nullptr && !tree_diff_moved(&diff))
			changes += changes_between(scratch, o, n);
		else
			++changes;
	}
	return changes;
}

static void *
read_versions(void *argument)
{
	struct reader *reader = argument;
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	struct poor_world_pin pin = POOR_WORLD_PIN_INIT;
	struct poor_world_diff_scratch scratch;
	uint64_t epoch = 0;

	tree_diff_scratch_init(&scratch, SLOTS, reader->slots, SLOTS, reader->matches);
	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		const struct snapshot *latest = tree_latest(&t, &hold);
		const struct snapshot *old = tree_pinned(&t, &pin);
		const struct snapshot copy = *latest;

		assert(latest->version.epoch >= epoch && latest->tag == latest->version.epoch);
		assert(latest->version.handle_bound <= SLOTS && latest->version.node_count <= SLOTS / 2);
		reader->versions += latest->version.epoch != epoch;
		epoch = latest->version.epoch;
		validate(latest);
		assert(copy.version.epoch == epoch && copy.version.node_count == latest->version.node_count);
		if (tree_root(latest) != nullptr) {
			const struct item root = *tree_root(latest);

			assert(root.sum == tree_root(&copy)->sum && root.node.id == tree_root(latest)->node.id);
		}
		if (old != nullptr) {
			assert(old->version.epoch <= epoch);
			validate(old);
			if (old != latest && tree_root(old) != nullptr && tree_root(latest) != nullptr
			    && tree_root(old)->node.id == tree_root(latest)->node.id)
				reader->changes += changes_between(&scratch, tree_root(old), tree_root(latest));
			assert(scratch.top == 0);
		}
		reader->seed ^= reader->seed << 13;
		reader->seed ^= reader->seed >> 7;
		reader->seed ^= reader->seed << 17;
		if (reader->seed % 4 == 0)
			tree_pin(&t, &hold, &pin);
		if (reader->seed % 16 == 1)
			tree_release(&t, &hold);
	}
	tree_release(&t, &hold);
	tree_unpin(&t, &pin);
	return nullptr;
}

int
main(void)
{
	uint64_t versions = 0, changes = 0;

	tree_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t i = 0; i < READERS; i++) {
		readers[i].seed = UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
		assert(pthread_create(&readers[i].thread, nullptr, read_versions, &readers[i]) == 0);
	}
	for (uint32_t transaction = 0; transaction < TRANSACTIONS; transaction++) {
		const uint32_t ops = (uint32_t)(1 + rng() % 4);
		struct snapshot *next = tree_begin(&t);

		next->tag = next->version.epoch;
		collect(next);
		for (uint32_t i = 0; i < ops; i++) {
			const struct op op = pick_op(&t, 400);
			uint32_t created;

			while (!try_op(&t, &op, &created))
				assert(provide(&t, &p));
			collect(next);
		}
		if (rng() % 10 == 0) {
			tree_abort(&t);
		} else {
			tree_publish(&t);
		}
		if (rng() % 32 == 0) {
			tree_reclaim(&t);
			drain(&t, &p);
		}
	}
	atomic_store(&stop, true);
	for (uint32_t i = 0; i < READERS; i++) {
		assert(pthread_join(readers[i].thread, nullptr) == 0);
		versions += readers[i].versions;
		changes += readers[i].changes;
	}
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1 && tree_stats(&t).nodes == tree_current(&t)->version.node_count);
	assert(versions > READERS && changes > 0);
	tree_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
	return 0;
}
