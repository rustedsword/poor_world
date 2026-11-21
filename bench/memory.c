/* SPDX-License-Identifier: MIT */
#include "bench.h"

constexpr uint32_t GROUPS = 200;
constexpr uint32_t PER_GROUP = 50;
constexpr uint32_t KEPT_EVERY = 100;
constexpr uint32_t PAGE_HANDLES = 128;
constexpr size_t PAGE_BYTES = 2048;
constexpr size_t SLAB = 1 << 16;

struct item {
	int64_t value;
	int64_t sum;
	struct poor_world_node node;
};

struct snapshot {
	uint64_t tag;
	struct poor_world_version version;
};

poor_world_declare(tree, struct item, node, struct snapshot, version, 16);

static tree t;
static size_t held;
static uint32_t root;
static uint32_t groups[GROUPS];
static uint32_t leaves[GROUPS][PER_GROUP];
static bool page_used[(GROUPS * (PER_GROUP + 1) + 1) / PAGE_HANDLES + 1];

static void
feed(void)
{
	const size_t bytes = poor_world_wanted(&t);
	void *memory = aligned_alloc(SLAB, bytes);

	if (memory == nullptr)
		abort();
	poor_world_feed(&t, memory, bytes);
	held += bytes;
}

static size_t
give_back(void)
{
	size_t bytes;

	for (void *memory; (memory = poor_world_take(&t, &bytes)) != nullptr; held -= bytes)
		free(memory);
	return held;
}

static uint32_t
add(uint32_t parent, uint32_t index)
{
	struct item *item;

	while ((item = poor_world_create(&t, parent, index)) == nullptr)
		feed();
	item->value = item->sum = 0;
	return item->node.handle;
}

static void
drop(uint32_t handle)
{
	while (!poor_world_destroy(&t, handle))
		feed();
}

static void
touch(uint32_t handle)
{
	struct item *item;

	while ((item = poor_world_edit(&t, handle)) == nullptr)
		feed();
	++item->value;
}

static bool
kept(uint32_t group, uint32_t leaf)
{
	return (group * PER_GROUP + leaf) % KEPT_EVERY == 0;
}

static size_t
start(void)
{
	poor_world_init(&t, SLAB)->tag = 0;
	poor_world_begin(&t);
	root = add(POOR_WORLD_NO_HANDLE, 0);
	poor_world_publish(&t);
	return give_back();
}

static size_t
grow(void)
{
	poor_world_begin(&t);
	foreach_array_index(groups, g) {
		groups[g] = add(root, (uint32_t)g);
		foreach_array_index(leaves[g], i)
			leaves[g][i] = add(groups[g], (uint32_t)i);
	}
	poor_world_publish(&t);
	return give_back();
}

static void
finish(void)
{
	poor_world_fini(&t);
	if (give_back() != 0)
		abort();
}

static void
use_page(uint32_t handle, uint32_t *pages)
{
	*pages += !page_used[handle / PAGE_HANDLES];
	page_used[handle / PAGE_HANDLES] = true;
}

static void
show(const char *what, size_t bytes)
{
	printf("%-44s %5zu KiB", what, bytes >> 10);
}

int
main(void)
{
	constexpr uint32_t nodes = GROUPS * (PER_GROUP + 1);
	size_t base, peak;
	uint32_t pages = 0, left = 0;

	printf("%zu-byte nodes in slabs of %zu KiB: %u groups of %u under one root\n", sizeof(struct item), SLAB >> 10, GROUPS,
	       PER_GROUP);

	base = start();
	show("only the root", base);
	peak = grow();
	show("\nall of them", peak);
	printf(", %zu bytes for each of %u nodes", (peak - base) / nodes, nodes);
	poor_world_begin(&t);
	foreach_array_ref(groups, group)
		drop(*group);
	poor_world_publish(&t);
	show("\nall but the root destroyed", give_back());
	poor_world_begin(&t);
	touch(root);
	poor_world_publish(&t);
	show("\nafter the next change to the root", give_back());
	finish();

	start();
	grow();
	poor_world_begin(&t);
	foreach_array_index(groups, g) {
		foreach_array_index(leaves[g], i) {
			if (!kept((uint32_t)g, (uint32_t)i))
				drop(leaves[g][i]);
		}
	}
	poor_world_publish(&t);
	show("\none leaf in 100 left", give_back());
	poor_world_begin(&t);
	foreach_array_ref(groups, group)
		touch(*group);
	use_page(root, &pages);
	foreach_array_index(groups, g) {
		use_page(groups[g], &pages);
		foreach_array_index(leaves[g], i) {
			if (!kept((uint32_t)g, (uint32_t)i))
				continue;
			touch(leaves[g][i]);
			use_page(leaves[g][i], &pages);
			++left;
		}
	}
	poor_world_publish(&t);
	show("\nafter a change to every node that is left", give_back());
	printf(", of which %u pages of handles for %u nodes hold %zu KiB\n", pages, 1 + GROUPS + left,
	       (pages * PAGE_BYTES) >> 10);
	finish();
	return 0;
}
