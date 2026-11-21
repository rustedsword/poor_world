/* SPDX-License-Identifier: MIT */
#ifndef TEST_TREE_H
#define TEST_TREE_H

#ifdef NDEBUG
#error "Tests must not be built with NDEBUG defined, they rely on assert()."
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <poor_stdio.h>

#include "poor_world.h"

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

#define TREE ((tree *)nullptr)

struct provider {
	size_t outstanding;
	size_t budget;
	uint64_t feeds;
};

#define PROVIDER_INIT { .outstanding = 0, .budget = SIZE_MAX, .feeds = 0 }

[[maybe_unused]] static bool
provide(tree *t, struct provider *p)
{
	const size_t bytes = poor_world_wanted(t);
	void *memory;

	if (bytes > p->budget || p->outstanding > p->budget - bytes)
		return false;
	memory = aligned_alloc(t->world.memory.slab_bytes, bytes);
	assert(memory != nullptr);
	poor_world_feed(t, memory, bytes);
	p->outstanding += bytes;
	++p->feeds;
	return true;
}

[[maybe_unused]] static void
drain(tree *t, struct provider *p)
{
	size_t bytes;

	for (void *memory; (memory = poor_world_take(t, &bytes)) != nullptr;) {
		free(memory);
		p->outstanding -= bytes;
	}
}

[[maybe_unused]] static void
lift(tree *t, uint32_t handle, int64_t delta)
{
	for (; handle != POOR_WORLD_NO_HANDLE; handle = poor_world_parent(t, handle)) {
		struct item *item = poor_world_edit(t, handle);

		assert(item != nullptr);
		item->sum += delta;
	}
}

[[maybe_unused]] static uint32_t
add(tree *t, struct provider *p, uint32_t parent, uint32_t index, int64_t value)
{
	struct item *item;

	while ((item = poor_world_create(t, parent, index)) == nullptr)
		assert(provide(t, p));
	item->value = value;
	item->sum = 0;
	lift(t, item->node.handle, value);
	return item->node.handle;
}

[[maybe_unused]] static void
set(tree *t, struct provider *p, uint32_t handle, int64_t value)
{
	struct item *item;
	int64_t delta;

	while ((item = poor_world_edit(t, handle)) == nullptr)
		assert(provide(t, p));
	delta = value - item->value;
	item->value = value;
	lift(t, handle, delta);
}

[[maybe_unused]] static void
drop(tree *t, struct provider *p, uint32_t handle)
{
	const int64_t sum = poor_world_node(t, handle)->sum;
	const uint32_t parent = poor_world_parent(t, handle);

	while (!poor_world_destroy(t, handle))
		assert(provide(t, p));
	lift(t, parent, -sum);
}

[[maybe_unused]] static void
shift(tree *t, struct provider *p, uint32_t handle, uint32_t parent, uint32_t index)
{
	const int64_t sum = poor_world_node(t, handle)->sum;
	const uint32_t origin = poor_world_parent(t, handle);

	while (!poor_world_move(t, handle, parent, index))
		assert(provide(t, p));
	if (origin != parent) {
		lift(t, origin, -sum);
		lift(t, parent, sum);
	}
}

[[maybe_unused]] static uint32_t
validate_node(const struct snapshot *s, const struct item *item)
{
	int64_t sum = item->value;
	uint32_t count = 1;

	assert(item->node.born <= s->version.epoch && item->node.born != 0);
	assert(item->node.handle < s->version.handle_bound);
	for (uint32_t i = 0; i < item->node.child_count; i++) {
		const struct item *child = poor_world_child(TREE, item, i);

		sum += child->sum;
		count += validate_node(s, child);
	}
	assert(sum == item->sum);
	return count;
}

[[maybe_unused]] static void
validate(const struct snapshot *s)
{
	const struct item *root = poor_world_root(TREE, s);

	assert((root != nullptr ? validate_node(s, root) : 0) == s->version.node_count);
}

[[maybe_unused]] static uint64_t
mix(uint64_t hash, uint64_t value)
{
	return (hash ^ value) * UINT64_C(0x100000001b3);
}

[[maybe_unused]] static uint64_t
hash_node(const struct item *item, uint64_t hash)
{
	hash = mix(hash, item->node.handle);
	hash = mix(hash, item->node.id);
	hash = mix(hash, (uint64_t)item->value);
	hash = mix(hash, (uint64_t)item->sum);
	hash = mix(hash, item->node.child_count);
	for (uint32_t i = 0; i < item->node.child_count; i++)
		hash = hash_node(poor_world_child(TREE, item, i), hash);
	return hash;
}

[[maybe_unused]] static uint64_t
hash_of(const struct snapshot *s)
{
	const struct item *root = poor_world_root(TREE, s);

	return root != nullptr ? hash_node(root, UINT64_C(0xcbf29ce484222325)) : 0;
}

[[maybe_unused]] static void
check_index_node(tree *t, const struct item *item, uint32_t parent, uint32_t index)
{
	assert(poor_world_node(t, item->node.handle) == item);
	assert(poor_world_parent(t, item->node.handle) == parent);
	assert(poor_world_index(t, item->node.handle) == index);
	for (uint32_t i = 0; i < item->node.child_count; i++)
		check_index_node(t, poor_world_child(TREE, item, i), item->node.handle, i);
}

[[maybe_unused]] static void
check_index(tree *t, const struct snapshot *s)
{
	const struct item *root = poor_world_root(TREE, s);

	if (root != nullptr)
		check_index_node(t, root, POOR_WORLD_NO_HANDLE, 0);
}

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

[[maybe_unused]] static uint64_t
rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

#define RUN(test) (println(#test), fflush(stdout), test())

#endif
