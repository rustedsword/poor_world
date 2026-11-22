/* SPDX-License-Identifier: MIT */
#ifndef POOR_WORLD_H
#define POOR_WORLD_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <poor_fan.h>
#include <poor_list.h>

/*
 * Poor World.
 *
 * A tree that one thread changes while other threads read it.
 *
 * The writer changes the tree in a transaction and publishes it as a new version. A reader takes the newest version
 * and reads it for as long as it likes: what it holds is never written again and never goes away under it. Versions
 * share every node that did not change. Nothing waits and nothing retries.
 *
 * The library never allocates memory. You feed it slabs, it tells you when it wants more, and you take back the ones
 * it no longer uses.
 *
 * Quick example:
 *
 *   struct window {
 *           struct poor_world_node node;
 *           int x, y;
 *   };
 *
 *   struct frame {
 *           struct poor_world_version version;
 *           uint64_t focus;
 *   };
 *
 *   poor_world_declare(scene, struct window, node, struct frame, version, 8);
 *
 *   static scene s;
 *
 *   // The writer:
 *   struct window *window;
 *
 *   poor_world_init(&s, 65536)->focus = 0;
 *   poor_world_begin(&s)->focus = 1;
 *   while (!(window = poor_world_create(&s, POOR_WORLD_NO_HANDLE, 0)))
 *           poor_world_feed(&s, aligned_alloc(65536, poor_world_wanted(&s)), poor_world_wanted(&s));
 *   window->x = window->y = 0;
 *   poor_world_publish(&s);
 *
 *   // A reader, in another thread:
 *   struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
 *   const struct frame *frame = poor_world_latest(&s, &hold);
 *   const struct window *root = poor_world_root(&s, frame);
 *
 *   for (uint32_t i = 0; i < root->node.child_count; i++)
 *           draw(poor_world_child(&s, root, i));
 *
 * Arguments are not checked: a handle that is not alive, an index out of range, a second root or a node moved into
 * its own subtree is caught only by assert().
 *
 * Except for poor_world_init(), no macro evaluates an argument more than once.
 */

constexpr uint32_t POOR_WORLD_NO_HANDLE = UINT32_MAX;
constexpr size_t POOR_WORLD_MIN_SLAB = 4096;

/*
 * Put this into your node.
 *
 * 'handle' names the node in every version until the node is destroyed; handles of destroyed nodes are used again.
 * 'id' is never used again, so it tells a node from a later one with the same handle. 'born' is the epoch of the
 * version this copy of the node was made for.
 */
struct poor_world_node {
	uint64_t born;
	uint64_t id;
	uint32_t handle;
	uint32_t child_count;
};

/*
 * Put this into your version, next to whatever belongs to the whole tree.
 *
 * 'epoch' is 0 for the empty tree and grows by one with every publish. Every handle in the version is below
 * 'handle_bound'. Get the root with poor_world_root().
 */
struct poor_world_version {
	uint64_t epoch;
	struct poor_world_node *root;
	uint32_t node_count;
	uint32_t handle_bound;
};

/* 'bytes' the tree has, 'idle_bytes' of them unused and ready for poor_world_take(), 'nodes' and 'versions' alive. */
struct poor_world_stats {
	size_t bytes;
	size_t idle_bytes;
	uint64_t nodes;
	uint32_t versions;
};

/*
 * Declare tree type 'name'.
 *
 * 'node_type' is your node, with a struct poor_world_node member called 'node_member' anywhere in it.
 * 'version_type' is what readers get, with a struct poor_world_version member called 'version_member'.
 * 'versions' is how many versions can be alive at once: one for every hold, one for every pin, plus 2.
 */
#define poor_world_declare(name, node_type, node_member, version_type, version_member, versions) \
	h_world_declare(name, h_world_cat(h_world_meta_, name), node_type, node_member, version_type, version_member, versions)

/*
 * Start an empty tree. 'slab_bytes' is a power of two, at least POOR_WORLD_MIN_SLAB.
 * Returns the empty version, so you can set your own fields in it.
 *
 * The tree keeps pointers into itself: do not copy or move it after this.
 */
#define poor_world_init(w, slab_bytes)									\
	((h_world_version_type(w) *)h_world_init(h_world_of(w), (w)->refs, (w)->slots, (w)->versions,	\
						 h_world_layout_of(w), slab_bytes))

/* Forget the tree. After this poor_world_take() returns everything that was fed. */
#define poor_world_fini(w) h_world_fini(h_world_of(w))

/*
 * Memory.
 *
 * poor_world_create(), poor_world_destroy(), poor_world_edit() and poor_world_move() fail when they need memory the
 * tree does not have. A failed operation changes nothing: feed and call it again, or go on without it.
 */

/* Give 'bytes' of 'memory', aligned to the slab size. 'bytes' is the slab size or what poor_world_wanted() said. */
#define poor_world_feed(w, memory, bytes) h_world_feed(h_world_of(w), memory, bytes)

/* After an operation failed: how many bytes the next poor_world_feed() should give. */
#define poor_world_wanted(w) h_world_wanted(h_world_of(w))

/* Take back memory that is not used and store its size in '*bytes'. Returns nullptr if there is none. */
#define poor_world_take(w, bytes) h_world_take(h_world_of(w), bytes)

/* Returns a struct poor_world_stats. */
#define poor_world_stats(w) h_world_stats(h_world_of(w))

/*
 * The writer. All of these are called from one thread.
 */

/* Begin a transaction. Returns the next version: a copy of the current one, for you to change. */
#define poor_world_begin(w) ((h_world_version_type(w) *)h_world_begin(h_world_of(w)))

/* Returns the next version of the open transaction. */
#define poor_world_next(w) ((h_world_version_type(w) *)h_world_next(h_world_of(w), h_world_version_bytes(w)))

/* Make the next version the current one. */
#define poor_world_publish(w) h_world_publish(h_world_of(w))

/* Drop everything the transaction did. */
#define poor_world_abort(w) h_world_abort(h_world_of(w))

/* Free the versions that readers let go of. poor_world_publish() does this too. */
#define poor_world_reclaim(w) h_world_reclaim(h_world_of(w))

/*
 * Add a node as child 'index' of 'parent', or as the root when 'parent' is POOR_WORLD_NO_HANDLE.
 * Returns the node with your fields not set, or nullptr when memory is needed.
 */
#define poor_world_create(w, parent, index) \
	((h_world_node_type(w) *)h_world_item(h_world_create(h_world_of(w), parent, index), h_world_node_hook(w)))

/* Remove a node and everything below it. Returns false when memory is needed. */
#define poor_world_destroy(w, handle) h_world_destroy(h_world_of(w), handle)

/*
 * Returns a node you may write to, or nullptr when memory is needed.
 * After this, poor_world_edit() of any ancestor of the node needs no memory until the transaction ends.
 */
#define poor_world_edit(w, handle) \
	((h_world_node_type(w) *)h_world_item(h_world_edit(h_world_of(w), handle), h_world_node_hook(w)))

/*
 * Make a node child 'index' of 'parent': its own parent to reorder, another one to move the whole subtree.
 * Returns false when memory is needed.
 */
#define poor_world_move(w, handle, parent, index) h_world_move(h_world_of(w), handle, parent, index)

/* Returns the node as the writer sees it, or nullptr if there is no such node. */
#define poor_world_node(w, handle) \
	((const h_world_node_type(w) *)h_world_item_const(h_world_find(h_world_of(w), handle), h_world_node_hook(w)))

/* Returns the handle of the parent, POOR_WORLD_NO_HANDLE for the root. */
#define poor_world_parent(w, handle) h_world_parent(h_world_of(w), handle)

/* Returns which child of its parent the node is. */
#define poor_world_index(w, handle) h_world_index_of(h_world_of(w), handle)

/* Returns the published version. */
#define poor_world_current(w) \
	((const h_world_version_type(w) *)h_world_current(h_world_of(w), h_world_version_bytes(w)))

/* Returns the epoch of the oldest version that is published or under a hold. */
#define poor_world_oldest_held(w) h_world_oldest_held(h_world_of(w))

/*
 * Readers. A struct poor_world_hold or struct poor_world_pin belongs to one thread at a time.
 *
 * Nothing a reader can reach is written after it was published. The writer learns that a version was let go when it
 * publishes or calls poor_world_reclaim().
 */

/* What a reader takes versions with. Start it with POOR_WORLD_HOLD_INIT. */
struct poor_world_hold;

/* What a reader keeps an older version with. Start it with POOR_WORLD_PIN_INIT. */
struct poor_world_pin;

#define POOR_WORLD_HOLD_INIT { .fan = POOR_FAN_HOLD_INIT }
#define POOR_WORLD_PIN_INIT { .fan = POOR_FAN_PIN_INIT }

/* Take the newest version and let go of the one the hold had. Returns the version. */
#define poor_world_latest(w, hold) \
	((const h_world_version_type(w) *)h_world_latest(h_world_of(w), h_world_version_bytes(w), hold))

/* Let go of the version the hold has. */
#define poor_world_release(w, hold) h_world_release(h_world_of(w), hold)

/* Returns the version the hold has, or nullptr. */
#define poor_world_held(w, hold) \
	((const h_world_version_type(w) *)h_world_version(h_world_of(w), h_world_version_bytes(w), (hold)->fan.slot))

/* Keep the version the hold has now, after the hold moves on. */
#define poor_world_pin(w, hold, pin) h_world_pin(h_world_of(w), hold, pin)

/* Let go of the version the pin has. */
#define poor_world_unpin(w, pin) h_world_unpin(h_world_of(w), pin)

/* Returns the version the pin has, or nullptr. */
#define poor_world_pinned(w, pin) \
	((const h_world_version_type(w) *)h_world_version(h_world_of(w), h_world_version_bytes(w), (pin)->fan.slot))

/* Returns the root of 'version', or nullptr for an empty tree. */
#define poor_world_root(w, version) \
	((const h_world_node_type(w) *)h_world_item_const(h_world_version_of(w, version)->root, h_world_node_hook(w)))

/* Returns child 'index' of 'node'. */
#define poor_world_child(w, node, index)										\
	((const h_world_node_type(w) *)h_world_item_const(								\
		h_world_child(h_world_node_of(w, node), h_world_children_at(w), index), h_world_node_hook(w)))

/* Returns all children of 'node': an array of pointers to their struct poor_world_node. */
#define poor_world_children(w, node) h_world_children(h_world_node_of(w, node), h_world_children_at(w))

/*
 * What changed between two versions.
 *
 * A struct poor_world_diff walks the children of the same node in two versions:
 *
 *   poor_world_diff_begin(&s, &diff, &scratch, old, next, false);
 *   while (poor_world_diff_next(&diff)) {
 *           const struct window *was = poor_world_diff_old(&s, &diff), *is = poor_world_diff_new(&s, &diff);
 *
 *           if (!was)
 *                   appeared(is);
 *           else if (!is)
 *                   vanished(was);
 *           else if (poor_world_diff_moved(&diff))
 *                   restacked(was, is);
 *           else
 *                   changed(was, is);    // go into the children with another struct poor_world_diff
 *   }
 *
 * A child that is the same node in both versions is skipped with everything below it, unless 'all' is true.
 * Children are "moved" when their order among the others changed; the fewest that explain the new order are picked.
 * Run the loop to its end or call poor_world_diff_stop().
 */

/* One walk. */
struct poor_world_diff;

/* The memory the walks of one thread work in: an array of uint32_t and an array of struct poor_world_diff_match. */
struct poor_world_diff_scratch;
struct poor_world_diff_match;

/*
 * Give a scratch its memory. One scratch serves one thread, for any number of walks.
 * 'slot_count' is at least the larger 'handle_bound' of the two versions, 'match_count' at least the sum of their
 * 'node_count'.
 */
void poor_world_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count, uint32_t *slots,
				  uint32_t match_count, struct poor_world_diff_match *matches);

/* Start walking the children of 'old' and 'next', which are the same node in two versions. */
#define poor_world_diff_begin(w, diff, scratch, old, next, all) \
	h_world_diff_begin(diff, scratch, h_world_node_of(w, old), h_world_node_of(w, next), h_world_children_at(w), all)

/* Step to the next change. Returns false when there are no more. */
bool poor_world_diff_next(struct poor_world_diff *diff);

/* Returns the child in the old version, or nullptr if it was added. */
#define poor_world_diff_old(w, diff) \
	((const h_world_node_type(w) *)h_world_item_const((diff)->old_node, h_world_node_hook(w)))

/* Returns the child in the new version, or nullptr if it was removed. */
#define poor_world_diff_new(w, diff) \
	((const h_world_node_type(w) *)h_world_item_const((diff)->new_node, h_world_node_hook(w)))

/* Returns true if the child is in both versions and its order among the others changed. */
#define poor_world_diff_moved(diff) ((bool)(diff)->moved)

/* End a walk before poor_world_diff_next() returned false. */
void poor_world_diff_stop(struct poor_world_diff *diff);

/* Implementation details, subject to change. */

constexpr uint32_t H_WORLD_NO_PAGE = UINT32_MAX;
constexpr uint32_t H_WORLD_PAGE_ENTRIES = 128;
constexpr uint32_t H_WORLD_ORDERS = 120;
constexpr size_t H_WORLD_ALIGN = 64;
constexpr size_t H_WORLD_HEADER = 16;

#define h_world_cat(a, ...) h_world_primitive_cat(a, __VA_ARGS__)
#define h_world_primitive_cat(a, ...) a ## __VA_ARGS__

struct poor_world_hold {
	struct poor_fan_hold fan;
};

struct poor_world_pin {
	struct poor_fan_pin fan;
};

struct poor_world_diff_match {
	uint32_t old_index;
	uint32_t new_index;
	uint32_t tail;
	uint32_t prev;
	bool stable;
};

struct poor_world_diff_scratch {
	uint32_t *slots;
	struct poor_world_diff_match *matches;
	uint32_t slot_count;
	uint32_t match_count;
	uint32_t top;
};

struct poor_world_diff {
	const struct poor_world_node *old_node;
	const struct poor_world_node *new_node;
	struct poor_world_diff_scratch *scratch;
	struct poor_world_node *const *old;
	struct poor_world_node *const *new;
	uint32_t old_count;
	uint32_t new_count;
	uint32_t head;
	uint32_t tail;
	uint32_t base;
	uint32_t common;
	uint32_t removed;
	uint32_t at;
	uint8_t phase;
	bool all;
	bool moved;
};

struct h_world_slot {
	struct poor_world_node *retired;
	uint64_t id_high;
	uint32_t older;
};

struct h_world_layout {
	uint32_t slot_count;
	uint32_t version_bytes;
	uint32_t version_hook;
	uint32_t node_header;
	uint32_t node_children;
	uint32_t block_bytes;
};

poor_list_declare(h_world_spans);
poor_list_declare(h_world_fed);

struct h_world_memory {
	size_t slab_bytes;
	size_t wanted;
	size_t bytes;
	size_t idle_bytes;
	h_world_spans partial[H_WORLD_ORDERS];
	h_world_spans empty;
	h_world_spans empty_big;
	h_world_fed fed;
};

struct h_world_entry {
	struct poor_world_node *node;
	uint32_t parent;
	uint32_t slot;
};

struct h_world_page {
	struct h_world_entry *entries;
	uint32_t live;
	uint32_t free;
	uint32_t prev;
	uint32_t next;
};

struct h_world_index {
	struct h_world_page *pages;
	uint32_t page_count;
	uint32_t capacity;
	uint32_t partial;
	uint32_t vacant;
};

poor_fan_declare(h_world_fan);

struct h_world {
	h_world_fan fan;
	alignas(H_WORLD_ALIGN) struct poor_fan_ref *refs;
	struct h_world_slot *slots;
	void *versions;
	struct h_world_layout layout;
	alignas(H_WORLD_ALIGN) uint32_t published;
	uint32_t free_last;
	uint32_t next;
	uint32_t dead;
	uint32_t alive;
	uint32_t stale;
	struct poor_world_version *version;
	uint64_t epoch;
	struct poor_world_node *retired;
	struct poor_world_node *born;
	uint64_t nodes;
	struct h_world_index index;
	struct h_world_memory memory;
};

poor_fan_attach(h_world_fan, struct h_world, fan, refs, published);

static_assert(offsetof(struct h_world, refs) == H_WORLD_ALIGN
	      && offsetof(struct h_world, published) == 2 * H_WORLD_ALIGN,
	      "what readers load must be on a cache line that nothing writes after poor_world_init()");

#define h_world_declare(name, meta, node_type, node_member, version_type, version_member, version_count)		\
	static_assert((version_count) >= 2 && (version_count) <= POOR_FAN_MAX_SLOTS);					\
	static_assert(alignof(node_type) <= H_WORLD_ALIGN && alignof(node_type) >= alignof(void *));			\
	static_assert(alignof(version_type) <= H_WORLD_ALIGN);								\
	static_assert(_Generic(&((node_type *)nullptr)->node_member, struct poor_world_node *: 1, default: 0),		\
		      "node member (" #node_member ") must be an unqualified struct poor_world_node");			\
	static_assert(_Generic(&((version_type *)nullptr)->version_member, struct poor_world_version *: 1, default: 0),	\
		      "version member (" #version_member ") must be an unqualified struct poor_world_version");		\
	struct meta {													\
		node_type *h_node;											\
		version_type *h_version;										\
		unsigned char h_node_hook[offsetof(node_type, node_member) + 1];					\
		unsigned char h_version_hook[offsetof(version_type, version_member) + 1];				\
	};														\
	typedef struct name {												\
		union {													\
			struct h_world world;										\
			struct meta *h_meta;										\
		};													\
		struct poor_fan_ref refs[version_count];									\
		struct h_world_slot slots[version_count];								\
		struct {												\
			alignas(H_WORLD_ALIGN) version_type version;							\
		} versions[version_count];										\
	} name

#define h_world_of(w) (&(w)->world)
#define h_world_node_type(w) typeof_unqual(*(w)->h_meta->h_node)
#define h_world_version_type(w) typeof_unqual(*(w)->h_meta->h_version)
#define h_world_node_hook(w) (sizeof((w)->h_meta->h_node_hook) - 1)
#define h_world_version_hook(w) (sizeof((w)->h_meta->h_version_hook) - 1)
#define h_world_node_bytes(w) \
	((sizeof(h_world_node_type(w)) + alignof(void *) - 1) / alignof(void *) * alignof(void *))
#define h_world_children_at(w) (h_world_node_bytes(w) - h_world_node_hook(w))
#define h_world_slot_count(w) (sizeof((w)->refs) / sizeof((w)->refs[0]))
#define h_world_version_bytes(w) sizeof((w)->versions[0])

#define h_world_node_base(w)												\
	((H_WORLD_HEADER + alignof(h_world_node_type(w)) - 1) / alignof(h_world_node_type(w))				\
	 * alignof(h_world_node_type(w)))

#define h_world_layout_of(w) ((struct h_world_layout){									\
	.slot_count = h_world_slot_count(w),										\
	.version_bytes = h_world_version_bytes(w),									\
	.version_hook = h_world_version_hook(w),									\
	.node_header = h_world_node_base(w) + h_world_node_hook(w),							\
	.node_children = h_world_children_at(w),									\
	.block_bytes = h_world_node_base(w) + h_world_node_bytes(w),							\
})

#define h_world_node_of(w, node)										\
	h_world_node_in(_Generic((node), h_world_node_type(w) *: (node), const h_world_node_type(w) *: (node)),	\
			h_world_node_hook(w))

#define h_world_version_of(w, version)											\
	h_world_version_in(_Generic((version), h_world_version_type(w) *: (version),					\
				    const h_world_version_type(w) *: (version)),					\
			   h_world_version_hook(w))

void *h_world_init(struct h_world *world, struct poor_fan_ref *refs, struct h_world_slot *slots, void *versions,
		   struct h_world_layout layout, size_t slab_bytes);
void h_world_fini(struct h_world *world);
void h_world_feed(struct h_world *world, void *memory, size_t bytes);
[[nodiscard]] void *h_world_take(struct h_world *world, size_t *bytes);
struct poor_world_stats h_world_stats(const struct h_world *world);
void *h_world_begin(struct h_world *world);
void h_world_publish(struct h_world *world);
void h_world_abort(struct h_world *world);
void h_world_reclaim(struct h_world *world);
[[nodiscard]] struct poor_world_node *h_world_create(struct h_world *world, uint32_t parent, uint32_t index);
[[nodiscard]] bool h_world_destroy(struct h_world *world, uint32_t handle);
[[nodiscard]] struct poor_world_node *h_world_copy(struct h_world *world, uint32_t handle);
[[nodiscard]] bool h_world_move(struct h_world *world, uint32_t handle, uint32_t parent, uint32_t index);
uint32_t h_world_index_of(struct h_world *world, uint32_t handle);
uint64_t h_world_oldest_held(const struct h_world *world);
void h_world_diff_begin(struct poor_world_diff *diff, struct poor_world_diff_scratch *scratch,
			const struct poor_world_node *old, const struct poor_world_node *next, size_t children_at, bool all);

static inline void *
h_world_item(struct poor_world_node *node, size_t hook)
{
	return node != nullptr ? (unsigned char *)node - hook : nullptr;
}

static inline const void *
h_world_item_const(const struct poor_world_node *node, size_t hook)
{
	return node != nullptr ? (const unsigned char *)node - hook : nullptr;
}

static inline const struct poor_world_node *
h_world_node_in(const void *item, size_t hook)
{
	return (const void *)((const unsigned char *)item + hook);
}

static inline const struct poor_world_version *
h_world_version_in(const void *item, size_t hook)
{
	return (const void *)((const unsigned char *)item + hook);
}

static inline struct poor_world_node *const *
h_world_children(const struct poor_world_node *node, size_t children_at)
{
	return (const void *)((const unsigned char *)node + children_at);
}

static inline const struct poor_world_node *
h_world_child(const struct poor_world_node *node, size_t children_at, uint32_t index)
{
	assert(index < node->child_count);
	return h_world_children(node, children_at)[index];
}

static inline struct poor_world_node *
h_world_find(const struct h_world *world, uint32_t handle)
{
	const struct h_world_index *index = &world->index;
	const uint32_t page = handle / H_WORLD_PAGE_ENTRIES;

	if (page >= index->page_count || index->pages[page].entries == nullptr)
		return nullptr;
	return index->pages[page].entries[handle % H_WORLD_PAGE_ENTRIES].node;
}

static inline struct h_world_entry *
h_world_entry(const struct h_world *world, uint32_t handle)
{
	assert(h_world_find(world, handle) != nullptr);
	return &world->index.pages[handle / H_WORLD_PAGE_ENTRIES].entries[handle % H_WORLD_PAGE_ENTRIES];
}

static inline uint32_t
h_world_parent(const struct h_world *world, uint32_t handle)
{
	return h_world_entry(world, handle)->parent;
}

[[nodiscard]] static inline struct poor_world_node *
h_world_edit(struct h_world *world, uint32_t handle)
{
	struct poor_world_node *node = h_world_entry(world, handle)->node;

	assert(world->next != POOR_FAN_NO_SLOT);
	return node->born == world->epoch ? node : h_world_copy(world, handle);
}

static inline size_t
h_world_wanted(const struct h_world *world)
{
	return world->memory.wanted;
}

static inline void *
h_world_version(const struct h_world *world, size_t version_bytes, uint32_t slot)
{
	return slot != POOR_FAN_NO_SLOT ? (unsigned char *)world->versions + (size_t)slot * version_bytes : nullptr;
}

static inline void *
h_world_current(const struct h_world *world, size_t version_bytes)
{
	return h_world_version(world, version_bytes, poor_fan_published(&world->fan));
}

static inline void *
h_world_next(struct h_world *world, size_t version_bytes)
{
	assert(world->next != POOR_FAN_NO_SLOT);
	return h_world_version(world, version_bytes, world->next);
}

static inline void
h_world_release(struct h_world *world, struct poor_world_hold *hold)
{
	poor_fan_release(&world->fan, &hold->fan);
}

static inline void *
h_world_latest(struct h_world *world, size_t version_bytes, struct poor_world_hold *hold)
{
	return h_world_version(world, version_bytes, poor_fan_latest(&world->fan, &hold->fan));
}

static inline void
h_world_unpin(struct h_world *world, struct poor_world_pin *pin)
{
	poor_fan_unpin(&world->fan, &pin->fan);
}

static inline void
h_world_pin(struct h_world *world, const struct poor_world_hold *hold, struct poor_world_pin *pin)
{
	poor_fan_pin(&world->fan, &hold->fan, &pin->fan);
}

#endif
