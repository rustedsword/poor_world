/* SPDX-License-Identifier: MIT */
#ifndef POOR_WORLD_H
#define POOR_WORLD_H

#include <assert.h>
#include <stdbit.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <poor_asan.h>
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
 * Only one transaction at a time is supported. A tree has one writer, and that writer has at most one transaction
 * open: it publishes or aborts it before it begins the next one.
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
 *   poor_world_declare(scene, struct window, node, struct frame, version, 8);    // in a header
 *   poor_world_define(scene);                                                     // in one .c file
 *
 *   static scene s;
 *
 *   // The writer:
 *   struct window *window;
 *
 *   scene_init(&s, 65536)->focus = 0;
 *   scene_begin(&s)->focus = 1;
 *   while (!(window = scene_create(&s, POOR_WORLD_NO_HANDLE, 0)))
 *           scene_feed(&s, aligned_alloc(65536, scene_wanted(&s)), scene_wanted(&s));
 *   window->x = window->y = 0;
 *   scene_publish(&s);
 *
 *   // A reader, in another thread:
 *   struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
 *   const struct frame *frame = scene_latest(&s, &hold);
 *   const struct window *root = scene_root(frame);
 *
 *   for (uint32_t i = 0; i < root->node.child_count; i++)
 *           draw(scene_child(root, i));
 *
 * Nothing is checked, neither the arguments nor the order of calls. A handle that is not alive, an index out of
 * range, a second root, a node moved into its own subtree, a scene_begin() while a transaction is open or a change
 * with none open is caught only by assert().
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
 * 'handle_bound'. Get the root with name_root().
 */
struct poor_world_version {
	uint64_t epoch;
	struct poor_world_node *root;
	uint32_t node_count;
	uint32_t handle_bound;
};

/*
 * 'bytes' the tree has, 'idle_bytes' of them unused and ready for name_take(), 'nodes' and 'versions' alive.
 * The nodes an open transaction made are counted, its next version is not.
 */
struct poor_world_stats {
	size_t bytes;
	size_t idle_bytes;
	uint64_t nodes;
	uint32_t versions;
};

/* What a reader takes versions with. Start it with POOR_WORLD_HOLD_INIT. */
struct poor_world_hold;

/* What a reader keeps an older version with. Start it with POOR_WORLD_PIN_INIT. */
struct poor_world_pin;

#define POOR_WORLD_HOLD_INIT { .fan = POOR_FAN_HOLD_INIT }
#define POOR_WORLD_PIN_INIT { .fan = POOR_FAN_PIN_INIT }

/* One walk over what changed between two versions. */
struct poor_world_diff;

/* The memory the walks of one thread work in: an array of uint32_t and an array of struct poor_world_diff_match. */
struct poor_world_diff_scratch;
struct poor_world_diff_match;

/*
 * Declare tree type 'name' and its functions: put this into a header.
 *
 * 'node_type' is your node, with a struct poor_world_node member called 'node_member' anywhere in it.
 * 'version_type' is what readers get, with a struct poor_world_version member called 'version_member'.
 * 'versions' is how many versions can be alive at once: one for every hold, one for every pin, plus 2.
 *
 * The functions are named after the tree: name_init(), name_begin() and so on. All of them are listed below, the
 * way they look for the 'scene' of the quick example.
 */
#define poor_world_declare(name, node_type, node_member, version_type, version_member, versions) \
	h_world_declare(name, node_type, node_member, version_type, version_member, versions)

/*
 * Create the functions of tree type 'name': put this into one .c file, after poor_world_declare().
 *
 * The functions are built for your node and version types, with the options of that file: its assert() checks the
 * arguments, its AddressSanitizer watches the memory of the tree.
 */
#define poor_world_define(name) h_world_define(name, name##_h_node, name##_h_version)

/*
 * struct frame *scene_init(scene *tree, size_t slab_bytes);
 *
 *     Start an empty tree. 'slab_bytes' is a power of two, at least POOR_WORLD_MIN_SLAB.
 *     Returns the empty version, so you can set your own fields in it.
 *     The tree keeps pointers into itself: do not copy or move it after this.
 *
 * void scene_fini(scene *tree);
 *
 *     Forget the tree. After this scene_take() returns everything that was fed.
 *     No transaction may be open: end it first.
 *
 *
 * Memory.
 *
 * scene_create(), scene_destroy(), scene_edit() and scene_move() fail when they need memory the tree does not have.
 * A failed operation changes nothing: feed and call it again, or go on without it.
 *
 * void scene_feed(scene *tree, void *memory, size_t bytes);
 *
 *     Give 'bytes' of 'memory', aligned to the slab size. 'bytes' is the slab size or what scene_wanted() said.
 *
 * size_t scene_wanted(const scene *tree);
 *
 *     After an operation failed: how many bytes the next scene_feed() should give.
 *
 * void *scene_take(scene *tree, size_t *bytes);
 *
 *     Take back memory that is not used and store its size in '*bytes'. Returns nullptr if there is none.
 *
 * struct poor_world_stats scene_stats(const scene *tree);
 *
 *     Returns how much memory the tree has and how much of it is in use: see struct poor_world_stats.
 *
 *
 * The writer. All of these are called from one thread.
 *
 * The tree is changed inside a transaction, and a tree has one transaction at a time. scene_begin() opens it,
 * scene_publish() or scene_abort() ends it. There is no transaction object: the transaction is a state of the tree.
 * The tree does not tell you whether one is open, so remember it yourself if you need to know.
 *
 * scene_next(), scene_create(), scene_destroy(), scene_edit(), scene_move(), scene_publish() and scene_abort() need
 * an open transaction. scene_begin() and scene_fini() need that none is open. Everything else works either way.
 *
 * Readers see nothing of a transaction before scene_publish(), and nothing ever of one that was aborted.
 *
 * A pointer to a node, from scene_create() or scene_edit(), is good until the transaction ends or the node gets
 * another child, whichever comes first: a node that gets a child may move to a larger block. Keep the handle and
 * call scene_edit() again. For a node the transaction already made or edited that needs no memory and cannot fail.
 *
 * struct frame *scene_begin(scene *tree);
 *
 *     Begin a transaction. Returns the next version: a copy of the current one, for you to change.
 *
 * struct frame *scene_next(scene *tree);
 *
 *     Returns the next version of the open transaction.
 *
 * void scene_publish(scene *tree);
 *
 *     End the transaction and make the next version the current one. Readers can take it from here on: do not
 *     write to it or to its nodes any more.
 *
 * void scene_abort(scene *tree);
 *
 *     End the transaction and drop everything it did. The next version and the nodes it made are gone.
 *
 * void scene_reclaim(scene *tree);
 *
 *     Free the versions that readers let go of. scene_publish() does this too.
 *
 * struct window *scene_create(scene *tree, uint32_t parent, uint32_t index);
 *
 *     Add a node as child 'index' of 'parent', or as the root when 'parent' is POOR_WORLD_NO_HANDLE.
 *     Returns the node with your fields not set, or nullptr when memory is needed.
 *
 * bool scene_destroy(scene *tree, uint32_t handle);
 *
 *     Remove a node and everything below it. Returns false when memory is needed.
 *
 * struct window *scene_edit(scene *tree, uint32_t handle);
 *
 *     Returns a node you may write to, or nullptr when memory is needed.
 *     After this, scene_edit() of any ancestor of the node needs no memory until the transaction ends.
 *
 * bool scene_move(scene *tree, uint32_t handle, uint32_t parent, uint32_t index);
 *
 *     Make a node child 'index' of 'parent': its own parent to reorder, another one to move the whole subtree.
 *     Returns false when memory is needed.
 *
 * const struct window *scene_node(const scene *tree, uint32_t handle);
 *
 *     Returns the node as the writer sees it, with what the open transaction did, or nullptr if there is no such
 *     node. The pointer is good until the tree is changed again or the transaction is aborted.
 *
 * uint32_t scene_parent(const scene *tree, uint32_t handle);
 *
 *     Returns the handle of the parent, POOR_WORLD_NO_HANDLE for the root.
 *
 * uint32_t scene_index(scene *tree, uint32_t handle);
 *
 *     Returns which child of its parent the node is.
 *
 * const struct frame *scene_current(const scene *tree);
 *
 *     Returns the published version.
 *
 * uint64_t scene_oldest_held(const scene *tree);
 *
 *     Returns the epoch of the oldest version that is published or under a hold.
 *
 *
 * Readers. A struct poor_world_hold or struct poor_world_pin belongs to one thread at a time.
 *
 * Nothing a reader can reach is written after it was published. The writer learns that a version was let go when it
 * publishes or calls scene_reclaim().
 *
 * const struct frame *scene_latest(scene *tree, struct poor_world_hold *hold);
 *
 *     Take the newest version and let go of the one the hold had. Returns the version.
 *
 * void scene_release(scene *tree, struct poor_world_hold *hold);
 *
 *     Let go of the version the hold has.
 *
 * const struct frame *scene_held(const scene *tree, const struct poor_world_hold *hold);
 *
 *     Returns the version the hold has, or nullptr.
 *
 * void scene_pin(scene *tree, const struct poor_world_hold *hold, struct poor_world_pin *pin);
 *
 *     Keep the version the hold has now, after the hold moves on.
 *
 * void scene_unpin(scene *tree, struct poor_world_pin *pin);
 *
 *     Let go of the version the pin has.
 *
 * const struct frame *scene_pinned(const scene *tree, const struct poor_world_pin *pin);
 *
 *     Returns the version the pin has, or nullptr.
 *
 * const struct window *scene_root(const struct frame *version);
 *
 *     Returns the root of 'version', or nullptr for an empty tree.
 *
 * const struct window *scene_child(const struct window *node, uint32_t index);
 *
 *     Returns child 'index' of 'node'.
 *
 * struct poor_world_node *const *scene_children(const struct window *node);
 *
 *     Returns all children of 'node': an array of pointers to their struct poor_world_node.
 *
 *
 * What changed between two versions.
 *
 * A struct poor_world_diff walks the children of the same node in two versions:
 *
 *   scene_diff_begin(&diff, &scratch, old, next, false);
 *   while (scene_diff_next(&diff)) {
 *           const struct window *was = scene_diff_old(&diff), *is = scene_diff_new(&diff);
 *
 *           if (!was)
 *                   appeared(is);
 *           else if (!is)
 *                   vanished(was);
 *           else if (scene_diff_moved(&diff))
 *                   restacked(was, is);
 *           else
 *                   changed(was, is);    // go into the children with another struct poor_world_diff
 *   }
 *
 * A child that is the same node in both versions is skipped with everything below it, unless 'all' is true.
 * Children are "moved" when their order among the others changed; the fewest that explain the new order are picked.
 * Run the loop to its end or call scene_diff_stop().
 *
 * void scene_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count, uint32_t *slots,
 *                              uint32_t match_count, struct poor_world_diff_match *matches);
 *
 *     Give a scratch its memory. One scratch serves one thread, for any number of walks.
 *     'slot_count' is at least the larger 'handle_bound' of the two versions, 'match_count' at least the sum of
 *     their 'node_count'.
 *
 * void scene_diff_begin(struct poor_world_diff *diff, struct poor_world_diff_scratch *scratch,
 *                       const struct window *old, const struct window *next, bool all);
 *
 *     Start walking the children of 'old' and 'next', which are the same node in two versions.
 *
 * bool scene_diff_next(struct poor_world_diff *diff);
 *
 *     Step to the next change. Returns false when there are no more.
 *
 * const struct window *scene_diff_old(const struct poor_world_diff *diff);
 *
 *     Returns the child in the old version, or nullptr if it was added.
 *
 * const struct window *scene_diff_new(const struct poor_world_diff *diff);
 *
 *     Returns the child in the new version, or nullptr if it was removed.
 *
 * bool scene_diff_moved(const struct poor_world_diff *diff);
 *
 *     Returns true if the child is in both versions and its order among the others changed.
 *
 * void scene_diff_stop(struct poor_world_diff *diff);
 *
 *     End a walk before scene_diff_next() returned false.
 */

/* Implementation details, subject to change. */

constexpr uint32_t H_WORLD_ORDERS = 120;
constexpr uint32_t H_WORLD_EVEN_ORDERS = 8;
constexpr uint32_t H_WORLD_ORDER_STEPS = 4;
constexpr size_t H_WORLD_ALIGN = 64;
constexpr size_t H_WORLD_HEADER = 16;
constexpr size_t H_WORLD_BLOCK_UNIT = 64;
constexpr size_t H_WORLD_SPAN_HEADER = 64;
constexpr uint32_t H_WORLD_DIFF_WIDE = 64;

#define h_world_round_up(value, to) (((value) + (to) - 1) / (to) * (to))
#define h_world_constant [[maybe_unused]] constexpr uint32_t

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

enum h_world_diff_phase : uint8_t {
	H_WORLD_DIFF_HEAD,
	H_WORLD_DIFF_TAIL,
	H_WORLD_DIFF_MIDDLE,
	H_WORLD_DIFF_ADDED,
	H_WORLD_DIFF_REMOVED,
	H_WORLD_DIFF_COMMON,
	H_WORLD_DIFF_DONE,
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
	enum h_world_diff_phase phase;
	bool all;
	bool moved;
};

enum h_world_node_state : uint8_t {
	H_WORLD_NODE_LIVE,
	H_WORLD_NODE_REPLACED,
	H_WORLD_NODE_DESTROYED,
	H_WORLD_NODE_FREE,
};

struct h_world_node_header {
	struct poor_world_node *link;
	uint8_t order;
	enum h_world_node_state state;
};

static_assert(sizeof(struct h_world_node_header) == H_WORLD_HEADER);

struct h_world_span {
	struct poor_list_node link;
	struct poor_list_node fed;
	void *free;
	size_t bytes;
	uint32_t live;
	uint32_t carved;
	uint32_t capacity;
	uint32_t order;
};

static_assert(sizeof(struct h_world_span) <= H_WORLD_SPAN_HEADER);

poor_list_define(h_world_spans, struct h_world_span, link);
poor_list_define(h_world_fed, struct h_world_span, fed);

struct h_world_slot {
	struct poor_world_node *retired;
	uint64_t id_high;
	uint32_t older;
};

struct h_world_memory {
	size_t slab_bytes;
	size_t wanted;
	size_t bytes;
	size_t idle_bytes;
	h_world_spans partial[H_WORLD_ORDERS];
	h_world_spans empty;
	h_world_spans empty_big;
	h_world_fed fed;
	bool idled;
};

struct h_world_entry {
	struct poor_world_node *node;
	uint32_t parent;
	uint32_t slot;
};

struct h_world_index {
	struct h_world_entry *entries;
	uint32_t capacity;
	uint32_t bound;
	uint32_t free;
	bool oversized;
};

poor_fan_declare(h_world_fan);

struct h_world {
	h_world_fan fan;
	alignas(H_WORLD_ALIGN) struct poor_fan_ref *refs;
	struct h_world_slot *slots;
	void *versions;
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
	      "what readers load must be on a cache line that nothing writes after the tree is started");

/* What the code knows of one tree type: sizes and offsets of its node and version, and its copies of the three
 * large functions that several operations share. Every function that takes it is always inlined, down into a
 * function that poor_world_define() makes for that type, so all of this is constant there. */
struct h_world_type {
	uint32_t version_bytes;
	uint32_t version_hook;
	uint32_t node_header;
	uint32_t node_children;
	struct poor_world_node *(*touch)(struct h_world *world, uint32_t handle, size_t capacity);
	void (*reclaim)(struct h_world *world, bool fresh);
	void (*unwind)(struct h_world *world, const struct poor_world_node *until);
};

#define h_world_declare(name, node_type, node_member, version_type, version_member, version_count)			\
	static_assert((version_count) >= 2 && (version_count) <= POOR_FAN_MAX_SLOTS);					\
	static_assert(alignof(node_type) <= H_WORLD_ALIGN && alignof(node_type) >= alignof(void *));			\
	static_assert(alignof(version_type) <= H_WORLD_ALIGN);								\
	static_assert(_Generic(&((node_type *)nullptr)->node_member, struct poor_world_node *: 1, default: 0),		\
		      "node member (" #node_member ") must be an unqualified struct poor_world_node");			\
	static_assert(_Generic(&((version_type *)nullptr)->version_member, struct poor_world_version *: 1, default: 0),	\
		      "version member (" #version_member ") must be an unqualified struct poor_world_version");		\
	typedef node_type name##_h_node;										\
	typedef version_type name##_h_version;										\
	typedef struct name {												\
		struct h_world world;											\
		struct poor_fan_ref refs[version_count];								\
		struct h_world_slot slots[version_count];								\
		struct {												\
			alignas(H_WORLD_ALIGN) version_type version;							\
		} versions[version_count];										\
	} name;														\
	h_world_constant name##_h_versions = (version_count);								\
	h_world_constant name##_h_version_bytes = sizeof(((name *)nullptr)->versions[0]);				\
	h_world_constant name##_h_version_hook = offsetof(version_type, version_member);				\
	h_world_constant name##_h_node_hook = offsetof(node_type, node_member);						\
	h_world_constant name##_h_node_header =										\
		h_world_round_up(H_WORLD_HEADER, alignof(node_type)) + offsetof(node_type, node_member);		\
	h_world_constant name##_h_node_children =									\
		h_world_round_up(sizeof(node_type), alignof(void *)) - offsetof(node_type, node_member);		\
	h_world_declare_functions(name, node_type, node_member, version_type, version_member)

#define h_world_inline [[maybe_unused]] static inline

#define h_world_declare_functions(name, node_type, node_member, version_type, version_member)				\
	version_type *name##_init(name *tree, size_t slab_bytes);							\
	void name##_fini(name *tree);											\
	void name##_feed(name *tree, void *memory, size_t bytes);							\
	[[nodiscard]] void *name##_take(name *tree, size_t *bytes);							\
	version_type *name##_begin(name *tree);										\
	void name##_publish(name *tree);										\
	void name##_abort(name *tree);											\
	void name##_reclaim(name *tree);										\
	[[nodiscard]] node_type *name##_create(name *tree, uint32_t parent, uint32_t index);				\
	[[nodiscard]] bool name##_destroy(name *tree, uint32_t handle);							\
	[[nodiscard]] bool name##_move(name *tree, uint32_t handle, uint32_t parent, uint32_t index);			\
	uint32_t name##_index(name *tree, uint32_t handle);								\
	uint64_t name##_oldest_held(const name *tree);									\
	void name##_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count,			\
				      uint32_t *slots, uint32_t match_count,						\
				      struct poor_world_diff_match *matches);						\
	[[nodiscard]] struct poor_world_node *name##_h_copy(name *tree, uint32_t handle);				\
	bool name##_h_diff_step(struct poor_world_diff *diff);								\
	h_world_inline size_t name##_wanted(const name *tree)								\
	{														\
		return h_world_wanted(&tree->world);									\
	}														\
	h_world_inline struct poor_world_stats name##_stats(const name *tree)						\
	{														\
		return h_world_stats(&tree->world);									\
	}														\
	h_world_inline version_type *name##_next(name *tree)								\
	{														\
		return h_world_next(&tree->world, name##_h_version_bytes);						\
	}														\
	[[nodiscard]] h_world_inline node_type *name##_edit(name *tree, uint32_t handle)				\
	{														\
		struct poor_world_node *node = h_world_owned(&tree->world, handle);					\
															\
		if (node == nullptr)											\
			node = name##_h_copy(tree, handle);								\
		return h_world_item(node, name##_h_node_hook);								\
	}														\
	h_world_inline const node_type *name##_node(const name *tree, uint32_t handle)					\
	{														\
		return h_world_item_const(h_world_find(&tree->world, handle), name##_h_node_hook);			\
	}														\
	h_world_inline uint32_t name##_parent(const name *tree, uint32_t handle)					\
	{														\
		return h_world_parent(&tree->world, handle);								\
	}														\
	h_world_inline const version_type *name##_current(const name *tree)						\
	{														\
		return h_world_current(&tree->world, name##_h_version_bytes);						\
	}														\
	h_world_inline const version_type *name##_latest(name *tree, struct poor_world_hold *hold)			\
	{														\
		return h_world_latest(&tree->world, name##_h_version_bytes, hold);					\
	}														\
	h_world_inline void name##_release(name *tree, struct poor_world_hold *hold)					\
	{														\
		h_world_release(&tree->world, hold);									\
	}														\
	h_world_inline const version_type *name##_held(const name *tree, const struct poor_world_hold *hold)		\
	{														\
		return h_world_version(&tree->world, name##_h_version_bytes, hold->fan.slot);				\
	}														\
	h_world_inline void name##_pin(name *tree, const struct poor_world_hold *hold, struct poor_world_pin *pin)	\
	{														\
		h_world_pin(&tree->world, hold, pin);									\
	}														\
	h_world_inline void name##_unpin(name *tree, struct poor_world_pin *pin)					\
	{														\
		h_world_unpin(&tree->world, pin);									\
	}														\
	h_world_inline const version_type *name##_pinned(const name *tree, const struct poor_world_pin *pin)		\
	{														\
		return h_world_version(&tree->world, name##_h_version_bytes, pin->fan.slot);				\
	}														\
	h_world_inline const node_type *name##_root(const version_type *version)					\
	{														\
		return h_world_item_const(version->version_member.root, name##_h_node_hook);				\
	}														\
	h_world_inline const node_type *name##_child(const node_type *node, uint32_t index)				\
	{														\
		return h_world_item_const(h_world_child(&node->node_member, name##_h_node_children,			\
							index), name##_h_node_hook);					\
	}														\
	h_world_inline struct poor_world_node *const *name##_children(const node_type *node)				\
	{														\
		return h_world_children(&node->node_member, name##_h_node_children);					\
	}														\
	h_world_inline void name##_diff_begin(struct poor_world_diff *diff, struct poor_world_diff_scratch *scratch,	\
					     const node_type *old, const node_type *next, bool all)			\
	{														\
		h_world_diff_begin(diff, scratch, &old->node_member, &next->node_member,				\
				   name##_h_node_children, all);							\
	}														\
	h_world_inline bool name##_diff_next(struct poor_world_diff *diff)						\
	{														\
		return h_world_diff_next(diff, name##_h_diff_step);							\
	}														\
	h_world_inline const node_type *name##_diff_old(const struct poor_world_diff *diff)				\
	{														\
		return h_world_item_const(diff->old_node, name##_h_node_hook);						\
	}														\
	h_world_inline const node_type *name##_diff_new(const struct poor_world_diff *diff)				\
	{														\
		return h_world_item_const(diff->new_node, name##_h_node_hook);						\
	}														\
	h_world_inline bool name##_diff_moved(const struct poor_world_diff *diff)					\
	{														\
		return diff->moved;											\
	}														\
	h_world_inline void name##_diff_stop(struct poor_world_diff *diff)						\
	{														\
		h_world_diff_stop(diff);										\
	}														\
	static_assert(sizeof(name) != 0)

/* name_h_diff_step() starts on a cache line: started in the middle of one, its loops measured up to 7% slower. */
#define h_world_define(name, node_type, version_type)									\
	static inline struct poor_world_node *name##_h_touch(struct h_world *world, uint32_t handle,			\
							     size_t capacity);						\
	static inline void name##_h_reclaim(struct h_world *world, bool fresh);						\
	[[gnu::cold]] static inline void name##_h_unwind(struct h_world *world, const struct poor_world_node *until);	\
	static inline struct poor_world_node *name##_h_touch(struct h_world *world, uint32_t handle,			\
							     size_t capacity)						\
	{														\
		return h_world_path_touch(world, h_world_type_of(name), handle, capacity);				\
	}														\
	static inline void name##_h_reclaim(struct h_world *world, bool fresh)						\
	{														\
		h_world_reclaim_from(world, h_world_type_of(name), fresh);						\
	}														\
	static inline void name##_h_unwind(struct h_world *world, const struct poor_world_node *until)			\
	{														\
		h_world_path_unwind(world, h_world_type_of(name), until);						\
	}														\
	static inline void *name##_h_init(name *tree, size_t slab_bytes)						\
	{														\
		return h_world_init(&tree->world, h_world_type_of(name), name##_h_versions, tree->refs, tree->slots,	\
				    tree->versions, slab_bytes);							\
	}														\
	static inline void *name##_h_begin(struct h_world *world)							\
	{														\
		return h_world_begin(world, h_world_type_of(name));							\
	}														\
	static inline void name##_h_publish(struct h_world *world)							\
	{														\
		h_world_publish(world, h_world_type_of(name));								\
	}														\
	static inline void name##_h_abort(struct h_world *world)							\
	{														\
		h_world_abort(world, h_world_type_of(name));								\
	}														\
	static inline struct poor_world_node *name##_h_create(struct h_world *world, uint32_t parent, uint32_t index)	\
	{														\
		return h_world_create(world, h_world_type_of(name), parent, index);					\
	}														\
	static inline bool name##_h_destroy(struct h_world *world, uint32_t handle)					\
	{														\
		return h_world_destroy(world, h_world_type_of(name), handle);						\
	}														\
	static inline bool name##_h_move(struct h_world *world, uint32_t handle, uint32_t parent, uint32_t index)	\
	{														\
		return h_world_move(world, h_world_type_of(name), handle, parent, index);				\
	}														\
	version_type *name##_init(name *tree, size_t slab_bytes)							\
	{														\
		return name##_h_init(tree, slab_bytes);									\
	}														\
	void name##_fini(name *tree)											\
	{														\
		h_world_fini(&tree->world);										\
	}														\
	void name##_feed(name *tree, void *memory, size_t bytes)							\
	{														\
		h_world_feed(&tree->world, memory, bytes);								\
	}														\
	void *name##_take(name *tree, size_t *bytes)									\
	{														\
		return h_world_take(&tree->world, bytes);								\
	}														\
	version_type *name##_begin(name *tree)										\
	{														\
		return name##_h_begin(&tree->world);									\
	}														\
	void name##_publish(name *tree)											\
	{														\
		name##_h_publish(&tree->world);										\
	}														\
	void name##_abort(name *tree)											\
	{														\
		name##_h_abort(&tree->world);										\
	}														\
	void name##_reclaim(name *tree)											\
	{														\
		name##_h_reclaim(&tree->world, false);									\
	}														\
	node_type *name##_create(name *tree, uint32_t parent, uint32_t index)						\
	{														\
		return h_world_item(name##_h_create(&tree->world, parent, index), name##_h_node_hook);			\
	}														\
	bool name##_destroy(name *tree, uint32_t handle)								\
	{														\
		return name##_h_destroy(&tree->world, handle);								\
	}														\
	bool name##_move(name *tree, uint32_t handle, uint32_t parent, uint32_t index)					\
	{														\
		return name##_h_move(&tree->world, handle, parent, index);						\
	}														\
	uint32_t name##_index(name *tree, uint32_t handle)								\
	{														\
		return h_world_child_index(&tree->world, h_world_type_of(name), handle);				\
	}														\
	uint64_t name##_oldest_held(const name *tree)									\
	{														\
		return h_world_oldest_held(&tree->world, h_world_type_of(name));					\
	}														\
	void name##_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count,			\
				      uint32_t *slots, uint32_t match_count,						\
				      struct poor_world_diff_match *matches)						\
	{														\
		h_world_diff_scratch_init(scratch, slot_count, slots, match_count, matches);				\
	}														\
	struct poor_world_node *name##_h_copy(name *tree, uint32_t handle)						\
	{														\
		return name##_h_touch(&tree->world, handle, 0);								\
	}														\
	[[gnu::aligned(64)]] bool name##_h_diff_step(struct poor_world_diff *diff)					\
	{														\
		return h_world_diff_step(diff);										\
	}														\
	static_assert(sizeof(name) != 0)

#define h_world_type_of(name) ((struct h_world_type){									\
	.version_bytes = name##_h_version_bytes,									\
	.version_hook = name##_h_version_hook,										\
	.node_header = name##_h_node_header,										\
	.node_children = name##_h_node_children,									\
	.touch = name##_h_touch,											\
	.reclaim = name##_h_reclaim,											\
	.unwind = name##_h_unwind,											\
})

static inline size_t
h_world_block_bytes(uint32_t order)
{
	const uint32_t above = order - H_WORLD_EVEN_ORDERS;

	if (order < H_WORLD_EVEN_ORDERS)
		return H_WORLD_BLOCK_UNIT * (order + 1);
	return (H_WORLD_BLOCK_UNIT * H_WORLD_EVEN_ORDERS / H_WORLD_ORDER_STEPS << (above / H_WORLD_ORDER_STEPS))
	       * (H_WORLD_ORDER_STEPS + 1 + above % H_WORLD_ORDER_STEPS);
}

static inline uint32_t
h_world_block_order(size_t bytes)
{
	const size_t last = (bytes - 1) / H_WORLD_BLOCK_UNIT;
	const unsigned width = stdc_bit_width(last);

	assert(bytes != 0);
	if (last < H_WORLD_EVEN_ORDERS)
		return (uint32_t)last;
	return H_WORLD_ORDER_STEPS * width - 12 + (uint32_t)(last >> (width - 3));
}

static_assert(H_WORLD_EVEN_ORDERS == 8 && H_WORLD_ORDER_STEPS == 4);

static inline h_world_spans *
h_world_spans_idle(struct h_world_memory *memory, const struct h_world_span *span)
{
	return span->bytes == memory->slab_bytes ? &memory->empty : &memory->empty_big;
}

static inline void
h_world_span_idle(struct h_world_memory *memory, struct h_world_span *span)
{
	span->live = 0;
	poor_list_append(h_world_spans_idle(memory, span), span);
	memory->idle_bytes += span->bytes;
	memory->idled = true;
}

static inline struct h_world_span *
h_world_span_find(struct h_world_memory *memory, size_t bytes)
{
	if (bytes <= memory->slab_bytes - H_WORLD_SPAN_HEADER)
		return poor_list_first(&memory->empty);
	poor_list_foreach(&memory->empty_big, span) {
		if (span->bytes - H_WORLD_SPAN_HEADER >= bytes)
			return span;
	}
	return nullptr;
}

[[nodiscard, gnu::cold]] static inline struct h_world_span *
h_world_span_open(struct h_world_memory *memory, uint32_t order)
{
	const size_t bytes = h_world_block_bytes(order);
	const size_t slab = memory->slab_bytes;
	struct h_world_span *span = h_world_span_find(memory, bytes);

	if (span == nullptr) {
		memory->wanted = (bytes + H_WORLD_SPAN_HEADER + slab - 1) / slab * slab;
		return nullptr;
	}
	poor_list_remove(h_world_spans_idle(memory, span), span);
	memory->idle_bytes -= span->bytes;
	span->order = order;
	span->free = nullptr;
	span->carved = 0;
	span->capacity = 1;
	if (bytes <= slab - H_WORLD_SPAN_HEADER)
		span->capacity = (uint32_t)((span->bytes - H_WORLD_SPAN_HEADER) / bytes);
	poor_list_append(&memory->partial[order], span);
	return span;
}

[[gnu::cold]] static inline void
h_world_span_close(struct h_world_memory *memory, struct h_world_span *span)
{
	poor_list_remove(&memory->partial[span->order], span);
	h_world_span_idle(memory, span);
}

/* Tests define H_WORLD_FAULT() to make an allocation fail. */
[[nodiscard, gnu::always_inline]] static inline void *
h_world_block_alloc(struct h_world_memory *memory, uint32_t order)
{
	struct h_world_span *span = poor_list_first(&memory->partial[order]);
	void *block;

#ifdef H_WORLD_FAULT
	if (H_WORLD_FAULT()) {
		memory->wanted = 0;
		return nullptr;
	}
#endif
	if (span == nullptr) {
		span = h_world_span_open(memory, order);
		if (span == nullptr)
			return nullptr;
	}
	block = span->free;
	if (block != nullptr) {
		poor_asan_unpoison_region(block, h_world_block_bytes(order));
		span->free = *(void **)block;
	} else {
		block = (unsigned char *)span + H_WORLD_SPAN_HEADER + span->carved++ * h_world_block_bytes(order);
		poor_asan_unpoison_region(block, h_world_block_bytes(order));
	}
	if (++span->live == span->capacity)
		poor_list_remove(&memory->partial[order], span);
	return block;
}

static inline void
h_world_block_free(struct h_world_memory *memory, void *block)
{
	struct h_world_span *span = (void *)((unsigned char *)block - ((uintptr_t)block & (memory->slab_bytes - 1)));

	*(void **)block = span->free;
	span->free = block;
	poor_asan_poison_region(block, h_world_block_bytes(span->order));
	if (span->live == span->capacity)
		poor_list_prepend(&memory->partial[span->order], span);
	if (--span->live == 0)
		h_world_span_close(memory, span);
}

static inline void
h_world_memory_init(struct h_world_memory *memory, size_t slab_bytes)
{
	assert(slab_bytes >= POOR_WORLD_MIN_SLAB && stdc_has_single_bit(slab_bytes));
	memory->slab_bytes = slab_bytes;
	memory->wanted = 0;
	memory->bytes = 0;
	memory->idle_bytes = 0;
	for (uint32_t order = 0; order < H_WORLD_ORDERS; order++)
		poor_list_init(&memory->partial[order]);
	poor_list_init(&memory->empty);
	poor_list_init(&memory->empty_big);
	poor_list_init(&memory->fed);
	memory->idled = false;
}

static inline void
h_world_memory_fini(struct h_world_memory *memory)
{
	for (uint32_t order = 0; order < H_WORLD_ORDERS; order++)
		poor_list_init(&memory->partial[order]);
	poor_list_init(&memory->empty);
	poor_list_init(&memory->empty_big);
	memory->idle_bytes = 0;
	poor_list_foreach(&memory->fed, span) {
		poor_asan_poison_region((unsigned char *)span + H_WORLD_SPAN_HEADER, span->bytes - H_WORLD_SPAN_HEADER);
		h_world_span_idle(memory, span);
	}
}

static inline void
h_world_feed(struct h_world *world, void *memory, size_t bytes)
{
	struct h_world_memory *m = &world->memory;
	struct h_world_span *span = memory;

	assert(bytes >= m->slab_bytes && bytes % m->slab_bytes == 0);
	assert((uintptr_t)memory % m->slab_bytes == 0);
	span->bytes = bytes;
	poor_asan_poison_region((unsigned char *)span + H_WORLD_SPAN_HEADER, bytes - H_WORLD_SPAN_HEADER);
	m->bytes += bytes;
	poor_list_append(&m->fed, span);
	h_world_span_idle(m, span);
}

[[nodiscard]] static inline void *
h_world_take(struct h_world *world, size_t *bytes)
{
	struct h_world_memory *m = &world->memory;
	struct h_world_span *span = poor_list_first(&m->empty_big);

	if (span == nullptr)
		span = poor_list_last(&m->empty);
	if (span == nullptr)
		return nullptr;
	poor_list_remove(h_world_spans_idle(m, span), span);
	poor_list_remove(&m->fed, span);
	m->bytes -= span->bytes;
	m->idle_bytes -= span->bytes;
	*bytes = span->bytes;
	poor_asan_unpoison_region(span, span->bytes);
	return span;
}

static inline size_t
h_world_wanted(const struct h_world *world)
{
	return world->memory.wanted;
}

[[gnu::always_inline]] static inline void *
h_world_item(struct poor_world_node *node, size_t hook)
{
	return node != nullptr ? (unsigned char *)node - hook : nullptr;
}

[[gnu::always_inline]] static inline const void *
h_world_item_const(const struct poor_world_node *node, size_t hook)
{
	return node != nullptr ? (const unsigned char *)node - hook : nullptr;
}

[[gnu::always_inline]] static inline struct poor_world_node *const *
h_world_children(const struct poor_world_node *node, size_t children_at)
{
	return (const void *)((const unsigned char *)node + children_at);
}

[[gnu::always_inline]] static inline const struct poor_world_node *
h_world_child(const struct poor_world_node *node, size_t children_at, uint32_t index)
{
	assert(index < node->child_count);
	return h_world_children(node, children_at)[index];
}

[[gnu::always_inline]] static inline void *
h_world_version(const struct h_world *world, size_t version_bytes, uint32_t slot)
{
	return slot != POOR_FAN_NO_SLOT ? (unsigned char *)world->versions + (size_t)slot * version_bytes : nullptr;
}

[[gnu::always_inline]] static inline void *
h_world_current(const struct h_world *world, size_t version_bytes)
{
	return h_world_version(world, version_bytes, poor_fan_published(&world->fan));
}

static inline void
h_world_release(struct h_world *world, struct poor_world_hold *hold)
{
	poor_fan_release(&world->fan, &hold->fan);
}

[[gnu::always_inline]] static inline void *
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

[[gnu::always_inline]] static inline void *
h_world_version_item(const struct h_world *world, const struct h_world_type type, uint32_t slot)
{
	return (unsigned char *)world->versions + (size_t)slot * type.version_bytes;
}

[[gnu::always_inline]] static inline struct poor_world_version *
h_world_version_at(const struct h_world *world, const struct h_world_type type, uint32_t slot)
{
	return (void *)((unsigned char *)h_world_version_item(world, type, slot) + type.version_hook);
}

[[gnu::always_inline]] static inline void *
h_world_next(struct h_world *world, size_t version_bytes)
{
	assert(world->next != POOR_FAN_NO_SLOT);
	return h_world_version(world, version_bytes, world->next);
}

[[gnu::always_inline]] static inline struct h_world_node_header *
h_world_header_of(const struct h_world_type type, struct poor_world_node *node)
{
	return (void *)((unsigned char *)node - type.node_header);
}

[[gnu::always_inline]] static inline struct poor_world_node **
h_world_children_of(const struct h_world_type type, struct poor_world_node *node)
{
	return (void *)((unsigned char *)node + type.node_children);
}

/* The bytes of a node's block that are in use, from the start of the block. A node is copied from there,
 * header included, in whole cache lines: a copy that is not aligned to them measured much slower. */
[[gnu::always_inline]] static inline size_t
h_world_node_size(const struct h_world_type type, size_t children)
{
	return (size_t)type.node_header + type.node_children + children * sizeof(struct poor_world_node *);
}

[[gnu::always_inline]] static inline size_t
h_world_node_capacity(const struct h_world_type type, struct poor_world_node *node)
{
	return (h_world_block_bytes(h_world_header_of(type, node)->order) - h_world_node_size(type, 0))
	       / sizeof(struct poor_world_node *);
}

[[gnu::always_inline]] static inline struct poor_world_node *
h_world_node_alloc(struct h_world *world, const struct h_world_type type, size_t children)
{
	const uint32_t order = h_world_block_order(h_world_node_size(type, children));
	struct h_world_node_header *header = h_world_block_alloc(&world->memory, order);

	if (header == nullptr)
		return nullptr;
	header->order = (uint8_t)order;
	header->state = H_WORLD_NODE_LIVE;
	++world->nodes;
	return (void *)((unsigned char *)header + type.node_header);
}

[[gnu::always_inline]] static inline void
h_world_node_free(struct h_world *world, const struct h_world_type type, struct poor_world_node *node)
{
	struct h_world_node_header *header = h_world_header_of(type, node);

	assert(header->state != H_WORLD_NODE_FREE);
	header->state = H_WORLD_NODE_FREE;
	--world->nodes;
	h_world_block_free(&world->memory, header);
}

[[gnu::always_inline]] static inline struct poor_world_node *
h_world_node_link(const struct h_world_type type, struct poor_world_node *node)
{
	return h_world_header_of(type, node)->link;
}

[[gnu::always_inline]] static inline void
h_world_node_push(const struct h_world_type type, struct poor_world_node **list, struct poor_world_node *node)
{
	h_world_header_of(type, node)->link = *list;
	*list = node;
}

static inline bool
h_world_node_owned(const struct h_world *world, const struct poor_world_node *node)
{
	return node->born == world->epoch;
}

/* A free entry has no node. Free entries are a list through 'slot', and each but the first has the one before it in
 * 'parent': the ones at the end of the table can leave the list, and the table can shrink. */
static inline struct poor_world_node *
h_world_find(const struct h_world *world, uint32_t handle)
{
	return handle < world->index.bound ? world->index.entries[handle].node : nullptr;
}

static inline struct h_world_entry *
h_world_entry_at(const struct h_world *world, uint32_t handle)
{
	return &world->index.entries[handle];
}

static inline struct h_world_entry *
h_world_entry(const struct h_world *world, uint32_t handle)
{
	assert(h_world_find(world, handle) != nullptr);
	return h_world_entry_at(world, handle);
}

static inline uint32_t
h_world_parent(const struct h_world *world, uint32_t handle)
{
	return h_world_entry(world, handle)->parent;
}

static inline size_t
h_world_index_capacity(uint32_t order)
{
	return h_world_block_bytes(order) / sizeof(struct h_world_entry);
}

/* The block of a table for 'entries' handles. One that is a whole number of slabs would take a slab more, for the
 * few bytes of the span header, so the table gets the size below. */
static inline uint32_t
h_world_index_order(const struct h_world *world, size_t entries)
{
	const uint32_t order = h_world_block_order(entries * sizeof(struct h_world_entry));

	return h_world_block_bytes(order) % world->memory.slab_bytes == 0 ? order - 1 : order;
}

[[gnu::cold]] static inline bool
h_world_index_resize(struct h_world *world, uint32_t order)
{
	struct h_world_index *index = &world->index;
	const size_t capacity = h_world_index_capacity(order);
	struct h_world_entry *table = h_world_block_alloc(&world->memory, order);

	assert(capacity <= POOR_WORLD_NO_HANDLE);
	if (table == nullptr)
		return false;
	if (index->entries != nullptr) {
		memcpy(table, index->entries, index->bound * sizeof(struct h_world_entry));
		h_world_block_free(&world->memory, index->entries);
	}
	index->entries = table;
	index->capacity = (uint32_t)capacity;
	return true;
}

[[gnu::cold]] static inline bool
h_world_index_grow(struct h_world *world)
{
	const size_t entries = world->index.capacity != 0 ? 2 * (size_t)world->index.capacity : 1;

	return h_world_index_resize(world, h_world_index_order(world, entries));
}

static inline bool
h_world_index_reserve(struct h_world *world)
{
	const struct h_world_index *index = &world->index;

	return index->free != POOR_WORLD_NO_HANDLE || index->bound < index->capacity || h_world_index_grow(world);
}

static inline uint32_t
h_world_index_alloc(struct h_world *world)
{
	struct h_world_index *index = &world->index;
	const uint32_t handle = index->free;

	if (handle == POOR_WORLD_NO_HANDLE)
		return index->bound++;
	index->free = index->entries[handle].slot;
	return handle;
}

static inline void
h_world_index_free(struct h_world *world, uint32_t handle)
{
	struct h_world_index *index = &world->index;
	struct h_world_entry *entry = &index->entries[handle];

	entry->node = nullptr;
	entry->slot = index->free;
	if (index->free != POOR_WORLD_NO_HANDLE)
		index->entries[index->free].parent = handle;
	index->free = handle;
}

/* A table that is at most a quarter full moves to a smaller block. When there is no memory for that block, the table
 * stays 'oversized', and this is tried again at a later publish or reclaim once a span has become unused ('idled'). */
[[gnu::cold]] static inline void
h_world_index_shrink(struct h_world *world)
{
	struct h_world_index *index = &world->index;
	const size_t wanted = world->memory.wanted;
	const uint32_t order = h_world_index_order(world, 2 * (size_t)index->bound);

	index->oversized = index->bound <= index->capacity / 4 && h_world_index_capacity(order) < index->capacity
			   && !h_world_index_resize(world, order);
	world->memory.idled = false;
	world->memory.wanted = wanted;
}

[[gnu::cold]] static inline void
h_world_index_fit(struct h_world *world)
{
	struct h_world_index *index = &world->index;

	while (index->bound > 0 && index->entries[index->bound - 1].node == nullptr) {
		const uint32_t handle = --index->bound;
		const struct h_world_entry *entry = &index->entries[handle];

		if (index->free == handle)
			index->free = entry->slot;
		else
			index->entries[entry->parent].slot = entry->slot;
		if (entry->slot != POOR_WORLD_NO_HANDLE)
			index->entries[entry->slot].parent = entry->parent;
	}
	if (index->bound == 0) {
		if (index->entries != nullptr)
			h_world_block_free(&world->memory, index->entries);
		index->entries = nullptr;
		index->capacity = 0;
		index->oversized = false;
	} else {
		h_world_index_shrink(world);
	}
}

[[gnu::always_inline]] static inline uint32_t
h_world_child_slot(const struct h_world_type type, struct poor_world_node *parent, struct h_world_entry *entry)
{
	struct poor_world_node *const *children = h_world_children_of(type, parent);
	const uint32_t hint = entry->slot < parent->child_count ? entry->slot : parent->child_count - 1;

	for (uint32_t slot = hint + 1; slot-- > 0;) {
		if (children[slot] == entry->node)
			return entry->slot = slot;
	}
	for (uint32_t slot = hint + 1; slot < parent->child_count; slot++) {
		if (children[slot] == entry->node)
			return entry->slot = slot;
	}
	unreachable();
}

[[gnu::always_inline]] static inline uint32_t
h_world_child_index(struct h_world *world, const struct h_world_type type, uint32_t handle)
{
	struct h_world_entry *entry = h_world_entry(world, handle);

	if (entry->parent == POOR_WORLD_NO_HANDLE)
		return 0;
	return h_world_child_slot(type, h_world_entry_at(world, entry->parent)->node, entry);
}

/* Free slots are a ring through 'older', and the one freed first is taken first:
 * taking the one freed last measured slower while readers are active.
 * 'fresh' is a slot that most likely no reader took. It is taken next: the writer still has it in its cache. */
static inline void
h_world_slot_free(struct h_world *world, uint32_t index, bool fresh)
{
	struct h_world_slot *slots = world->slots;

	if (world->free_last == 0) {
		slots[index].older = index + 1;
		world->free_last = index + 1;
		return;
	}
	slots[index].older = slots[world->free_last - 1].older;
	slots[world->free_last - 1].older = index + 1;
	if (!fresh)
		world->free_last = index + 1;
}

static inline uint32_t
h_world_slot_take(struct h_world *world)
{
	struct h_world_slot *last = &world->slots[world->free_last - 1];
	const uint32_t index = last->older - 1;

	if (index + 1 == world->free_last)
		world->free_last = 0;
	else
		last->older = world->slots[index].older;
	return index;
}

[[gnu::always_inline]] static inline void
h_world_version_free(struct h_world *world, const struct h_world_type type, uint32_t index, bool fresh)
{
	struct h_world_slot *slot = &world->slots[index];
	struct h_world_slot *older = slot->older != 0 ? &world->slots[slot->older - 1] : nullptr;
	const uint64_t epoch = older != nullptr ? h_world_version_at(world, type, slot->older - 1)->epoch : 0;

	for (struct poor_world_node *node = slot->retired, *next; node != nullptr; node = next) {
		next = h_world_node_link(type, node);
		if (older != nullptr && node->born <= epoch)
			h_world_node_push(type, &older->retired, node);
		else
			h_world_node_free(world, type, node);
	}
	slot->retired = nullptr;
	--world->alive;
	h_world_slot_free(world, index, fresh);
}

[[gnu::always_inline]] static inline void
h_world_reclaim_from(struct h_world *world, const struct h_world_type type, bool fresh)
{
	uint32_t *link = &world->slots[poor_fan_published(&world->fan)].older;

	for (; *link != 0; fresh = false) {
		const uint32_t index = *link - 1;

		if (poor_fan_unused(&world->fan, index)) {
			*link = world->slots[index].older;
			h_world_version_free(world, type, index, fresh);
		} else {
			link = &world->slots[index].older;
		}
	}
	if (world->index.oversized && world->memory.idled)
		h_world_index_shrink(world);
}

[[gnu::always_inline]] static inline uint64_t
h_world_oldest_held(const struct h_world *world, const struct h_world_type type)
{
	const uint32_t published = poor_fan_published(&world->fan);
	uint64_t oldest = h_world_version_at(world, type, published)->epoch;

	for (uint32_t older = world->slots[published].older; older != 0; older = world->slots[older - 1].older) {
		if (poor_fan_held(&world->fan, older - 1))
			oldest = h_world_version_at(world, type, older - 1)->epoch;
	}
	return oldest;
}

static inline struct poor_world_stats
h_world_stats(const struct h_world *world)
{
	return (struct poor_world_stats){
		.bytes = world->memory.bytes,
		.idle_bytes = world->memory.idle_bytes,
		.nodes = world->nodes,
		.versions = world->alive,
	};
}

[[gnu::always_inline]] static inline void *
h_world_init(struct h_world *world, const struct h_world_type type, uint32_t slot_count,
	     struct poor_fan_ref *refs, struct h_world_slot *slots, void *versions, size_t slab_bytes)
{
	struct poor_world_version *version;

	poor_fan_init(&world->fan, (struct poor_fan_ref (*)[slot_count])refs);
	world->slots = slots;
	world->versions = versions;
	world->free_last = 0;
	world->next = POOR_FAN_NO_SLOT;
	world->dead = POOR_WORLD_NO_HANDLE;
	world->alive = 1;
	world->stale = 0;
	world->retired = nullptr;
	world->born = nullptr;
	world->nodes = 0;
	world->index.entries = nullptr;
	world->index.capacity = 0;
	world->index.bound = 0;
	world->index.free = POOR_WORLD_NO_HANDLE;
	world->index.oversized = false;
	h_world_memory_init(&world->memory, slab_bytes);
	for (uint32_t index = 0; index < slot_count; index++)
		slots[index] = (struct h_world_slot){ .retired = nullptr, .id_high = 0, .older = 0 };
	for (uint32_t index = 1; index < slot_count; index++)
		h_world_slot_free(world, index, false);
	version = h_world_version_at(world, type, 0);
	version->epoch = 0;
	version->root = nullptr;
	version->node_count = 0;
	version->handle_bound = 0;
	return versions;
}

static inline void
h_world_fini(struct h_world *world)
{
	assert(world->next == POOR_FAN_NO_SLOT);
	h_world_memory_fini(&world->memory);
	world->nodes = 0;
	world->index.entries = nullptr;
	world->index.capacity = 0;
	world->index.bound = 0;
	world->index.free = POOR_WORLD_NO_HANDLE;
	world->index.oversized = false;
}

[[gnu::always_inline]] static inline void *
h_world_begin(struct h_world *world, const struct h_world_type type)
{
	const uint32_t published = poor_fan_published(&world->fan);
	struct poor_world_version *next;
	uint32_t slot;
	void *item;

	assert(world->next == POOR_FAN_NO_SLOT);
	if (world->free_last == 0)
		type.reclaim(world, false);
	assert(world->free_last != 0);
	slot = h_world_slot_take(world);
	item = h_world_version_item(world, type, slot);
	memcpy(item, h_world_version_item(world, type, published), type.version_bytes);
	next = h_world_version_at(world, type, slot);
	++next->epoch;
	world->slots[slot].retired = nullptr;
	world->slots[slot].id_high = world->slots[published].id_high;
	world->slots[slot].older = 0;
	world->next = slot;
	world->version = next;
	world->epoch = next->epoch;
	world->retired = nullptr;
	world->born = nullptr;
	world->dead = POOR_WORLD_NO_HANDLE;
	world->stale = 0;
	return item;
}

[[gnu::always_inline]] static inline void
h_world_born_sweep(struct h_world *world, const struct h_world_type type)
{
	for (struct poor_world_node *node = world->born, *link; node != nullptr; node = link) {
		link = h_world_node_link(type, node);
		if (h_world_header_of(type, node)->state != H_WORLD_NODE_LIVE)
			h_world_node_free(world, type, node);
	}
}

[[gnu::always_inline]] static inline void
h_world_publish(struct h_world *world, const struct h_world_type type)
{
	const uint32_t published = poor_fan_published(&world->fan);

	assert(world->next != POOR_FAN_NO_SLOT);
	if (world->stale != 0)
		h_world_born_sweep(world, type);
	if (world->dead != POOR_WORLD_NO_HANDLE) {
		for (uint32_t handle = world->dead, link; handle != POOR_WORLD_NO_HANDLE; handle = link) {
			link = h_world_entry_at(world, handle)->slot;
			h_world_index_free(world, handle);
		}
		h_world_index_fit(world);
	}
	world->version->handle_bound = world->index.bound;
	world->slots[world->next].older = published + 1;
	world->slots[published].retired = world->retired;
	poor_fan_publish(&world->fan, world->next);
	world->next = POOR_FAN_NO_SLOT;
	++world->alive;
	type.reclaim(world, true);
}

[[gnu::always_inline]] static inline void
h_world_abort(struct h_world *world, const struct h_world_type type)
{
	const uint64_t id_high = world->slots[poor_fan_published(&world->fan)].id_high;

	assert(world->next != POOR_FAN_NO_SLOT);
	for (struct poor_world_node *node = world->retired; node != nullptr; node = h_world_node_link(type, node)) {
		struct poor_world_node **children = h_world_children_of(type, node);

		h_world_entry_at(world, node->handle)->node = node;
		for (uint32_t slot = 0; slot < node->child_count; slot++) {
			struct h_world_entry *child = h_world_entry_at(world, children[slot]->handle);

			child->parent = node->handle;
			child->slot = slot;
		}
	}
	for (struct poor_world_node *node = world->born, *link; node != nullptr; node = link) {
		link = h_world_node_link(type, node);
		if (node->id > id_high && h_world_header_of(type, node)->state != H_WORLD_NODE_REPLACED)
			h_world_index_free(world, node->handle);
		h_world_node_free(world, type, node);
	}
	h_world_slot_free(world, world->next, true);
	world->next = POOR_FAN_NO_SLOT;
	h_world_index_fit(world);
}

[[gnu::always_inline]] static inline struct poor_world_node *
h_world_node_copy(struct h_world *world, const struct h_world_type type, struct poor_world_node *node,
		  size_t capacity)
{
	const size_t bytes = h_world_node_size(type, node->child_count);
	struct poor_world_node *copy;
	struct h_world_node_header *header;
	uint8_t order;

	copy = h_world_node_alloc(world, type, capacity > node->child_count ? capacity : node->child_count);
	if (copy == nullptr)
		return nullptr;
	header = h_world_header_of(type, copy);
	order = header->order;
	memcpy(header, h_world_header_of(type, node),
	       (bytes + H_WORLD_BLOCK_UNIT - 1) / H_WORLD_BLOCK_UNIT * H_WORLD_BLOCK_UNIT);
	assert(header->state == H_WORLD_NODE_LIVE);
	header->order = order;
	copy->born = world->epoch;
	h_world_node_push(type, &world->born, copy);
	return copy;
}

[[gnu::always_inline]] static inline void
h_world_node_retire(struct h_world *world, const struct h_world_type type, struct poor_world_node *node,
		    enum h_world_node_state state)
{
	if (h_world_node_owned(world, node)) {
		h_world_header_of(type, node)->state = state;
		++world->stale;
	} else {
		h_world_node_push(type, &world->retired, node);
	}
}

[[gnu::always_inline]] static inline void
h_world_path_unwind(struct h_world *world, const struct h_world_type type, const struct poor_world_node *until)
{
	while (world->born != until) {
		struct poor_world_node *copy = world->born;
		struct poor_world_node *node = world->retired;

		assert(copy->handle == node->handle);
		world->born = h_world_node_link(type, copy);
		world->retired = h_world_node_link(type, node);
		h_world_entry_at(world, node->handle)->node = node;
		h_world_node_free(world, type, copy);
	}
}

[[gnu::always_inline]] static inline void
h_world_path_disown(struct h_world *world, const struct h_world_type type, const struct poor_world_node *until)
{
	struct poor_world_node *top = world->retired;
	struct h_world_entry *entry;

	if (world->born == until)
		return;
	entry = h_world_entry_at(world, top->handle);
	if (entry->parent == POOR_WORLD_NO_HANDLE) {
		world->version->root = top;
	} else {
		struct poor_world_node *parent = h_world_entry_at(world, entry->parent)->node;

		h_world_children_of(type, parent)[h_world_child_slot(type, parent, entry)] = top;
	}
	type.unwind(world, until);
}

/* When a copy fails, every copy before it replaced a node of an older version,
 * so h_world_path_unwind() finds them in pairs. */
[[gnu::always_inline]] static inline struct poor_world_node *
h_world_path_touch(struct h_world *world, const struct h_world_type type, uint32_t handle, size_t capacity)
{
	struct h_world_entry *entry = h_world_entry_at(world, handle);
	struct poor_world_node *node = entry->node;
	const struct poor_world_node *born = world->born;
	struct poor_world_node *owned;
	struct poor_world_node *copy;

	if (h_world_node_owned(world, node) && (capacity == 0 || h_world_node_capacity(type, node) >= capacity))
		return node;
	owned = copy = h_world_node_copy(world, type, node, capacity);
	if (copy == nullptr)
		return nullptr;
	for (;;) {
		struct h_world_entry *above =
			entry->parent != POOR_WORLD_NO_HANDLE ? h_world_entry_at(world, entry->parent) : nullptr;
		struct poor_world_node *parent = above != nullptr ? above->node : nullptr;
		const uint32_t slot = parent != nullptr ? h_world_child_slot(type, parent, entry) : 0;
		struct poor_world_node *parent_copy;

		h_world_node_retire(world, type, node, H_WORLD_NODE_REPLACED);
		entry->node = copy;
		if (parent == nullptr) {
			world->version->root = copy;
			break;
		}
		if (h_world_node_owned(world, parent)) {
			h_world_children_of(type, parent)[slot] = copy;
			break;
		}
		parent_copy = h_world_node_copy(world, type, parent, 0);
		if (parent_copy == nullptr) {
			type.unwind(world, born);
			return nullptr;
		}
		h_world_children_of(type, parent_copy)[slot] = copy;
		entry = above;
		node = parent;
		copy = parent_copy;
	}
	return owned;
}

static inline struct poor_world_node *
h_world_owned(const struct h_world *world, uint32_t handle)
{
	struct poor_world_node *node = h_world_entry(world, handle)->node;

	assert(world->next != POOR_FAN_NO_SLOT);
	return h_world_node_owned(world, node) ? node : nullptr;
}

[[gnu::always_inline]] static inline void
h_world_children_insert(const struct h_world_type type, struct poor_world_node *parent, uint32_t index,
			struct poor_world_node *child)
{
	struct poor_world_node **children = h_world_children_of(type, parent);

	memmove(&children[index + 1], &children[index], (parent->child_count - index) * sizeof(*children));
	children[index] = child;
	++parent->child_count;
}

[[gnu::always_inline]] static inline void
h_world_children_remove(const struct h_world_type type, struct poor_world_node *parent, uint32_t index)
{
	struct poor_world_node **children = h_world_children_of(type, parent);

	--parent->child_count;
	memmove(&children[index], &children[index + 1], (parent->child_count - index) * sizeof(*children));
}

[[nodiscard, gnu::always_inline]] static inline struct poor_world_node *
h_world_create(struct h_world *world, const struct h_world_type type, uint32_t parent, uint32_t index)
{
	struct poor_world_version *next = world->version;
	const size_t capacity =
		parent != POOR_WORLD_NO_HANDLE ? (size_t)h_world_entry(world, parent)->node->child_count + 1 : 0;
	struct poor_world_node *above = nullptr;
	struct poor_world_node *node;
	struct h_world_entry *entry;

	assert(world->next != POOR_FAN_NO_SLOT);
	assert(parent != POOR_WORLD_NO_HANDLE ? index < capacity : next->root == nullptr && index == 0);
	node = h_world_index_reserve(world) ? h_world_node_alloc(world, type, 0) : nullptr;
	if (node != nullptr && parent != POOR_WORLD_NO_HANDLE) {
		above = type.touch(world, parent, capacity);
		if (above == nullptr) {
			h_world_node_free(world, type, node);
			node = nullptr;
		}
	}
	if (node == nullptr) {
		if (world->index.bound == 0)
			h_world_index_fit(world);
		return nullptr;
	}
	h_world_node_push(type, &world->born, node);
	node->born = world->epoch;
	node->id = ++world->slots[world->next].id_high;
	node->handle = h_world_index_alloc(world);
	node->child_count = 0;
	entry = h_world_entry_at(world, node->handle);
	entry->node = node;
	entry->parent = parent;
	entry->slot = index;
	if (above != nullptr)
		h_world_children_insert(type, above, index, node);
	else
		next->root = node;
	++next->node_count;
	next->handle_bound = world->index.bound;
	return node;
}

[[gnu::always_inline]] static inline void
h_world_subtree_retire(struct h_world *world, const struct h_world_type type, struct poor_world_node *victim)
{
	struct poor_world_version *next = world->version;
	struct poor_world_node *node = victim;

	for (;;) {
		while (node->child_count != 0) {
			node = h_world_children_of(type, node)[0];
			h_world_entry_at(world, node->handle)->slot = 0;
		}
		for (;;) {
			struct h_world_entry *entry = h_world_entry_at(world, node->handle);
			const uint32_t slot = entry->slot;
			struct poor_world_node *parent;

			entry->node = nullptr;
			entry->slot = world->dead;
			world->dead = node->handle;
			--next->node_count;
			h_world_node_retire(world, type, node, H_WORLD_NODE_DESTROYED);
			if (node == victim)
				return;
			parent = h_world_entry_at(world, entry->parent)->node;
			if (slot + 1 < parent->child_count) {
				node = h_world_children_of(type, parent)[slot + 1];
				h_world_entry_at(world, node->handle)->slot = slot + 1;
				break;
			}
			node = parent;
		}
	}
}

[[nodiscard, gnu::always_inline]] static inline bool
h_world_destroy(struct h_world *world, const struct h_world_type type, uint32_t handle)
{
	struct h_world_entry *entry = h_world_entry(world, handle);
	struct poor_world_node *parent;

	assert(world->next != POOR_FAN_NO_SLOT);
	if (entry->parent == POOR_WORLD_NO_HANDLE) {
		world->version->root = nullptr;
	} else {
		parent = type.touch(world, entry->parent, 0);
		if (parent == nullptr)
			return false;
		h_world_children_remove(type, parent, h_world_child_slot(type, parent, entry));
	}
	h_world_subtree_retire(world, type, entry->node);
	return true;
}

static inline bool
h_world_handle_inside(const struct h_world *world, uint32_t handle, uint32_t ancestor)
{
	for (; handle != POOR_WORLD_NO_HANDLE; handle = h_world_entry_at(world, handle)->parent) {
		if (handle == ancestor)
			return true;
	}
	return false;
}

[[nodiscard, gnu::always_inline]] static inline bool
h_world_move(struct h_world *world, const struct h_world_type type, uint32_t handle, uint32_t parent,
	     uint32_t index)
{
	struct h_world_entry *entry = h_world_entry(world, handle);
	const uint32_t origin = entry->parent;
	const size_t count = h_world_entry(world, parent)->node->child_count;
	const struct poor_world_node *born;
	struct poor_world_node *from;
	struct poor_world_node *to;

	assert(world->next != POOR_FAN_NO_SLOT);
	assert(origin != POOR_WORLD_NO_HANDLE);
	if (origin == parent) {
		const uint32_t slot = h_world_child_slot(type, h_world_entry_at(world, parent)->node, entry);

		assert(index < count);
		if (slot == index)
			return true;
		to = type.touch(world, parent, 0);
		if (to == nullptr)
			return false;
		h_world_children_remove(type, to, slot);
		h_world_children_insert(type, to, index, entry->node);
		entry->slot = index;
		return true;
	}
	assert(index <= count && !h_world_handle_inside(world, parent, handle));
	born = world->born;
	if (type.touch(world, origin, 0) == nullptr)
		return false;
	to = type.touch(world, parent, count + 1);
	if (to == nullptr) {
		h_world_path_disown(world, type, born);
		return false;
	}
	from = h_world_entry_at(world, origin)->node;
	h_world_children_remove(type, from, h_world_child_slot(type, from, entry));
	h_world_children_insert(type, to, index, entry->node);
	entry->parent = parent;
	entry->slot = index;
	return true;
}

static inline void
h_world_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count, uint32_t *slots,
			  uint32_t match_count, struct poor_world_diff_match *matches)
{
	for (uint32_t slot = 0; slot < slot_count; slot++)
		slots[slot] = 0;
	scratch->slots = slots;
	scratch->matches = matches;
	scratch->slot_count = slot_count;
	scratch->match_count = match_count;
	scratch->top = 0;
}

[[gnu::always_inline]] static inline void
h_world_diff_begin(struct poor_world_diff *diff, struct poor_world_diff_scratch *scratch,
		   const struct poor_world_node *old, const struct poor_world_node *next, size_t children_at, bool all)
{
	diff->scratch = scratch;
	diff->old = h_world_children(old, children_at);
	diff->new = h_world_children(next, children_at);
	diff->old_count = old->child_count;
	diff->new_count = next->child_count;
	diff->head = 0;
	diff->tail = 0;
	diff->base = scratch->top;
	diff->phase = H_WORLD_DIFF_HEAD;
	diff->all = all;
}

static inline void
h_world_diff_stop(struct poor_world_diff *diff)
{
	diff->scratch->top = diff->base;
	diff->phase = H_WORLD_DIFF_DONE;
}

/* Marks the matches of a longest run that kept its order as stable. */
static inline void
h_world_diff_mark_stable(uint32_t count, struct poor_world_diff_match *matches)
{
	uint32_t length = 0;

	for (uint32_t k = 0; k < count; k++) {
		uint32_t low = 0, high = length;

		while (low < high) {
			const uint32_t middle = (low + high) / 2;

			if (matches[matches[middle].tail].old_index < matches[k].old_index)
				low = middle + 1;
			else
				high = middle;
		}
		matches[low].tail = k;
		matches[k].prev = low > 0 ? matches[low - 1].tail : UINT32_MAX;
		matches[k].stable = false;
		if (low == length)
			++length;
	}
	if (length > 0) {
		for (uint32_t k = matches[length - 1].tail; k != UINT32_MAX; k = matches[k].prev)
			matches[k].stable = true;
	}
}

static inline bool
h_world_diff_same(const struct poor_world_node *old, const struct poor_world_node *next)
{
	return old == next || old->id == next->id;
}

static inline bool
h_world_diff_match_rotation(uint32_t count, struct poor_world_node *const *old, struct poor_world_node *const *new,
			    struct poor_world_diff_match *matches)
{
	bool up = h_world_diff_same(old[0], new[count - 1]), down = h_world_diff_same(old[count - 1], new[0]);

	for (uint32_t i = 1; i < count && (up || down); i++) {
		up = up && h_world_diff_same(old[i], new[i - 1]);
		down = down && h_world_diff_same(old[i - 1], new[i]);
	}
	if (!up && !down)
		return false;
	for (uint32_t j = 0; j < count; j++) {
		if (up)
			matches[j].old_index = j + 1 < count ? j + 1 : 0;
		else
			matches[j].old_index = j > 0 ? j - 1 : count - 1;
		matches[j].new_index = j;
		matches[j].stable = j != (up ? count - 1 : 0);
	}
	return true;
}

static inline void
h_world_diff_match_middle(struct poor_world_diff *diff)
{
	struct poor_world_diff_scratch *scratch = diff->scratch;
	struct poor_world_node *const *old = &diff->old[diff->head];
	struct poor_world_node *const *new = &diff->new[diff->head];
	const uint32_t old_count = diff->old_count - diff->head - diff->tail;
	const uint32_t new_count = diff->new_count - diff->head - diff->tail;
	struct poor_world_diff_match *matches = &scratch->matches[diff->base];
	uint32_t *slots = scratch->slots;
	uint32_t common = 0, removed = 0;

	assert(scratch->match_count - diff->base >= (size_t)old_count + new_count);
	if (old_count == new_count && old_count >= 2 && h_world_diff_match_rotation(old_count, old, new, matches)) {
		diff->common = old_count;
		diff->removed = 0;
		diff->at = new_count;
		scratch->top = diff->base + new_count;
		return;
	}
	for (uint32_t i = 0; i < old_count; i++) {
		assert(old[i]->handle < scratch->slot_count);
		slots[old[i]->handle] = i + 1;
	}
	for (uint32_t j = 0; j < new_count; j++) {
		const uint32_t slot = new[j]->handle < scratch->slot_count ? slots[new[j]->handle] : 0;

		if (slot != 0 && old[slot - 1]->id == new[j]->id) {
			matches[common].old_index = slot - 1;
			matches[common].new_index = j;
			++common;
			slots[new[j]->handle] = 0;
		} else {
			matches[new_count - 1 - (j - common)].new_index = j;
		}
	}
	for (uint32_t i = 0; i < old_count; i++) {
		if (slots[old[i]->handle] != 0) {
			slots[old[i]->handle] = 0;
			matches[new_count + removed++].old_index = i;
		}
	}
	h_world_diff_mark_stable(common, matches);
	diff->common = common;
	diff->removed = removed;
	diff->at = new_count;
	scratch->top = diff->base + new_count + removed;
}

static inline bool
h_world_diff_yield(struct poor_world_diff *diff, const struct poor_world_node *old, const struct poor_world_node *next,
		   bool moved)
{
	diff->old_node = old;
	diff->new_node = next;
	diff->moved = moved;
	return true;
}

static inline uint32_t
h_world_diff_same_head(struct poor_world_node *const *old, struct poor_world_node *const *new, uint32_t count)
{
	uint32_t run = 0;

	while (run < count && old[run] == new[run])
		++run;
	return run;
}

static inline uint32_t
h_world_diff_same_tail(struct poor_world_node *const *old_end, struct poor_world_node *const *new_end, uint32_t count)
{
	uint32_t run = 0;

	while (run < count && old_end[-(ptrdiff_t)run - 1] == new_end[-(ptrdiff_t)run - 1])
		++run;
	return run;
}

/* A long run is compared four at a time, which measured twice as fast. A short one is not: the second loop is one
 * more branch to mispredict, and that measured slower. */
static inline uint32_t
h_world_diff_same_head_wide(struct poor_world_node *const *old, struct poor_world_node *const *new, uint32_t count)
{
	uint32_t run = 0;

	while (count - run >= 4 && !((old[run] != new[run]) | (old[run + 1] != new[run + 1])
				     | (old[run + 2] != new[run + 2]) | (old[run + 3] != new[run + 3])))
		run += 4;
	return run + h_world_diff_same_head(&old[run], &new[run], count - run);
}

static inline uint32_t
h_world_diff_same_tail_wide(struct poor_world_node *const *old_end, struct poor_world_node *const *new_end,
			    uint32_t count)
{
	uint32_t run = 0;

	while (count - run >= 4 && !((old_end[-(ptrdiff_t)run - 1] != new_end[-(ptrdiff_t)run - 1])
				     | (old_end[-(ptrdiff_t)run - 2] != new_end[-(ptrdiff_t)run - 2])
				     | (old_end[-(ptrdiff_t)run - 3] != new_end[-(ptrdiff_t)run - 3])
				     | (old_end[-(ptrdiff_t)run - 4] != new_end[-(ptrdiff_t)run - 4])))
		run += 4;
	return run + h_world_diff_same_tail(&old_end[-(ptrdiff_t)run], &new_end[-(ptrdiff_t)run], count - run);
}

static inline bool
h_world_diff_scan_head(struct poor_world_diff *diff, uint32_t shared, bool wide)
{
	struct poor_world_node *const *old = &diff->old[diff->head], *const *new = &diff->new[diff->head];
	uint32_t head = diff->head;

	if (!diff->all)
		head += wide ? h_world_diff_same_head_wide(old, new, shared - head)
			     : h_world_diff_same_head(old, new, shared - head);
	diff->head = head;
	if (head == shared || !h_world_diff_same(diff->old[head], diff->new[head]))
		return false;
	diff->head = head + 1;
	return h_world_diff_yield(diff, diff->old[head], diff->new[head], false);
}

static inline bool
h_world_diff_scan_tail(struct poor_world_diff *diff, uint32_t shared, bool wide)
{
	struct poor_world_node *const *old_end = &diff->old[diff->old_count - diff->tail];
	struct poor_world_node *const *new_end = &diff->new[diff->new_count - diff->tail];
	const uint32_t rest = shared - diff->head;
	uint32_t tail = diff->tail;
	const struct poor_world_node *old, *next;

	if (!diff->all && tail < rest)
		tail += wide ? h_world_diff_same_tail_wide(old_end, new_end, rest - tail)
			     : h_world_diff_same_tail(old_end, new_end, rest - tail);
	diff->tail = tail;
	if (tail == rest)
		return false;
	old = diff->old[diff->old_count - 1 - tail];
	next = diff->new[diff->new_count - 1 - tail];
	if (!h_world_diff_same(old, next))
		return false;
	diff->tail = tail + 1;
	return h_world_diff_yield(diff, old, next, false);
}

static inline bool
h_world_diff_scan(struct poor_world_diff *diff, bool wide)
{
	const uint32_t shared = diff->old_count < diff->new_count ? diff->old_count : diff->new_count;

	if (diff->phase == H_WORLD_DIFF_HEAD) {
		if (h_world_diff_scan_head(diff, shared, wide))
			return true;
		diff->phase = H_WORLD_DIFF_TAIL;
	}
	if (h_world_diff_scan_tail(diff, shared, wide))
		return true;
	if (diff->head + diff->tail == diff->old_count && diff->head + diff->tail == diff->new_count)
		diff->phase = H_WORLD_DIFF_DONE;
	else
		diff->phase = H_WORLD_DIFF_MIDDLE;
	return false;
}

static inline bool
h_world_diff_scan_common(struct poor_world_diff *diff, const struct poor_world_diff_match *matches)
{
	struct poor_world_node *const *olds = &diff->old[diff->head], *const *news = &diff->new[diff->head];
	const uint32_t common = diff->common;
	const bool all = diff->all;

	for (uint32_t at = diff->at; at < common; at++) {
		const struct poor_world_diff_match *match = &matches[at];
		const struct poor_world_node *old = olds[match->old_index], *next = news[match->new_index];

		if (!match->stable || all || old != next) {
			diff->at = at + 1;
			return h_world_diff_yield(diff, old, next, !match->stable);
		}
	}
	return false;
}

static inline bool
h_world_diff_step(struct poor_world_diff *diff)
{
	const struct poor_world_diff_match *matches = &diff->scratch->matches[diff->base];
	struct poor_world_node *const *olds, *const *news;
	uint32_t new_count;

	if (diff->phase <= H_WORLD_DIFF_TAIL && h_world_diff_scan(diff, true))
		return true;
	olds = &diff->old[diff->head];
	news = &diff->new[diff->head];
	new_count = diff->new_count - diff->head - diff->tail;
	switch (diff->phase) {
	case H_WORLD_DIFF_MIDDLE:
		h_world_diff_match_middle(diff);
		diff->phase = H_WORLD_DIFF_ADDED;
		[[fallthrough]];
	case H_WORLD_DIFF_ADDED:
		if (diff->at > diff->common)
			return h_world_diff_yield(diff, nullptr, news[matches[--diff->at].new_index], false);
		diff->at = 0;
		diff->phase = H_WORLD_DIFF_REMOVED;
		[[fallthrough]];
	case H_WORLD_DIFF_REMOVED:
		if (diff->at < diff->removed)
			return h_world_diff_yield(diff, olds[matches[new_count + diff->at++].old_index], nullptr,
						  false);
		diff->at = 0;
		diff->phase = H_WORLD_DIFF_COMMON;
		[[fallthrough]];
	case H_WORLD_DIFF_COMMON:
		if (h_world_diff_scan_common(diff, matches))
			return true;
		h_world_diff_stop(diff);
		return false;
	default:
		return false;
	}
}

/* 'step' is the tree type's copy of h_world_diff_step(). Only the unchanged head and tail of a short list of
 * children are skipped here. */
[[gnu::always_inline]] static inline bool
h_world_diff_next(struct poor_world_diff *diff, bool (*step)(struct poor_world_diff *diff))
{
	const uint32_t shared = diff->old_count < diff->new_count ? diff->old_count : diff->new_count;

	if (diff->phase > H_WORLD_DIFF_TAIL || shared - diff->head >= H_WORLD_DIFF_WIDE)
		return step(diff);
	if (h_world_diff_scan(diff, false))
		return true;
	return diff->phase == H_WORLD_DIFF_MIDDLE && step(diff);
}

#endif
