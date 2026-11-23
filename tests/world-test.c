/* SPDX-License-Identifier: MIT */
#include "tree.h"

static tree t;
static struct provider p;

static void
start(size_t slab)
{
	tree_init(&t, slab)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
}

static void
finish(void)
{
	tree_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
}

static void
commit(void)
{
	validate(tree_next(&t));
	check_index(&t, tree_next(&t));
	tree_publish(&t);
	assert(tree_stats(&t).versions == 1);
	assert(tree_stats(&t).nodes == tree_current(&t)->version.node_count);
}

static void
empty_world(void)
{
	start(4096);
	assert(tree_current(&t)->version.epoch == 0);
	assert(tree_root(tree_current(&t)) == nullptr);
	assert(tree_stats(&t).bytes == 0 && tree_stats(&t).nodes == 0);
	tree_begin(&t)->tag = 7;
	tree_publish(&t);
	assert(tree_current(&t)->version.epoch == 1 && tree_current(&t)->tag == 7);
	assert(tree_node(&t, 0) == nullptr && tree_node(&t, POOR_WORLD_NO_HANDLE) == nullptr);
	finish();
}

static void
init_does_not_need_clean_memory(void)
{
	static tree s;
	struct provider sp = PROVIDER_INIT;
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;

	memset(&s, 0xa5, sizeof(s));
	tree_init(&s, 4096)->tag = 0;
	assert(tree_latest(&s, &hold)->version.epoch == 0 && tree_stats(&s).bytes == 0);
	tree_begin(&s);
	add(&s, &sp, POOR_WORLD_NO_HANDLE, 0, 5);
	tree_publish(&s);
	assert(tree_root(tree_latest(&s, &hold))->sum == 5);
	tree_release(&s, &hold);
	tree_fini(&s);
	drain(&s, &sp);
	assert(sp.outstanding == 0);
}

static void
children_keep_their_order(void)
{
	uint32_t root, a, b, c, d;
	const struct item *r;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 1);
	a = add(&t, &p, root, 0, 10);
	b = add(&t, &p, root, 1, 20);
	c = add(&t, &p, root, 2, 30);
	d = add(&t, &p, root, 1, 40);
	commit();

	r = tree_root(tree_current(&t));
	assert(r->node.child_count == 4 && r->sum == 101);
	assert(tree_child(r, 0)->node.handle == a && tree_child(r, 1)->node.handle == d);
	assert(tree_child(r, 2)->node.handle == b && tree_child(r, 3)->node.handle == c);
	assert(tree_index(&t, d) == 1 && tree_index(&t, c) == 3 && tree_parent(&t, d) == root);
	assert(tree_parent(&t, root) == POOR_WORLD_NO_HANDLE);
	assert(tree_node(&t, b)->value == 20);
	assert(tree_child(r, 0)->node.id != tree_child(r, 1)->node.id);
	assert(tree_current(&t)->version.node_count == 5);
	finish();
}

static void
readers_keep_what_they_latched(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT, newer = POOR_WORLD_HOLD_INIT;
	const struct snapshot *before, *after;
	uint32_t root, a, b, leaf;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	leaf = add(&t, &p, a, 0, 3);
	commit();

	before = tree_latest(&t, &hold);
	assert(tree_held(&t, &hold) == before);
	tree_begin(&t);
	set(&t, &p, leaf, 100);
	tree_publish(&t);
	assert(tree_latest(&t, &hold) == tree_current(&t));
	tree_release(&t, &hold);
	tree_reclaim(&t);

	before = tree_latest(&t, &hold);
	tree_begin(&t);
	set(&t, &p, leaf, 200);
	tree_publish(&t);
	after = tree_latest(&t, &newer);
	assert(before != after && before->version.epoch + 1 == after->version.epoch);
	assert(tree_child(tree_child(tree_root(before), 0), 0)->value == 100);
	assert(tree_child(tree_child(tree_root(after), 0), 0)->value == 200);
	assert(tree_root(before) != tree_root(after));
	assert(tree_child(tree_root(before), 1) == tree_child(tree_root(after), 1));
	assert(tree_child(tree_root(after), 1)->node.handle == b);
	validate(before);
	validate(after);
	assert(tree_stats(&t).versions == 2 && tree_stats(&t).nodes == 4 + 3);
	assert(tree_oldest_held(&t) == before->version.epoch);

	tree_release(&t, &hold);
	assert(tree_oldest_held(&t) == after->version.epoch);
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1 && tree_stats(&t).nodes == 4);
	tree_release(&t, &newer);
	finish();
}

static void
destroyed_handles_come_back(void)
{
	uint32_t root, a, b, again;
	uint64_t id;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	add(&t, &p, a, 0, 2);
	b = add(&t, &p, a, 1, 3);
	add(&t, &p, b, 0, 4);
	commit();
	id = tree_node(&t, b)->node.id;

	tree_begin(&t);
	drop(&t, &p, a);
	assert(tree_node(&t, a) == nullptr && tree_node(&t, b) == nullptr);
	assert(tree_next(&t)->version.node_count == 1);
	commit();
	assert(tree_root(tree_current(&t))->sum == 0);

	tree_begin(&t);
	again = add(&t, &p, root, 0, 9);
	assert(again == a || again == b || again < 5);
	assert(tree_node(&t, again)->node.id > id);
	commit();

	tree_begin(&t);
	drop(&t, &p, root);
	assert(tree_root(tree_next(&t)) == nullptr);
	commit();
	assert(tree_stats(&t).nodes == 0);
	assert(tree_current(&t)->version.handle_bound == 0);
	finish();
}

static void
nodes_move(void)
{
	uint32_t root, a, b, x, y, z;
	const struct item *r;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	x = add(&t, &p, a, 0, 10);
	y = add(&t, &p, a, 1, 20);
	z = add(&t, &p, a, 2, 30);
	add(&t, &p, y, 0, 5);
	commit();

	tree_begin(&t);
	shift(&t, &p, z, a, 0);
	shift(&t, &p, z, a, 0);
	commit();
	r = tree_child(tree_root(tree_current(&t)), 0);
	assert(tree_child(r, 0)->node.handle == z && tree_child(r, 1)->node.handle == x);
	assert(tree_child(r, 2)->node.handle == y);

	tree_begin(&t);
	shift(&t, &p, x, a, 2);
	shift(&t, &p, y, b, 0);
	assert(tree_parent(&t, y) == b && tree_index(&t, y) == 0);
	commit();
	r = tree_root(tree_current(&t));
	assert(tree_child(r, 0)->sum == 1 + 30 + 10 && tree_child(r, 1)->sum == 2 + 20 + 5);
	assert(tree_child(tree_child(r, 0), 1)->node.handle == x);

	tree_begin(&t);
	shift(&t, &p, y, root, 0);
	shift(&t, &p, a, y, 1);
	commit();
	r = tree_root(tree_current(&t));
	assert(r->node.child_count == 2 && tree_child(r, 0)->node.handle == y);
	assert(tree_child(tree_child(r, 0), 1)->node.handle == a && r->sum == 68);
	finish();
}

static void
abort_restores_everything(void)
{
	uint32_t root, a, b, x, y, grown;
	uint64_t hash, nodes;
	size_t bound;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	x = add(&t, &p, a, 0, 10);
	y = add(&t, &p, a, 1, 20);
	add(&t, &p, y, 0, 5);
	commit();
	hash = hash_of(tree_current(&t));
	nodes = tree_stats(&t).nodes;
	bound = tree_current(&t)->version.handle_bound;

	tree_begin(&t)->tag = 99;
	set(&t, &p, x, 11);
	grown = add(&t, &p, b, 0, 7);
	for (int i = 0; i < 300; i++)
		add(&t, &p, grown, 0, i);
	drop(&t, &p, grown);
	grown = add(&t, &p, root, 2, 8);
	shift(&t, &p, y, b, 0);
	shift(&t, &p, x, root, 0);
	drop(&t, &p, a);
	shift(&t, &p, b, grown, 0);
	validate(tree_next(&t));
	check_index(&t, tree_next(&t));
	tree_abort(&t);

	assert(tree_current(&t)->tag == 0);
	assert(hash_of(tree_current(&t)) == hash);
	validate(tree_current(&t));
	check_index(&t, tree_current(&t));
	assert(tree_stats(&t).nodes == nodes && tree_stats(&t).versions == 1);
	assert(tree_node(&t, grown) == nullptr);

	tree_begin(&t);
	set(&t, &p, y, 21);
	add(&t, &p, x, 0, 3);
	commit();
	assert(tree_root(tree_current(&t))->sum == 1 + 2 + 10 + 21 + 5 + 3);
	assert(tree_current(&t)->version.handle_bound == bound + 1);
	finish();
}

static void
versions_die_in_any_order(void)
{
	static constexpr uint32_t order[] = { 2, 0, 4, 1, 3 };
	struct poor_world_hold holds[5];
	struct poor_world_pin pins[5];
	uint64_t hashes[5];
	uint32_t root, kids[10];

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	for (uint32_t i = 0; i < 10; i++)
		kids[i] = add(&t, &p, root, i, i);
	commit();

	for (uint32_t i = 0; i < 5; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		pins[i] = (struct poor_world_pin)POOR_WORLD_PIN_INIT;
		hashes[i] = hash_of(tree_latest(&t, &holds[i]));
		tree_pin(&t, &holds[i], &pins[i]);
		tree_release(&t, &holds[i]);
		tree_begin(&t);
		set(&t, &p, kids[i], 100 + i);
		drop(&t, &p, kids[9 - i]);
		kids[9 - i] = add(&t, &p, kids[i], 0, 50);
		tree_publish(&t);
	}
	assert(tree_stats(&t).versions == 6);
	for (uint32_t i = 0; i < 5; i++) {
		for (uint32_t j = i; j < 5; j++) {
			assert(hash_of(tree_pinned(&t, &pins[order[j]])) == hashes[order[j]]);
			validate(tree_pinned(&t, &pins[order[j]]));
		}
		tree_unpin(&t, &pins[order[i]]);
		assert(tree_pinned(&t, &pins[order[i]]) == nullptr);
		tree_reclaim(&t);
		assert(tree_stats(&t).versions == 5 - i);
	}
	assert(tree_stats(&t).nodes == tree_current(&t)->version.node_count);
	validate(tree_current(&t));
	finish();
}

static void
a_held_version_keeps_only_its_own_nodes(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	uint32_t root, leaf;
	uint64_t nodes;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	leaf = add(&t, &p, add(&t, &p, root, 0, 0), 0, 0);
	commit();
	validate(tree_latest(&t, &hold));
	tree_begin(&t);
	set(&t, &p, leaf, 1);
	tree_publish(&t);
	nodes = tree_stats(&t).nodes;
	assert(nodes == 6);
	for (int i = 2; i < 1000; i++) {
		tree_begin(&t);
		set(&t, &p, leaf, i);
		tree_publish(&t);
		assert(tree_stats(&t).nodes == nodes && tree_stats(&t).versions == 2);
	}
	assert(tree_child(tree_child(tree_root(tree_held(&t, &hold)), 0), 0)->value == 0);
	tree_release(&t, &hold);
	finish();
}

static void
ancestors_of_an_edited_node_need_no_memory(void)
{
	struct item *item;
	uint32_t handle;

	start(4096);
	tree_begin(&t);
	handle = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 1);
	for (int i = 0; i < 40; i++)
		handle = add(&t, &p, handle, 0, 1);
	commit();

	tree_begin(&t);
	while ((item = tree_edit(&t, handle)) == nullptr)
		assert(provide(&t, &p));
	++item->value;
	drain(&t, &p);
	p.budget = p.outstanding;
	lift(&t, handle, 1);
	assert(p.outstanding == p.budget);
	commit();
	assert(tree_root(tree_current(&t))->sum == 42);
	finish();
}

static void
one_parent_holds_many_children(void)
{
	static constexpr uint32_t COUNT = 10000;
	static uint32_t kids[COUNT];
	uint32_t root;
	const struct item *r;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	for (uint32_t i = 0; i < COUNT; i++)
		kids[i] = add(&t, &p, root, i, i);
	commit();
	r = tree_root(tree_current(&t));
	assert(r->node.child_count == COUNT && r->sum == (int64_t)COUNT * (COUNT - 1) / 2);
	for (uint32_t i = 0; i < COUNT; i++)
		assert(tree_child(r, i)->node.handle == kids[i]);

	tree_begin(&t);
	shift(&t, &p, kids[COUNT - 1], root, 0);
	set(&t, &p, kids[5000], 0);
	for (uint32_t i = 0; i < COUNT; i += 2)
		drop(&t, &p, kids[i]);
	commit();
	r = tree_root(tree_current(&t));
	assert(r->node.child_count == COUNT / 2 && tree_child(r, 0)->node.handle == kids[COUNT - 1]);

	tree_begin(&t);
	drop(&t, &p, root);
	commit();
	drain(&t, &p);
	assert(p.outstanding == 0);
	finish();
}

static void
depth_is_not_limited(void)
{
	static constexpr uint32_t DEPTH = 200000;
	const struct item *item;
	uint32_t root, second, handle, depth = 0;

	start(1 << 16);
	tree_begin(&t);
	root = handle = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	second = handle = add(&t, &p, handle, 0, 0);
	for (uint32_t i = 2; i < DEPTH; i++) {
		struct item *below;

		while ((below = tree_create(&t, handle, 0)) == nullptr)
			assert(provide(&t, &p));
		below->value = 0;
		below->sum = 0;
		handle = below->node.handle;
	}
	tree_publish(&t);

	tree_begin(&t);
	set(&t, &p, handle, 3);
	tree_publish(&t);
	for (item = tree_root(tree_current(&t)); item->node.child_count != 0; item = tree_child(item, 0)) {
		assert(item->sum == 3);
		++depth;
	}
	assert(depth == DEPTH - 1 && item->value == 3);

	tree_begin(&t);
	set(&t, &p, handle, 4);
	tree_abort(&t);
	assert(tree_root(tree_current(&t))->sum == 3);

	tree_begin(&t);
	drop(&t, &p, second);
	tree_publish(&t);
	assert(tree_stats(&t).nodes == 1 && tree_node(&t, handle) == nullptr && tree_node(&t, root)->sum == 0);
	finish();
}

static void
every_hold_and_pin_fits(void)
{
	struct poor_world_hold holds[7];
	struct poor_world_pin pins[7];
	uint32_t root;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	commit();
	for (uint32_t i = 0; i < 7; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		pins[i] = (struct poor_world_pin)POOR_WORLD_PIN_INIT;
	}
	for (uint32_t round = 0; round < 100; round++) {
		for (uint32_t i = 0; i < 7; i++) {
			if (rng() % 2 == 0)
				tree_pin(&t, &holds[i], &pins[i]);
			validate(tree_latest(&t, &holds[i]));
			tree_begin(&t);
			set(&t, &p, root, round);
			tree_publish(&t);
		}
	}
	assert(tree_stats(&t).versions <= 15);
	for (uint32_t i = 0; i < 7; i++) {
		validate(tree_latest(&t, &holds[i]));
		tree_release(&t, &holds[i]);
		tree_unpin(&t, &pins[i]);
	}
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1);
	finish();
}

static void
begin_takes_a_slot_that_was_just_let_go(void)
{
	struct poor_world_hold holds[15];
	uint32_t root;

	start(4096);
	tree_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	commit();
	for (uint32_t i = 0; i < 15; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		validate(tree_latest(&t, &holds[i]));
		tree_begin(&t);
		set(&t, &p, root, i + 1);
		tree_publish(&t);
	}
	assert(tree_stats(&t).versions == 16);
	tree_release(&t, &holds[7]);
	tree_begin(&t);
	set(&t, &p, root, 16);
	tree_publish(&t);
	assert(tree_stats(&t).versions == 15);
	for (uint32_t i = 0; i < 15; i++) {
		assert(i == 7 || tree_root(tree_held(&t, &holds[i]))->sum == i);
		tree_release(&t, &holds[i]);
	}
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1);
	finish();
}

int
main(void)
{
	RUN(empty_world);
	RUN(init_does_not_need_clean_memory);
	RUN(children_keep_their_order);
	RUN(readers_keep_what_they_latched);
	RUN(destroyed_handles_come_back);
	RUN(nodes_move);
	RUN(abort_restores_everything);
	RUN(versions_die_in_any_order);
	RUN(a_held_version_keeps_only_its_own_nodes);
	RUN(ancestors_of_an_edited_node_need_no_memory);
	RUN(one_parent_holds_many_children);
	RUN(depth_is_not_limited);
	RUN(every_hold_and_pin_fits);
	RUN(begin_takes_a_slot_that_was_just_let_go);
	return 0;
}
