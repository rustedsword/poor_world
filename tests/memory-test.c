/* SPDX-License-Identifier: MIT */
#include "tree.h"

constexpr uint32_t GROUPS = 200;
constexpr uint32_t PER_GROUP = 50;
constexpr size_t SLAB = 1 << 16;

static tree t;
static struct provider p;
static uint32_t root;
static uint32_t groups[GROUPS];
static uint32_t leaves[GROUPS][PER_GROUP];

static void
start(size_t slab)
{
	tree_init(&t, slab)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	tree_publish(&t);
	drain(&t, &p);
}

static void
finish(void)
{
	tree_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
}

static void
grow(void)
{
	tree_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++) {
		groups[g] = add(&t, &p, root, g, 1);
		for (uint32_t i = 0; i < PER_GROUP; i++)
			leaves[g][i] = add(&t, &p, groups[g], i, 1);
	}
	tree_publish(&t);
	validate(tree_current(&t));
}

static void
shrink(void)
{
	tree_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++)
		drop(&t, &p, groups[g]);
	tree_publish(&t);
}

static void
settle(void)
{
	tree_begin(&t);
	set(&t, &p, root, 0);
	tree_publish(&t);
	drain(&t, &p);
}

static void
memory_comes_back(void)
{
	size_t base, peak, after = 0;

	start(SLAB);
	base = p.outstanding;
	assert(base > 0 && tree_stats(&t).bytes == base && tree_stats(&t).idle_bytes == 0);
	for (int cycle = 0; cycle < 4; cycle++) {
		grow();
		drain(&t, &p);
		peak = p.outstanding;
		assert(peak >= base + (size_t)GROUPS * PER_GROUP * sizeof(struct item));
		assert(tree_current(&t)->version.handle_bound >= GROUPS * (PER_GROUP + 1));
		shrink();
		assert(tree_stats(&t).idle_bytes >= peak - base - SLAB);
		drain(&t, &p);
		assert(p.outstanding <= base + SLAB && tree_stats(&t).bytes == p.outstanding);
		assert(cycle == 0 || p.outstanding == after);
		after = p.outstanding;
		assert(tree_current(&t)->version.handle_bound == 1);
		settle();
		assert(p.outstanding == base);
	}
	finish();
}

static void
an_abandoned_create_keeps_nothing(void)
{
	tree_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t slabs = 0; slabs < 8; slabs++) {
		struct item *item;

		tree_begin(&t);
		for (uint32_t i = 0; i < slabs; i++) {
			item = tree_create(&t, POOR_WORLD_NO_HANDLE, 0);
			if (item != nullptr) {
				item->value = item->sum = 0;
				break;
			}
			assert(provide(&t, &p));
		}
		item = tree_create(&t, tree_root(tree_next(&t)) != nullptr ? 0 : POOR_WORLD_NO_HANDLE, 0);
		if (item != nullptr)
			item->value = item->sum = 0;
		tree_abort(&t);
		drain(&t, &p);
		assert(p.outstanding == 0 && tree_stats(&t).bytes == 0 && tree_stats(&t).nodes == 0);
	}
	finish();
}

static void
held_versions_keep_their_memory(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	size_t base, peak;

	start(SLAB);
	base = p.outstanding;
	grow();
	drain(&t, &p);
	peak = p.outstanding;
	validate(tree_latest(&t, &hold));
	shrink();
	drain(&t, &p);
	assert(p.outstanding > (base + peak) / 2);
	assert(tree_stats(&t).versions == 2);
	validate(tree_held(&t, &hold));
	tree_release(&t, &hold);
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1);
	drain(&t, &p);
	assert(p.outstanding <= base + SLAB);
	finish();
}

static void
idle_memory_is_used_again(void)
{
	uint64_t feeds;

	start(SLAB);
	grow();
	shrink();
	feeds = p.feeds;
	grow();
	assert(p.feeds == feeds);
	finish();
}

static void
fini_gives_everything_back(void)
{
	size_t bytes = 0, taken;

	start(4096);
	grow();
	tree_begin(&t);
	for (uint32_t i = 0; i < 3000; i++)
		add(&t, &p, groups[0], 0, 1);
	tree_publish(&t);
	assert(tree_stats(&t).idle_bytes < p.outstanding / 2);
	tree_fini(&t);
	assert(tree_stats(&t).idle_bytes == p.outstanding);
	for (void *memory; (memory = tree_take(&t, &taken)) != nullptr; free(memory)) {
		assert(taken % 4096 == 0);
		bytes += taken;
	}
	assert(bytes == p.outstanding && tree_stats(&t).bytes == 0);
}

static void
a_wide_node_asks_for_one_large_span(void)
{
	struct item *item;
	size_t largest = 0;
	uint32_t parent;

	start(4096);
	tree_begin(&t);
	parent = add(&t, &p, root, 0, 0);
	for (uint32_t i = 0; i < 5000; i++) {
		while ((item = tree_create(&t, parent, i)) == nullptr) {
			if (tree_wanted(&t) > largest)
				largest = tree_wanted(&t);
			assert(tree_wanted(&t) % 4096 == 0);
			assert(provide(&t, &p));
		}
		item->value = 0;
		item->sum = 0;
	}
	tree_publish(&t);
	assert(largest >= 5000 * sizeof(void *) / 2 && largest > 4096);
	tree_begin(&t);
	drop(&t, &p, parent);
	tree_publish(&t);
	drain(&t, &p);
	assert(p.outstanding <= 3 * 4096);
	finish();
}

static void
the_handle_table_fits_its_slabs(void)
{
	struct item *item;
	uint32_t last = root;

	start(4096);
	tree_begin(&t);
	for (uint32_t i = 0; i < 220; i++) {
		while ((item = tree_create(&t, last, 0)) == nullptr) {
			assert(tree_wanted(&t) == 4096);
			assert(provide(&t, &p));
		}
		item->value = item->sum = 0;
		last = item->node.handle;
	}
	tree_publish(&t);
	assert(tree_current(&t)->version.handle_bound == 221);
	finish();
}

static void
touching_survivors_packs_them(void)
{
	size_t sparse;

	start(SLAB);
	grow();
	tree_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++) {
		for (uint32_t i = 0; i < PER_GROUP; i++) {
			if ((g * PER_GROUP + i) % 100 != 0)
				drop(&t, &p, leaves[g][i]);
		}
	}
	tree_publish(&t);
	drain(&t, &p);
	sparse = p.outstanding;

	tree_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++)
		set(&t, &p, groups[g], 2);
	for (uint32_t g = 0; g < GROUPS; g += 2)
		set(&t, &p, leaves[g][0], 2);
	tree_publish(&t);
	drain(&t, &p);
	assert(p.outstanding < sparse);
	validate(tree_current(&t));
	finish();
}

static size_t
in_use(void)
{
	const struct poor_world_stats stats = tree_stats(&t);

	return stats.bytes - stats.idle_bytes;
}

enum reclaimed { AT_PUBLISH, BY_RECLAIM, IN_A_PUBLISHED_TRANSACTION, IN_AN_ABORTED_TRANSACTION };

static void
shrink_the_handle_table(enum reclaimed when, uint32_t bound)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	const size_t table = (size_t)GROUPS * PER_GROUP * 16;
	uint32_t first = POOR_WORLD_NO_HANDLE;
	size_t small;

	start(4096);
	tree_begin(&t);
	groups[0] = add(&t, &p, root, 0, 1);
	for (uint32_t i = 2; i < bound; i++)
		first = add(&t, &p, groups[0], 0, 1);
	tree_publish(&t);
	assert(tree_current(&t)->version.handle_bound == bound);
	drain(&t, &p);
	small = p.outstanding;

	tree_begin(&t);
	for (uint32_t g = 1; g < GROUPS; g++) {
		groups[g] = add(&t, &p, root, g, 1);
		for (uint32_t i = 0; i < PER_GROUP; i++)
			leaves[g][i] = add(&t, &p, groups[g], i, 1);
	}
	tree_publish(&t);
	drain(&t, &p);
	assert(p.outstanding > small + table);
	if (when != AT_PUBLISH)
		tree_latest(&t, &hold);

	tree_begin(&t);
	for (uint32_t g = 1; g < GROUPS; g++)
		drop(&t, &p, groups[g]);
	tree_publish(&t);
	if (when != AT_PUBLISH) {
		assert(in_use() > table);
		if (when != BY_RECLAIM) {
			tree_begin(&t);
			set(&t, &p, first, 2);
			add(&t, &p, groups[0], 0, 3);
		}
		tree_release(&t, &hold);
		tree_reclaim(&t);
	}
	assert(in_use() < table);
	if (when == IN_A_PUBLISHED_TRANSACTION)
		tree_publish(&t);
	if (when == IN_AN_ABORTED_TRANSACTION)
		tree_abort(&t);
	validate(tree_current(&t));
	drain(&t, &p);
	assert(p.outstanding <= small + 4096);
	finish();
}

static void
the_handle_table_shrinks(void)
{
	shrink_the_handle_table(AT_PUBLISH, 52);
}

static void
the_handle_table_shrinks_after_a_reader_lets_go(void)
{
	shrink_the_handle_table(BY_RECLAIM, 52);
}

static void
the_handle_table_shrinks_inside_a_transaction(void)
{
	shrink_the_handle_table(IN_A_PUBLISHED_TRANSACTION, 52);
	shrink_the_handle_table(IN_AN_ABORTED_TRANSACTION, 52);
}

static void
a_shrunk_handle_table_fits_its_slab(void)
{
	shrink_the_handle_table(AT_PUBLISH, 113);
	shrink_the_handle_table(BY_RECLAIM, 120);
	shrink_the_handle_table(AT_PUBLISH, 128);
}

int
main(void)
{
	RUN(memory_comes_back);
	RUN(an_abandoned_create_keeps_nothing);
	RUN(held_versions_keep_their_memory);
	RUN(idle_memory_is_used_again);
	RUN(fini_gives_everything_back);
	RUN(a_wide_node_asks_for_one_large_span);
	RUN(the_handle_table_fits_its_slabs);
	RUN(the_handle_table_shrinks);
	RUN(the_handle_table_shrinks_after_a_reader_lets_go);
	RUN(the_handle_table_shrinks_inside_a_transaction);
	RUN(a_shrunk_handle_table_fits_its_slab);
	RUN(touching_survivors_packs_them);
	return 0;
}
