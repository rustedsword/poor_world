/* SPDX-License-Identifier: MIT */
#include "tree.h"

static tree t;
static struct provider p;

static void
start(size_t slab)
{
	poor_world_init(&t, slab)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
}

static void
finish(void)
{
	poor_world_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
}

static void
commit(void)
{
	validate(poor_world_next(&t));
	check_index(&t, poor_world_next(&t));
	poor_world_publish(&t);
	assert(poor_world_stats(&t).versions == 1);
	assert(poor_world_stats(&t).nodes == poor_world_current(&t)->version.node_count);
}

static void
empty_world(void)
{
	start(4096);
	assert(poor_world_current(&t)->version.epoch == 0);
	assert(poor_world_root(TREE, poor_world_current(&t)) == nullptr);
	assert(poor_world_stats(&t).bytes == 0 && poor_world_stats(&t).nodes == 0);
	poor_world_begin(&t)->tag = 7;
	poor_world_publish(&t);
	assert(poor_world_current(&t)->version.epoch == 1 && poor_world_current(&t)->tag == 7);
	assert(poor_world_node(&t, 0) == nullptr && poor_world_node(&t, POOR_WORLD_NO_HANDLE) == nullptr);
	finish();
}

static void
init_does_not_need_clean_memory(void)
{
	static tree s;
	struct provider sp = PROVIDER_INIT;
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;

	memset(&s, 0xa5, sizeof(s));
	poor_world_init(&s, 4096)->tag = 0;
	assert(poor_world_latest(&s, &hold)->version.epoch == 0 && poor_world_stats(&s).bytes == 0);
	poor_world_begin(&s);
	add(&s, &sp, POOR_WORLD_NO_HANDLE, 0, 5);
	poor_world_publish(&s);
	assert(poor_world_root(TREE, poor_world_latest(&s, &hold))->sum == 5);
	poor_world_release(&s, &hold);
	poor_world_fini(&s);
	drain(&s, &sp);
	assert(sp.outstanding == 0);
}

static void
children_keep_their_order(void)
{
	uint32_t root, a, b, c, d;
	const struct item *r;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 1);
	a = add(&t, &p, root, 0, 10);
	b = add(&t, &p, root, 1, 20);
	c = add(&t, &p, root, 2, 30);
	d = add(&t, &p, root, 1, 40);
	commit();

	r = poor_world_root(TREE, poor_world_current(&t));
	assert(r->node.child_count == 4 && r->sum == 101);
	assert(poor_world_child(TREE, r, 0)->node.handle == a && poor_world_child(TREE, r, 1)->node.handle == d);
	assert(poor_world_child(TREE, r, 2)->node.handle == b && poor_world_child(TREE, r, 3)->node.handle == c);
	assert(poor_world_index(&t, d) == 1 && poor_world_index(&t, c) == 3 && poor_world_parent(&t, d) == root);
	assert(poor_world_parent(&t, root) == POOR_WORLD_NO_HANDLE);
	assert(poor_world_node(&t, b)->value == 20);
	assert(poor_world_child(TREE, r, 0)->node.id != poor_world_child(TREE, r, 1)->node.id);
	assert(poor_world_current(&t)->version.node_count == 5);
	finish();
}

static void
readers_keep_what_they_latched(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT, newer = POOR_WORLD_HOLD_INIT;
	const struct snapshot *before, *after;
	uint32_t root, a, b, leaf;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	leaf = add(&t, &p, a, 0, 3);
	commit();

	before = poor_world_latest(&t, &hold);
	assert(poor_world_held(&t, &hold) == before);
	poor_world_begin(&t);
	set(&t, &p, leaf, 100);
	poor_world_publish(&t);
	assert(poor_world_latest(&t, &hold) == poor_world_current(&t));
	poor_world_release(&t, &hold);
	poor_world_reclaim(&t);

	before = poor_world_latest(&t, &hold);
	poor_world_begin(&t);
	set(&t, &p, leaf, 200);
	poor_world_publish(&t);
	after = poor_world_latest(&t, &newer);
	assert(before != after && before->version.epoch + 1 == after->version.epoch);
	assert(poor_world_child(TREE, poor_world_child(TREE, poor_world_root(TREE, before), 0), 0)->value == 100);
	assert(poor_world_child(TREE, poor_world_child(TREE, poor_world_root(TREE, after), 0), 0)->value == 200);
	assert(poor_world_root(TREE, before) != poor_world_root(TREE, after));
	assert(poor_world_child(TREE, poor_world_root(TREE, before), 1) == poor_world_child(TREE, poor_world_root(TREE, after), 1));
	assert(poor_world_child(TREE, poor_world_root(TREE, after), 1)->node.handle == b);
	validate(before);
	validate(after);
	assert(poor_world_stats(&t).versions == 2 && poor_world_stats(&t).nodes == 4 + 3);
	assert(poor_world_oldest_held(&t) == before->version.epoch);

	poor_world_release(&t, &hold);
	assert(poor_world_oldest_held(&t) == after->version.epoch);
	poor_world_reclaim(&t);
	assert(poor_world_stats(&t).versions == 1 && poor_world_stats(&t).nodes == 4);
	poor_world_release(&t, &newer);
	finish();
}

static void
destroyed_handles_come_back(void)
{
	uint32_t root, a, b, again;
	uint64_t id;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	add(&t, &p, a, 0, 2);
	b = add(&t, &p, a, 1, 3);
	add(&t, &p, b, 0, 4);
	commit();
	id = poor_world_node(&t, b)->node.id;

	poor_world_begin(&t);
	drop(&t, &p, a);
	assert(poor_world_node(&t, a) == nullptr && poor_world_node(&t, b) == nullptr);
	assert(poor_world_next(&t)->version.node_count == 1);
	commit();
	assert(poor_world_root(TREE, poor_world_current(&t))->sum == 0);

	poor_world_begin(&t);
	again = add(&t, &p, root, 0, 9);
	assert(again == a || again == b || again < 5);
	assert(poor_world_node(&t, again)->node.id > id);
	commit();

	poor_world_begin(&t);
	drop(&t, &p, root);
	assert(poor_world_root(TREE, poor_world_next(&t)) == nullptr);
	commit();
	assert(poor_world_stats(&t).nodes == 0);
	assert(poor_world_current(&t)->version.handle_bound == 0);
	finish();
}

static void
nodes_move(void)
{
	uint32_t root, a, b, x, y, z;
	const struct item *r;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	x = add(&t, &p, a, 0, 10);
	y = add(&t, &p, a, 1, 20);
	z = add(&t, &p, a, 2, 30);
	add(&t, &p, y, 0, 5);
	commit();

	poor_world_begin(&t);
	shift(&t, &p, z, a, 0);
	shift(&t, &p, z, a, 0);
	commit();
	r = poor_world_child(TREE, poor_world_root(TREE, poor_world_current(&t)), 0);
	assert(poor_world_child(TREE, r, 0)->node.handle == z && poor_world_child(TREE, r, 1)->node.handle == x);
	assert(poor_world_child(TREE, r, 2)->node.handle == y);

	poor_world_begin(&t);
	shift(&t, &p, x, a, 2);
	shift(&t, &p, y, b, 0);
	assert(poor_world_parent(&t, y) == b && poor_world_index(&t, y) == 0);
	commit();
	r = poor_world_root(TREE, poor_world_current(&t));
	assert(poor_world_child(TREE, r, 0)->sum == 1 + 30 + 10 && poor_world_child(TREE, r, 1)->sum == 2 + 20 + 5);
	assert(poor_world_child(TREE, poor_world_child(TREE, r, 0), 1)->node.handle == x);

	poor_world_begin(&t);
	shift(&t, &p, y, root, 0);
	shift(&t, &p, a, y, 1);
	commit();
	r = poor_world_root(TREE, poor_world_current(&t));
	assert(r->node.child_count == 2 && poor_world_child(TREE, r, 0)->node.handle == y);
	assert(poor_world_child(TREE, poor_world_child(TREE, r, 0), 1)->node.handle == a && r->sum == 68);
	finish();
}

static void
abort_restores_everything(void)
{
	uint32_t root, a, b, x, y, grown;
	uint64_t hash, nodes;
	size_t bound;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	a = add(&t, &p, root, 0, 1);
	b = add(&t, &p, root, 1, 2);
	x = add(&t, &p, a, 0, 10);
	y = add(&t, &p, a, 1, 20);
	add(&t, &p, y, 0, 5);
	commit();
	hash = hash_of(poor_world_current(&t));
	nodes = poor_world_stats(&t).nodes;
	bound = poor_world_current(&t)->version.handle_bound;

	poor_world_begin(&t)->tag = 99;
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
	validate(poor_world_next(&t));
	check_index(&t, poor_world_next(&t));
	poor_world_abort(&t);

	assert(poor_world_current(&t)->tag == 0);
	assert(hash_of(poor_world_current(&t)) == hash);
	validate(poor_world_current(&t));
	check_index(&t, poor_world_current(&t));
	assert(poor_world_stats(&t).nodes == nodes && poor_world_stats(&t).versions == 1);
	assert(poor_world_node(&t, grown) == nullptr);

	poor_world_begin(&t);
	set(&t, &p, y, 21);
	add(&t, &p, x, 0, 3);
	commit();
	assert(poor_world_root(TREE, poor_world_current(&t))->sum == 1 + 2 + 10 + 21 + 5 + 3);
	assert(poor_world_current(&t)->version.handle_bound == bound);
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
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	for (uint32_t i = 0; i < 10; i++)
		kids[i] = add(&t, &p, root, i, i);
	commit();

	for (uint32_t i = 0; i < 5; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		pins[i] = (struct poor_world_pin)POOR_WORLD_PIN_INIT;
		hashes[i] = hash_of(poor_world_latest(&t, &holds[i]));
		poor_world_pin(&t, &holds[i], &pins[i]);
		poor_world_release(&t, &holds[i]);
		poor_world_begin(&t);
		set(&t, &p, kids[i], 100 + i);
		drop(&t, &p, kids[9 - i]);
		kids[9 - i] = add(&t, &p, kids[i], 0, 50);
		poor_world_publish(&t);
	}
	assert(poor_world_stats(&t).versions == 6);
	for (uint32_t i = 0; i < 5; i++) {
		for (uint32_t j = i; j < 5; j++) {
			assert(hash_of(poor_world_pinned(&t, &pins[order[j]])) == hashes[order[j]]);
			validate(poor_world_pinned(&t, &pins[order[j]]));
		}
		poor_world_unpin(&t, &pins[order[i]]);
		assert(poor_world_pinned(&t, &pins[order[i]]) == nullptr);
		poor_world_reclaim(&t);
		assert(poor_world_stats(&t).versions == 5 - i);
	}
	assert(poor_world_stats(&t).nodes == poor_world_current(&t)->version.node_count);
	validate(poor_world_current(&t));
	finish();
}

static void
a_held_version_keeps_only_its_own_nodes(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	uint32_t root, leaf;
	uint64_t nodes;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	leaf = add(&t, &p, add(&t, &p, root, 0, 0), 0, 0);
	commit();
	validate(poor_world_latest(&t, &hold));
	poor_world_begin(&t);
	set(&t, &p, leaf, 1);
	poor_world_publish(&t);
	nodes = poor_world_stats(&t).nodes;
	assert(nodes == 6);
	for (int i = 2; i < 1000; i++) {
		poor_world_begin(&t);
		set(&t, &p, leaf, i);
		poor_world_publish(&t);
		assert(poor_world_stats(&t).nodes == nodes && poor_world_stats(&t).versions == 2);
	}
	assert(poor_world_child(TREE, poor_world_child(TREE, poor_world_root(TREE, poor_world_held(&t, &hold)), 0), 0)->value == 0);
	poor_world_release(&t, &hold);
	finish();
}

static void
ancestors_of_an_edited_node_need_no_memory(void)
{
	struct item *item;
	uint32_t handle;

	start(4096);
	poor_world_begin(&t);
	handle = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 1);
	for (int i = 0; i < 40; i++)
		handle = add(&t, &p, handle, 0, 1);
	commit();

	poor_world_begin(&t);
	while ((item = poor_world_edit(&t, handle)) == nullptr)
		assert(provide(&t, &p));
	++item->value;
	drain(&t, &p);
	p.budget = p.outstanding;
	lift(&t, handle, 1);
	assert(p.outstanding == p.budget);
	commit();
	assert(poor_world_root(TREE, poor_world_current(&t))->sum == 42);
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
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	for (uint32_t i = 0; i < COUNT; i++)
		kids[i] = add(&t, &p, root, i, i);
	commit();
	r = poor_world_root(TREE, poor_world_current(&t));
	assert(r->node.child_count == COUNT && r->sum == (int64_t)COUNT * (COUNT - 1) / 2);
	for (uint32_t i = 0; i < COUNT; i++)
		assert(poor_world_child(TREE, r, i)->node.handle == kids[i]);

	poor_world_begin(&t);
	shift(&t, &p, kids[COUNT - 1], root, 0);
	set(&t, &p, kids[5000], 0);
	for (uint32_t i = 0; i < COUNT; i += 2)
		drop(&t, &p, kids[i]);
	commit();
	r = poor_world_root(TREE, poor_world_current(&t));
	assert(r->node.child_count == COUNT / 2 && poor_world_child(TREE, r, 0)->node.handle == kids[COUNT - 1]);

	poor_world_begin(&t);
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
	poor_world_begin(&t);
	root = handle = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	second = handle = add(&t, &p, handle, 0, 0);
	for (uint32_t i = 2; i < DEPTH; i++) {
		struct item *below;

		while ((below = poor_world_create(&t, handle, 0)) == nullptr)
			assert(provide(&t, &p));
		below->value = 0;
		below->sum = 0;
		handle = below->node.handle;
	}
	poor_world_publish(&t);

	poor_world_begin(&t);
	set(&t, &p, handle, 3);
	poor_world_publish(&t);
	for (item = poor_world_root(TREE, poor_world_current(&t)); item->node.child_count != 0; item = poor_world_child(TREE, item, 0)) {
		assert(item->sum == 3);
		++depth;
	}
	assert(depth == DEPTH - 1 && item->value == 3);

	poor_world_begin(&t);
	set(&t, &p, handle, 4);
	poor_world_abort(&t);
	assert(poor_world_root(TREE, poor_world_current(&t))->sum == 3);

	poor_world_begin(&t);
	drop(&t, &p, second);
	poor_world_publish(&t);
	assert(poor_world_stats(&t).nodes == 1 && poor_world_node(&t, handle) == nullptr && poor_world_node(&t, root)->sum == 0);
	finish();
}

static uint32_t evaluations;

static tree *
counted(void)
{
	++evaluations;
	return &t;
}

static uint32_t
counted_handle(uint32_t handle)
{
	++evaluations;
	return handle;
}

static void
arguments_are_evaluated_once(void)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	struct poor_world_pin pin = POOR_WORLD_PIN_INIT;
	const struct snapshot *latest;
	const struct item *node;
	struct item *item;
	uint32_t root;

	start(4096);
	poor_world_begin(counted());
	assert(evaluations == 1);
	while ((item = poor_world_create(&t, POOR_WORLD_NO_HANDLE, 0)) == nullptr)
		assert(provide(&t, &p));
	item->value = item->sum = 0;
	root = item->node.handle;
	evaluations = 0;
	while ((item = poor_world_create(counted(), counted_handle(root), counted_handle(0))) == nullptr) {
		assert(provide(&t, &p));
		evaluations = 0;
	}
	assert(evaluations == 3);
	item->value = item->sum = 0;
	evaluations = 0;
	assert(poor_world_edit(counted(), counted_handle(root)) != nullptr && evaluations == 2);
	node = poor_world_node(counted(), counted_handle(root));
	assert(evaluations == 4 && poor_world_next(counted()) != nullptr && evaluations == 5);
	assert(poor_world_child(counted(), node, counted_handle(0)) != nullptr && evaluations == 6);
	poor_world_publish(counted());
	latest = poor_world_latest(counted(), &hold);
	poor_world_pin(counted(), &hold, &pin);
	assert(poor_world_held(counted(), &hold) == latest && poor_world_pinned(counted(), &pin) == latest);
	assert(poor_world_root(counted(), latest) != nullptr && poor_world_current(counted()) == latest);
	poor_world_unpin(counted(), &pin);
	poor_world_release(counted(), &hold);
	assert(evaluations == 6 + 1 + 1 + 1 + 2 + 1 + 2);
	finish();
}

static void
every_hold_and_pin_fits(void)
{
	struct poor_world_hold holds[7];
	struct poor_world_pin pins[7];
	uint32_t root;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	commit();
	for (uint32_t i = 0; i < 7; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		pins[i] = (struct poor_world_pin)POOR_WORLD_PIN_INIT;
	}
	for (uint32_t round = 0; round < 100; round++) {
		for (uint32_t i = 0; i < 7; i++) {
			if (rng() % 2 == 0)
				poor_world_pin(&t, &holds[i], &pins[i]);
			validate(poor_world_latest(&t, &holds[i]));
			poor_world_begin(&t);
			set(&t, &p, root, round);
			poor_world_publish(&t);
		}
	}
	assert(poor_world_stats(&t).versions <= 15);
	for (uint32_t i = 0; i < 7; i++) {
		validate(poor_world_latest(&t, &holds[i]));
		poor_world_release(&t, &holds[i]);
		poor_world_unpin(&t, &pins[i]);
	}
	poor_world_reclaim(&t);
	assert(poor_world_stats(&t).versions == 1);
	finish();
}

static void
begin_takes_a_slot_that_was_just_let_go(void)
{
	struct poor_world_hold holds[15];
	uint32_t root;

	start(4096);
	poor_world_begin(&t);
	root = add(&t, &p, POOR_WORLD_NO_HANDLE, 0, 0);
	commit();
	for (uint32_t i = 0; i < 15; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		validate(poor_world_latest(&t, &holds[i]));
		poor_world_begin(&t);
		set(&t, &p, root, i + 1);
		poor_world_publish(&t);
	}
	assert(poor_world_stats(&t).versions == 16);
	poor_world_release(&t, &holds[7]);
	poor_world_begin(&t);
	set(&t, &p, root, 16);
	poor_world_publish(&t);
	assert(poor_world_stats(&t).versions == 15);
	for (uint32_t i = 0; i < 15; i++) {
		assert(i == 7 || poor_world_root(TREE, poor_world_held(&t, &holds[i]))->sum == i);
		poor_world_release(&t, &holds[i]);
	}
	poor_world_reclaim(&t);
	assert(poor_world_stats(&t).versions == 1);
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
	RUN(arguments_are_evaluated_once);
	RUN(every_hold_and_pin_fits);
	RUN(begin_takes_a_slot_that_was_just_let_go);
	return 0;
}
