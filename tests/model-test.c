/* SPDX-License-Identifier: MIT */
#include "ops.h"

constexpr uint32_t TRANSACTIONS = 6000;
constexpr uint32_t READERS = 6;

struct mnode {
	bool alive;
	uint64_t id;
	int64_t value;
	int64_t sum;
	uint32_t parent;
	uint32_t count;
	uint32_t capacity;
	uint32_t *children;
};

struct model {
	struct mnode *nodes;
	uint32_t size;
	uint32_t root;
	uint64_t id;
};

static tree t;
static struct provider p;
static struct model model = { .root = POOR_WORLD_NO_HANDLE };
static uint64_t epoch_hash[TRANSACTIONS + 2];
static uint64_t diffs, yields, nested;

static struct mnode *
model_node(struct model *m, uint32_t handle)
{
	if (handle >= m->size) {
		const uint32_t size = handle + 64;

		m->nodes = realloc(m->nodes, size * sizeof(*m->nodes));
		assert(m->nodes != nullptr);
		memset(&m->nodes[m->size], 0, (size - m->size) * sizeof(*m->nodes));
		m->size = size;
	}
	return &m->nodes[handle];
}

static void
model_insert(struct model *m, uint32_t parent, uint32_t index, uint32_t handle)
{
	struct mnode *above = model_node(m, parent);

	if (above->count == above->capacity) {
		above->capacity = above->capacity * 2 + 4;
		above->children = realloc(above->children, above->capacity * sizeof(*above->children));
		assert(above->children != nullptr);
	}
	memmove(&above->children[index + 1], &above->children[index], (above->count - index) * sizeof(*above->children));
	above->children[index] = handle;
	++above->count;
	model_node(m, handle)->parent = parent;
}

static void
model_remove(struct model *m, uint32_t handle)
{
	struct mnode *above = model_node(m, model_node(m, handle)->parent);
	uint32_t index = 0;

	while (above->children[index] != handle)
		++index;
	--above->count;
	memmove(&above->children[index], &above->children[index + 1], (above->count - index) * sizeof(*above->children));
}

static void
model_kill(struct model *m, uint32_t handle)
{
	struct mnode *node = model_node(m, handle);

	for (uint32_t i = 0; i < node->count; i++)
		model_kill(m, node->children[i]);
	free(node->children);
	*node = (struct mnode){};
}

static void
model_apply(struct model *m, const struct op *op, uint32_t created)
{
	struct mnode *node;

	switch (op->kind) {
	case OP_CREATE:
		node = model_node(m, created);
		assert(!node->alive);
		*node = (struct mnode){ .alive = true, .id = ++m->id, .value = op->value, .parent = op->parent };
		if (op->parent == POOR_WORLD_NO_HANDLE)
			m->root = created;
		else
			model_insert(m, op->parent, op->index, created);
		break;
	case OP_DESTROY:
		if (model_node(m, op->handle)->parent == POOR_WORLD_NO_HANDLE)
			m->root = POOR_WORLD_NO_HANDLE;
		else
			model_remove(m, op->handle);
		model_kill(m, op->handle);
		break;
	case OP_EDIT:
		model_node(m, op->handle)->value = op->value;
		break;
	case OP_MOVE:
		model_remove(m, op->handle);
		model_insert(m, op->parent, op->index, op->handle);
		break;
	}
}

static struct model
model_clone(const struct model *m)
{
	struct model copy = *m;

	copy.nodes = malloc((m->size + 1) * sizeof(*copy.nodes));
	assert(copy.nodes != nullptr);
	for (uint32_t i = 0; i < m->size; i++) {
		copy.nodes[i] = m->nodes[i];
		if (m->nodes[i].children != nullptr) {
			copy.nodes[i].children = malloc(m->nodes[i].capacity * sizeof(uint32_t));
			assert(copy.nodes[i].children != nullptr);
			memcpy(copy.nodes[i].children, m->nodes[i].children, m->nodes[i].count * sizeof(uint32_t));
		}
	}
	return copy;
}

static void
model_free(struct model *m)
{
	for (uint32_t i = 0; i < m->size; i++)
		free(m->nodes[i].children);
	free(m->nodes);
}

static int64_t
model_sum(struct model *m, uint32_t handle)
{
	struct mnode *node = &m->nodes[handle];

	node->sum = node->value;
	for (uint32_t i = 0; i < node->count; i++)
		node->sum += model_sum(m, node->children[i]);
	return node->sum;
}

static uint64_t
model_hash_node(const struct model *m, uint32_t handle, uint64_t hash)
{
	const struct mnode *node = &m->nodes[handle];

	assert(node->alive);
	hash = mix(hash, handle);
	hash = mix(hash, node->id);
	hash = mix(hash, (uint64_t)node->value);
	hash = mix(hash, (uint64_t)node->sum);
	hash = mix(hash, node->count);
	for (uint32_t i = 0; i < node->count; i++)
		hash = model_hash_node(m, node->children[i], hash);
	return hash;
}

static uint64_t
model_hash(struct model *m)
{
	if (m->root == POOR_WORLD_NO_HANDLE)
		return 0;
	model_sum(m, m->root);
	return model_hash_node(m, m->root, UINT64_C(0xcbf29ce484222325));
}

static uint32_t
slot_of(const struct item *parent, const struct item *child)
{
	for (uint32_t i = 0; i < parent->node.child_count; i++) {
		if (tree_child(parent, i) == child)
			return i;
	}
	assert(false);
	return 0;
}

static uint32_t
slot_of_id(const struct item *parent, uint64_t id)
{
	for (uint32_t i = 0; i < parent->node.child_count; i++) {
		if (tree_child(parent, i)->node.id == id)
			return i;
	}
	return UINT32_MAX;
}

enum mark : uint8_t {
	MARK_NONE,
	MARK_ALONE,
	MARK_STABLE,
	MARK_MOVED,
	MARK_SAME,
};

static void check_pair(struct poor_world_diff_scratch *scratch, const struct item *old, const struct item *next);

static void
check_children(struct poor_world_diff_scratch *scratch, const struct item *old, const struct item *next)
{
	const uint32_t old_count = old->node.child_count, new_count = next->node.child_count;
	uint8_t *old_marks = calloc(old_count + 1, 1), *new_marks = calloc(new_count + 1, 1);
	uint32_t *run = calloc(new_count + 1, sizeof(*run)), *from = calloc(new_count + 1, sizeof(*from));
	const uint32_t top = scratch->top;
	uint32_t common = 0, moved = 0, longest = 0, last = 0;
	bool first = true;
	struct poor_world_diff diff;

	assert(old_marks != nullptr && new_marks != nullptr && run != nullptr && from != nullptr);
	++diffs;
	tree_diff_begin(&diff, scratch, old, next, false);
	while (tree_diff_next(&diff)) {
		const struct item *o = tree_diff_old(&diff), *n = tree_diff_new(&diff);

		++yields;
		if (o == nullptr) {
			assert(!tree_diff_moved(&diff) && slot_of_id(old, n->node.id) == UINT32_MAX);
			assert(new_marks[slot_of(next, n)] == MARK_NONE);
			new_marks[slot_of(next, n)] = MARK_ALONE;
		} else if (n == nullptr) {
			assert(!tree_diff_moved(&diff) && slot_of_id(next, o->node.id) == UINT32_MAX);
			assert(old_marks[slot_of(old, o)] == MARK_NONE);
			old_marks[slot_of(old, o)] = MARK_ALONE;
		} else {
			assert(o->node.id == n->node.id && (tree_diff_moved(&diff) || o != n));
			assert(old_marks[slot_of(old, o)] == MARK_NONE && new_marks[slot_of(next, n)] == MARK_NONE);
			old_marks[slot_of(old, o)] = new_marks[slot_of(next, n)] = tree_diff_moved(&diff) ? MARK_MOVED : MARK_STABLE;
			if (!tree_diff_moved(&diff)) {
				++nested;
				check_pair(scratch, o, n);
			}
		}
	}
	assert(scratch->top == top);

	for (uint32_t j = 0; j < new_count; j++) {
		const struct item *n = tree_child(next, j);
		uint32_t i;

		if (new_marks[j] == MARK_ALONE)
			continue;
		i = slot_of_id(old, n->node.id);
		assert(i != UINT32_MAX);
		if (new_marks[j] == MARK_NONE) {
			assert(tree_child(old, i) == n && old_marks[i] == MARK_NONE);
			old_marks[i] = new_marks[j] = MARK_SAME;
		}
		from[common++] = i;
		if (new_marks[j] == MARK_MOVED) {
			++moved;
		} else {
			assert(first || i > last);
			first = false;
			last = i;
		}
	}
	for (uint32_t i = 0; i < old_count; i++)
		assert(old_marks[i] != MARK_NONE);
	for (uint32_t k = 0; k < common; k++) {
		run[k] = 1;
		for (uint32_t before = 0; before < k; before++) {
			if (from[before] < from[k] && run[before] + 1 > run[k])
				run[k] = run[before] + 1;
		}
		if (run[k] > longest)
			longest = run[k];
	}
	assert(moved == common - longest);
	free(old_marks);
	free(new_marks);
	free(run);
	free(from);
}

static void
check_pair(struct poor_world_diff_scratch *scratch, const struct item *old, const struct item *next)
{
	assert(old->node.id == next->node.id && old->node.handle == next->node.handle);
	if (old != next)
		check_children(scratch, old, next);
}

static void
check_diff(const struct snapshot *old, const struct snapshot *next)
{
	const uint32_t slot_count =
		old->version.handle_bound > next->version.handle_bound ? old->version.handle_bound : next->version.handle_bound;
	const uint32_t match_count = old->version.node_count + next->version.node_count;
	uint32_t *slots = malloc((slot_count + 1) * sizeof(*slots));
	struct poor_world_diff_match *matches = malloc((match_count + 1) * sizeof(*matches));
	const struct item *a = tree_root(old), *b = tree_root(next);
	struct poor_world_diff_scratch scratch;
	struct poor_world_diff diff;
	uint32_t all = 0;

	assert(slots != nullptr && matches != nullptr);
	tree_diff_scratch_init(&scratch, slot_count, slots, match_count, matches);
	if (a != nullptr && b != nullptr && a->node.id == b->node.id) {
		check_pair(&scratch, a, b);
		tree_diff_begin(&diff, &scratch, a, b, true);
		while (tree_diff_next(&diff))
			all += tree_diff_new(&diff) != nullptr;
		assert(all == b->node.child_count);
		tree_diff_begin(&diff, &scratch, a, b, true);
		if (tree_diff_next(&diff))
			tree_diff_stop(&diff);
		assert(!tree_diff_next(&diff));
	}
	assert(scratch.top == 0);
	for (uint32_t i = 0; i < slot_count; i++)
		assert(slots[i] == 0);
	free(slots);
	free(matches);
}

static void
perform(const struct op *op)
{
	uint32_t created = POOR_WORLD_NO_HANDLE;

	while (!try_op(&t, op, &created))
		assert(provide(&t, &p));
	model_apply(&model, op, created);
}

int
main(void)
{
	struct poor_world_hold holds[READERS];
	struct poor_world_pin pins[READERS];
	uint64_t aborted = 0;

	tree_init(&t, 4096)->tag = 0;
	p = (struct provider)PROVIDER_INIT;
	for (uint32_t i = 0; i < READERS; i++) {
		holds[i] = (struct poor_world_hold)POOR_WORLD_HOLD_INIT;
		pins[i] = (struct poor_world_pin)POOR_WORLD_PIN_INIT;
	}
	for (uint32_t transaction = 0; transaction < TRANSACTIONS; transaction++) {
		const bool abort_it = rng() % 6 == 0;
		const uint32_t ops = (uint32_t)(1 + rng() % 6);
		struct model saved = abort_it ? model_clone(&model) : (struct model){};
		uint32_t reader = (uint32_t)(rng() % READERS);
		const struct snapshot *held;

		tree_begin(&t)->tag = transaction;
		collect(tree_next(&t));
		for (uint32_t i = 0; i < ops; i++) {
			const struct op op = pick_op(&t, 250);

			perform(&op);
			collect(tree_next(&t));
		}
		if (rng() % 150 == 0 && live_count != 0) {
			const struct op wide = { .kind = OP_CREATE, .parent = live[rng() % live_count], .value = 1 };

			for (uint32_t i = 0; i < 700; i++)
				perform(&wide);
			collect(tree_next(&t));
		}
		assert(hash_of(tree_next(&t)) == model_hash(&model));
		validate(tree_next(&t));
		check_index(&t, tree_next(&t));
		if (abort_it) {
			tree_abort(&t);
			model_free(&model);
			model = saved;
			++aborted;
			check_index(&t, tree_current(&t));
		} else {
			tree_publish(&t);
			epoch_hash[tree_current(&t)->version.epoch] = model_hash(&model);
		}
		assert(hash_of(tree_current(&t)) == epoch_hash[tree_current(&t)->version.epoch]);

		if (rng() % 3 == 0)
			tree_pin(&t, &holds[reader], &pins[reader]);
		held = tree_latest(&t, &holds[reader]);
		assert(hash_of(held) == epoch_hash[held->version.epoch]);
		reader = (uint32_t)(rng() % READERS);
		if (tree_pinned(&t, &pins[reader]) != nullptr) {
			const struct snapshot *old = tree_pinned(&t, &pins[reader]);

			assert(hash_of(old) == epoch_hash[old->version.epoch]);
			validate(old);
			check_diff(old, tree_current(&t));
			if (rng() % 4 == 0)
				tree_unpin(&t, &pins[reader]);
		}
		if (rng() % 5 == 0)
			tree_release(&t, &holds[(uint32_t)(rng() % READERS)]);
		if (rng() % 7 == 0)
			tree_reclaim(&t);
		if (rng() % 40 == 0)
			drain(&t, &p);
	}
	for (uint32_t i = 0; i < READERS; i++) {
		tree_release(&t, &holds[i]);
		tree_unpin(&t, &pins[i]);
	}
	tree_reclaim(&t);
	assert(tree_stats(&t).versions == 1 && tree_stats(&t).nodes == tree_current(&t)->version.node_count);
	println("epoch ", tree_current(&t)->version.epoch, ", aborted ", aborted, ", nodes ",
		tree_current(&t)->version.node_count, ", diffs ", diffs, ", yields ", yields, ", nested ", nested);
	assert(diffs > TRANSACTIONS && nested > TRANSACTIONS / 4);
	model_free(&model);
	tree_fini(&t);
	drain(&t, &p);
	assert(p.outstanding == 0);
	return 0;
}
