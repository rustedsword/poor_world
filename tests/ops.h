/* SPDX-License-Identifier: MIT */
#ifndef TEST_OPS_H
#define TEST_OPS_H

#include "tree.h"

enum op_kind {
	OP_CREATE,
	OP_DESTROY,
	OP_EDIT,
	OP_MOVE,
};

struct op {
	enum op_kind kind;
	uint32_t handle;
	uint32_t parent;
	uint32_t index;
	int64_t value;
};

constexpr uint32_t LIVE_MAX = 1 << 16;

static uint32_t live[LIVE_MAX];
static uint32_t live_count;

static void
collect_node(const struct item *item)
{
	assert(live_count < LIVE_MAX);
	live[live_count++] = item->node.handle;
	for (uint32_t i = 0; i < item->node.child_count; i++)
		collect_node(tree_child(item, i));
}

[[maybe_unused]] static void
collect(const struct snapshot *s)
{
	live_count = 0;
	if (tree_root(s) != nullptr)
		collect_node(tree_root(s));
}

static bool
inside(tree *t, uint32_t handle, uint32_t ancestor)
{
	for (; handle != POOR_WORLD_NO_HANDLE; handle = tree_parent(t, handle)) {
		if (handle == ancestor)
			return true;
	}
	return false;
}

[[maybe_unused]] static struct op
pick_op(tree *t, uint32_t limit)
{
	const uint32_t roll = (uint32_t)(rng() % 100);
	struct op op = { .value = (int64_t)(rng() % 1000) };

	if (live_count == 0) {
		op.kind = OP_CREATE;
		op.parent = POOR_WORLD_NO_HANDLE;
		return op;
	}
	op.handle = live[rng() % live_count];
	op.parent = live[rng() % live_count];
	if (roll < (live_count < limit ? 45u : 15u)) {
		op.kind = OP_CREATE;
		op.index = (uint32_t)(rng() % (tree_node(t, op.parent)->node.child_count + 1));
	} else if (roll < 52 && (tree_parent(t, op.handle) != POOR_WORLD_NO_HANDLE || rng() % 50 == 0)) {
		op.kind = OP_DESTROY;
	} else if (roll < 80 && tree_parent(t, op.handle) != POOR_WORLD_NO_HANDLE && !inside(t, op.parent, op.handle)) {
		const uint32_t count = tree_node(t, op.parent)->node.child_count;

		op.kind = OP_MOVE;
		op.index = (uint32_t)(rng() % (tree_parent(t, op.handle) == op.parent ? count : count + 1));
	} else {
		op.kind = OP_EDIT;
	}
	return op;
}

[[maybe_unused]] static bool
try_op(tree *t, const struct op *op, uint32_t *created)
{
	struct item *item;
	uint32_t origin;
	int64_t sum;

	switch (op->kind) {
	case OP_CREATE:
		item = tree_create(t, op->parent, op->index);
		if (item == nullptr)
			return false;
		item->value = op->value;
		item->sum = 0;
		*created = item->node.handle;
		lift(t, item->node.handle, op->value);
		return true;
	case OP_DESTROY:
		sum = tree_node(t, op->handle)->sum;
		origin = tree_parent(t, op->handle);
		if (!tree_destroy(t, op->handle))
			return false;
		lift(t, origin, -sum);
		return true;
	case OP_EDIT:
		item = tree_edit(t, op->handle);
		if (item == nullptr)
			return false;
		sum = op->value - item->value;
		item->value = op->value;
		lift(t, op->handle, sum);
		return true;
	case OP_MOVE:
		sum = tree_node(t, op->handle)->sum;
		origin = tree_parent(t, op->handle);
		if (!tree_move(t, op->handle, op->parent, op->index))
			return false;
		if (origin != op->parent) {
			lift(t, origin, -sum);
			lift(t, op->parent, sum);
		}
		return true;
	}
	return false;
}

#endif
