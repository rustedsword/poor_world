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
	poor_world_init(&t, slab)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	poor_world_publish(&t);
	drain(&t, &p);
}

static void
finish(void)
{
	poor_world_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
}

static void
grow(void)
{
	poor_world_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++) {
		groups[g] = add(&t, &p, root, g, 1);
		for (uint32_t i = 0; i < PER_GROUP; i++)
			leaves[g][i] = add(&t, &p, groups[g], i, 1);
	}
	poor_world_publish(&t);
	validate(poor_world_current(&t));
}

static void
shrink(void)
{
	poor_world_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++)
		drop(&t, &p, groups[g]);
	poor_world_publish(&t);
}

static void
settle(void)
{
	poor_world_begin(&t);
	set(&t, &p, root, 0);
	poor_world_publish(&t);
	drain(&t, &p);
}

static void
memory_comes_back(void)
{
	size_t base, peak, after = 0;

	start(SLAB);
	base = p.outstanding;
	assert(base > 0 && poor_world_stats(&t).bytes == base && poor_world_stats(&t).idle_bytes == 0);
	for (int cycle = 0; cycle < 4; cycle++) {
		grow();
		drain(&t, &p);
		peak = p.outstanding;
		assert(peak >= base + (size_t)GROUPS * PER_GROUP * sizeof(struct item));
		assert(poor_world_current(&t)->version.handle_bound >= GROUPS * (PER_GROUP + 1));
		shrink();
		assert(poor_world_stats(&t).idle_bytes >= peak - base - SLAB);
		drain(&t, &p);
		assert(p.outstanding <= base + SLAB && poor_world_stats(&t).bytes == p.outstanding);
		assert(cycle == 0 || p.outstanding == after);
		after = p.outstanding;
		assert(poor_world_current(&t)->version.handle_bound == 128);
		settle();
		assert(p.outstanding == base);
	}
	finish();
}

static void
an_abandoned_create_keeps_nothing(void)
{
	poor_world_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t slabs = 0; slabs < 8; slabs++) {
		struct item *item;

		poor_world_begin(&t);
		for (uint32_t i = 0; i < slabs; i++) {
			item = poor_world_create(&t, POOR_WORLD_NO_HANDLE, 0);
			if (item != nullptr) {
				item->value = item->sum = 0;
				break;
			}
			assert(provide(&t, &p));
		}
		item = poor_world_create(&t, poor_world_root(&t, poor_world_next(&t)) != nullptr ? 0 : POOR_WORLD_NO_HANDLE, 0);
		if (item != nullptr)
			item->value = item->sum = 0;
		poor_world_abort(&t);
		drain(&t, &p);
		assert(p.outstanding == 0 && poor_world_stats(&t).bytes == 0 && poor_world_stats(&t).nodes == 0);
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
	validate(poor_world_latest(&t, &hold));
	shrink();
	drain(&t, &p);
	assert(p.outstanding > (base + peak) / 2);
	validate(poor_world_held(&t, &hold));
	poor_world_release(&t, &hold);
	poor_world_reclaim(&t);
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
	poor_world_begin(&t);
	for (uint32_t i = 0; i < 3000; i++)
		add(&t, &p, groups[0], 0, 1);
	poor_world_publish(&t);
	assert(poor_world_stats(&t).idle_bytes < p.outstanding / 2);
	poor_world_fini(&t);
	assert(poor_world_stats(&t).idle_bytes == p.outstanding);
	for (void *memory; (memory = poor_world_take(&t, &taken)) != nullptr; free(memory)) {
		assert(taken % 4096 == 0);
		bytes += taken;
	}
	assert(bytes == p.outstanding && poor_world_stats(&t).bytes == 0);
}

static void
a_wide_node_asks_for_one_large_span(void)
{
	struct item *item;
	size_t largest = 0;
	uint32_t parent;

	start(4096);
	poor_world_begin(&t);
	parent = add(&t, &p, root, 0, 0);
	for (uint32_t i = 0; i < 5000; i++) {
		while ((item = poor_world_create(&t, parent, i)) == nullptr) {
			if (poor_world_wanted(&t) > largest)
				largest = poor_world_wanted(&t);
			assert(poor_world_wanted(&t) % 4096 == 0);
			assert(provide(&t, &p));
		}
		item->value = 0;
		item->sum = 0;
	}
	poor_world_publish(&t);
	assert(largest >= 5000 * sizeof(void *) / 2 && largest > 4096);
	poor_world_begin(&t);
	drop(&t, &p, parent);
	poor_world_publish(&t);
	drain(&t, &p);
	assert(p.outstanding <= 3 * 4096);
	finish();
}

static void
touching_survivors_packs_them(void)
{
	size_t sparse;
	uint32_t pages = 0;

	start(SLAB);
	grow();
	poor_world_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++) {
		for (uint32_t i = 0; i < PER_GROUP; i++) {
			if ((g * PER_GROUP + i) % 100 != 0)
				drop(&t, &p, leaves[g][i]);
		}
	}
	poor_world_publish(&t);
	drain(&t, &p);
	sparse = p.outstanding;

	poor_world_begin(&t);
	for (uint32_t g = 0; g < GROUPS; g++)
		set(&t, &p, groups[g], 2);
	for (uint32_t g = 0; g < GROUPS; g += 2)
		set(&t, &p, leaves[g][0], 2);
	poor_world_publish(&t);
	drain(&t, &p);
	for (uint32_t page = 0; page < t.world.index.page_count; page++)
		pages += t.world.index.pages[page].entries != nullptr;
	assert(p.outstanding < sparse && pages == 80);
	validate(poor_world_current(&t));
	finish();
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
	RUN(touching_survivors_packs_them);
	return 0;
}
