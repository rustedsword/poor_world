/* SPDX-License-Identifier: MIT */
#include "bench.h"

constexpr uint32_t GROUPS = 200;
constexpr uint32_t PER_GROUP = 50;
constexpr uint32_t KEPT_EVERY = 100;
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
poor_world_define(tree);

static tree t;
static size_t held;
static uint32_t root;
static uint32_t groups[GROUPS];
static uint32_t leaves[GROUPS][PER_GROUP];

static void
feed(void)
{
	const size_t bytes = tree_wanted(&t);
	void *memory = aligned_alloc(SLAB, bytes);

	if (memory == nullptr)
		abort();
	tree_feed(&t, memory, bytes);
	held += bytes;
}

static size_t
give_back(void)
{
	size_t bytes;

	for (void *memory; (memory = tree_take(&t, &bytes)) != nullptr; held -= bytes)
		free(memory);
	return held;
}

static uint32_t
add(uint32_t parent, uint32_t index)
{
	struct item *item;

	while ((item = tree_create(&t, parent, index)) == nullptr)
		feed();
	item->value = item->sum = 0;
	return item->node.handle;
}

static void
drop(uint32_t handle)
{
	while (!tree_destroy(&t, handle))
		feed();
}

static void
touch(uint32_t handle)
{
	struct item *item;

	while ((item = tree_edit(&t, handle)) == nullptr)
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
	tree_init(&t, SLAB)->tag = 0;
	tree_begin(&t);
	root = add(POOR_WORLD_NO_HANDLE, 0);
	tree_publish(&t);
	return give_back();
}

static size_t
grow(void)
{
	tree_begin(&t);
	foreach_array_index(groups, g) {
		groups[g] = add(root, (uint32_t)g);
		foreach_array_index(leaves[g], i)
			leaves[g][i] = add(groups[g], (uint32_t)i);
	}
	tree_publish(&t);
	return give_back();
}

static void
finish(void)
{
	tree_fini(&t);
	if (give_back() != 0)
		abort();
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
	uint32_t left = 0;

	printf("%zu-byte nodes in slabs of %zu KiB: %u groups of %u under one root\n", sizeof(struct item), SLAB >> 10,
	       GROUPS, PER_GROUP);

	base = start();
	show("only the root", base);
	peak = grow();
	show("\nall of them", peak);
	printf(", %zu bytes for each of %u nodes", (peak - base) / nodes, nodes);
	tree_begin(&t);
	foreach_array_ref(groups, group)
		drop(*group);
	tree_publish(&t);
	show("\nall but the root destroyed", give_back());
	tree_begin(&t);
	touch(root);
	tree_publish(&t);
	show("\nafter the next change to the root", give_back());
	finish();

	start();
	grow();
	tree_begin(&t);
	foreach_array_index(groups, g) {
		foreach_array_index(leaves[g], i) {
			if (!kept((uint32_t)g, (uint32_t)i))
				drop(leaves[g][i]);
		}
	}
	tree_publish(&t);
	show("\none leaf in 100 left", give_back());
	tree_begin(&t);
	foreach_array_ref(groups, group)
		touch(*group);
	foreach_array_index(groups, g) {
		foreach_array_index(leaves[g], i) {
			if (!kept((uint32_t)g, (uint32_t)i))
				continue;
			touch(leaves[g][i]);
			++left;
		}
	}
	tree_publish(&t);
	show("\nafter a change to every node that is left", give_back());
	printf(", %u nodes with handles up to %u\n", 1 + GROUPS + left,
	       tree_current(&t)->version.handle_bound);
	finish();
	return 0;
}
