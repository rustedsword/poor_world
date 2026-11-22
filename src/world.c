/* SPDX-License-Identifier: MIT */
#include "internal.h"

static void *
version_item(const struct h_world *world, uint32_t slot)
{
	return (unsigned char *)world->versions + (size_t)slot * world->layout.version_bytes;
}

static struct poor_world_version *
version_at(const struct h_world *world, uint32_t slot)
{
	return (void *)((unsigned char *)version_item(world, slot) + world->layout.version_hook);
}

static struct poor_world_version *
version_next(const struct h_world *world)
{
	assert(world->next != POOR_FAN_NO_SLOT);
	return world->version;
}

static struct node_header *
node_header(const struct h_world_layout *layout, struct poor_world_node *node)
{
	return (void *)((unsigned char *)node - layout->node_header);
}

static struct poor_world_node **
node_children(const struct h_world_layout *layout, struct poor_world_node *node)
{
	return (void *)((unsigned char *)node + layout->node_children);
}

/* The bytes of a node's block that are in use, from the start of the block. A node is copied from there,
 * header included, in whole cache lines: a copy that is not aligned to them measured much slower. */
static size_t
node_bytes(const struct h_world_layout *layout, size_t children)
{
	return layout->block_bytes + children * sizeof(struct poor_world_node *);
}

static uint32_t
node_order(const struct h_world_layout *layout, size_t children)
{
	return block_order(node_bytes(layout, children));
}

static size_t
node_capacity(const struct h_world_layout *layout, struct poor_world_node *node)
{
	return (block_bytes(node_header(layout, node)->order) - layout->block_bytes) / sizeof(struct poor_world_node *);
}

static struct poor_world_node *
node_alloc(struct h_world *world, const struct h_world_layout *layout, size_t children)
{
	const uint32_t order = node_order(layout, children);
	struct node_header *header = h_world_block_alloc(&world->memory, order);

	if (header == nullptr)
		return nullptr;
	header->order = (uint8_t)order;
	header->state = NODE_LIVE;
	++world->nodes;
	return (void *)((unsigned char *)header + layout->node_header);
}

static void
node_free(struct h_world *world, const struct h_world_layout *layout, struct poor_world_node *node)
{
	struct node_header *header = node_header(layout, node);

	assert(header->state != NODE_FREE);
	header->state = NODE_FREE;
	--world->nodes;
	h_world_block_free(&world->memory, header);
}

static struct poor_world_node *
node_link(const struct h_world_layout *layout, struct poor_world_node *node)
{
	return node_header(layout, node)->link;
}

static void
node_push(const struct h_world_layout *layout, struct poor_world_node **list, struct poor_world_node *node)
{
	node_header(layout, node)->link = *list;
	*list = node;
}

static struct h_world_entry *
entry_of(const struct h_world *world, uint32_t handle)
{
	assert(handle / PAGE_ENTRIES < world->index.page_count);
	return &world->index.pages[handle / PAGE_ENTRIES].entries[handle % PAGE_ENTRIES];
}

static struct h_world_entry *
entry_alive(const struct h_world *world, uint32_t handle)
{
	return h_world_entry(world, handle);
}

static void
pages_push(struct h_world_index *index, uint32_t *first, uint32_t page)
{
	index->pages[page].prev = H_WORLD_NO_PAGE;
	index->pages[page].next = *first;
	if (*first != H_WORLD_NO_PAGE)
		index->pages[*first].prev = page;
	*first = page;
}

static void
pages_remove(struct h_world_index *index, uint32_t *first, uint32_t page)
{
	const struct h_world_page *p = &index->pages[page];

	if (p->prev != H_WORLD_NO_PAGE)
		index->pages[p->prev].next = p->next;
	else
		*first = p->next;
	if (p->next != H_WORLD_NO_PAGE)
		index->pages[p->next].prev = p->prev;
}

static bool
pages_resize(struct h_world *world, uint32_t order)
{
	struct h_world_index *index = &world->index;
	struct h_world_page *pages = h_world_block_alloc(&world->memory, order);

	if (pages == nullptr)
		return false;
	if (index->pages != nullptr) {
		copy_array(array_ptr(pages, index->page_count), array_ptr(index->pages, index->page_count));
		h_world_block_free(&world->memory, index->pages);
	}
	index->pages = pages;
	index->capacity = (uint32_t)(block_bytes(order) / sizeof(struct h_world_page));
	return true;
}

static void
pages_drop(struct h_world *world)
{
	h_world_block_free(&world->memory, world->index.pages);
	world->index.pages = nullptr;
	world->index.capacity = 0;
}

static void
page_release(struct h_world *world, uint32_t page)
{
	struct h_world_index *index = &world->index;

	pages_remove(index, &index->partial, page);
	h_world_block_free(&world->memory, index->pages[page].entries);
	index->pages[page].entries = nullptr;
	pages_push(index, &index->vacant, page);
	while (index->page_count > 0 && index->pages[index->page_count - 1].entries == nullptr)
		pages_remove(index, &index->vacant, --index->page_count);
	if (index->page_count == 0)
		pages_drop(world);
}

static void
pages_fit(struct h_world *world)
{
	const struct h_world_index *index = &world->index;
	const size_t wanted = world->memory.wanted;
	uint32_t order;

	if (index->page_count == 0 || index->page_count > index->capacity / 4)
		return;
	order = block_order(2 * (size_t)index->page_count * sizeof(struct h_world_page));
	if (block_bytes(order) / sizeof(struct h_world_page) < index->capacity) {
		pages_resize(world, order);
		world->memory.wanted = wanted;
	}
}

static bool
index_reserve(struct h_world *world)
{
	struct h_world_index *index = &world->index;
	struct h_world_entry *entries;
	uint32_t page = index->vacant;

	if (index->partial != H_WORLD_NO_PAGE)
		return true;
	if (page == H_WORLD_NO_PAGE && index->page_count == index->capacity
	    && !pages_resize(world, block_order((2 * (size_t)index->page_count + 2) * sizeof(struct h_world_page))))
		return false;
	entries = h_world_block_alloc(&world->memory, PAGE_ORDER);
	if (entries == nullptr)
		return false;
	if (page != H_WORLD_NO_PAGE)
		pages_remove(index, &index->vacant, page);
	else
		page = index->page_count++;
	foreach_array_index(array_ptr(entries, PAGE_ENTRIES), slot) {
		entries[slot].node = nullptr;
		entries[slot].slot = slot + 1 < PAGE_ENTRIES ? (uint32_t)slot + 1 : POOR_WORLD_NO_HANDLE;
	}
	index->pages[page].entries = entries;
	index->pages[page].live = 0;
	index->pages[page].free = 0;
	pages_push(index, &index->partial, page);
	return true;
}

static void
index_unreserve(struct h_world *world)
{
	struct h_world_index *index = &world->index;

	if (index->partial != H_WORLD_NO_PAGE && index->pages[index->partial].live == 0)
		page_release(world, index->partial);
	if (index->pages != nullptr && index->page_count == 0)
		pages_drop(world);
}

static uint32_t
index_alloc(struct h_world *world)
{
	struct h_world_index *index = &world->index;
	const uint32_t page = index->partial;
	struct h_world_page *p = &index->pages[page];
	const uint32_t slot = p->free;

	p->free = p->entries[slot].slot;
	if (++p->live == PAGE_ENTRIES)
		pages_remove(index, &index->partial, page);
	return page * PAGE_ENTRIES + slot;
}

static void
index_free(struct h_world *world, uint32_t handle)
{
	struct h_world_index *index = &world->index;
	const uint32_t page = handle / PAGE_ENTRIES;
	struct h_world_page *p = &index->pages[page];

	p->entries[handle % PAGE_ENTRIES].node = nullptr;
	p->entries[handle % PAGE_ENTRIES].slot = p->free;
	p->free = handle % PAGE_ENTRIES;
	if (p->live == PAGE_ENTRIES)
		pages_push(index, &index->partial, page);
	if (--p->live == 0)
		page_release(world, page);
}

static inline uint32_t
child_slot(const struct h_world_layout *layout, struct poor_world_node *parent, struct h_world_entry *entry)
{
	struct poor_world_node *const *children = node_children(layout, parent);
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

EXPORT uint32_t
h_world_index_of(struct h_world *world, uint32_t handle)
{
	const struct h_world_layout *layout = &world->layout;
	struct h_world_entry *entry = entry_alive(world, handle);

	return entry->parent != POOR_WORLD_NO_HANDLE ? child_slot(layout, entry_of(world, entry->parent)->node, entry) : 0;
}

/* Free slots are a ring through 'older', and the one freed first is taken first:
 * taking the one freed last measured slower while readers are active.
 * 'fresh' is a slot that most likely no reader took. It is taken next: the writer still has it in its cache. */
static void
slot_free(struct h_world *world, uint32_t index, bool fresh)
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

static uint32_t
slot_take(struct h_world *world)
{
	struct h_world_slot *last = &world->slots[world->free_last - 1];
	const uint32_t index = last->older - 1;

	if (index + 1 == world->free_last)
		world->free_last = 0;
	else
		last->older = world->slots[index].older;
	return index;
}

static void
version_free(struct h_world *world, const struct h_world_layout *layout, uint32_t index, bool fresh)
{
	struct h_world_slot *slot = &world->slots[index];
	struct h_world_slot *older = slot->older != 0 ? &world->slots[slot->older - 1] : nullptr;
	const uint64_t epoch = older != nullptr ? version_at(world, slot->older - 1)->epoch : 0;

	for (struct poor_world_node *node = slot->retired, *next; node != nullptr; node = next) {
		next = node_link(layout, node);
		if (older != nullptr && node->born <= epoch)
			node_push(layout, &older->retired, node);
		else
			node_free(world, layout, node);
	}
	slot->retired = nullptr;
	--world->alive;
	slot_free(world, index, fresh);
}

static void
reclaim(struct h_world *world, bool fresh)
{
	const struct h_world_layout *layout = &world->layout;
	uint32_t *link = &world->slots[poor_fan_published(&world->fan)].older;

	for (; *link != 0; fresh = false) {
		const uint32_t index = *link - 1;

		if (poor_fan_unused(&world->fan, index)) {
			*link = world->slots[index].older;
			version_free(world, layout, index, fresh);
		} else {
			link = &world->slots[index].older;
		}
	}
}

EXPORT void
h_world_reclaim(struct h_world *world)
{
	reclaim(world, false);
}

EXPORT uint64_t
h_world_oldest_held(const struct h_world *world)
{
	const uint32_t published = poor_fan_published(&world->fan);
	uint64_t oldest = version_at(world, published)->epoch;

	for (uint32_t older = world->slots[published].older; older != 0; older = world->slots[older - 1].older) {
		if (poor_fan_held(&world->fan, older - 1))
			oldest = version_at(world, older - 1)->epoch;
	}
	return oldest;
}

EXPORT struct poor_world_stats
h_world_stats(const struct h_world *world)
{
	return (struct poor_world_stats){
		.bytes = world->memory.bytes,
		.idle_bytes = world->memory.idle_bytes,
		.nodes = world->nodes,
		.versions = world->alive,
	};
}

EXPORT void *
h_world_init(struct h_world *world, struct poor_fan_ref *refs, struct h_world_slot *slots, void *versions,
	     struct h_world_layout layout, size_t slab_bytes)
{
	struct poor_world_version *version;

	poor_fan_init(&world->fan, array_ptr(refs, layout.slot_count));
	world->slots = slots;
	world->versions = versions;
	world->layout = layout;
	world->free_last = 0;
	world->next = POOR_FAN_NO_SLOT;
	world->dead = POOR_WORLD_NO_HANDLE;
	world->alive = 1;
	world->stale = 0;
	world->retired = nullptr;
	world->born = nullptr;
	world->nodes = 0;
	world->index.pages = nullptr;
	world->index.page_count = 0;
	world->index.partial = H_WORLD_NO_PAGE;
	world->index.vacant = H_WORLD_NO_PAGE;
	world->index.capacity = 0;
	h_world_memory_init(&world->memory, slab_bytes);
	fill_array(array_ptr(slots, layout.slot_count),
		   (struct h_world_slot){ .retired = nullptr, .id_high = 0, .older = 0 });
	for (uint32_t index = 1; index < layout.slot_count; index++)
		slot_free(world, index, false);
	version = version_at(world, 0);
	version->epoch = 0;
	version->root = nullptr;
	version->node_count = 0;
	version->handle_bound = 0;
	return versions;
}

EXPORT void
h_world_fini(struct h_world *world)
{
	assert(world->next == POOR_FAN_NO_SLOT);
	h_world_memory_fini(&world->memory);
	world->nodes = 0;
	world->index.pages = nullptr;
	world->index.page_count = 0;
	world->index.capacity = 0;
	world->index.partial = H_WORLD_NO_PAGE;
	world->index.vacant = H_WORLD_NO_PAGE;
}

EXPORT void *
h_world_begin(struct h_world *world)
{
	const struct h_world_layout *layout = &world->layout;
	const uint32_t published = poor_fan_published(&world->fan);
	struct poor_world_version *current = version_at(world, published);
	struct poor_world_version *next;
	uint32_t slot;

	assert(world->next == POOR_FAN_NO_SLOT);
	if (world->free_last == 0)
		h_world_reclaim(world);
	assert(world->free_last != 0);
	slot = slot_take(world);
	memcpy(version_item(world, slot), version_item(world, published), layout->version_bytes);
	next = version_at(world, slot);
	next->epoch = current->epoch + 1;
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
	return version_item(world, slot);
}

static void
born_sweep(struct h_world *world, const struct h_world_layout *layout)
{
	for (struct poor_world_node *node = world->born, *link; node != nullptr; node = link) {
		link = node_link(layout, node);
		if (node_header(layout, node)->state != NODE_LIVE)
			node_free(world, layout, node);
	}
}

EXPORT void
h_world_publish(struct h_world *world)
{
	const struct h_world_layout *layout = &world->layout;
	const uint32_t published = poor_fan_published(&world->fan);
	struct poor_world_version *next = version_next(world);

	if (world->stale != 0)
		born_sweep(world, layout);
	for (uint32_t handle = world->dead, link; handle != POOR_WORLD_NO_HANDLE; handle = link) {
		link = entry_of(world, handle)->slot;
		index_free(world, handle);
	}
	next->handle_bound = world->index.page_count * PAGE_ENTRIES;
	world->slots[world->next].older = published + 1;
	world->slots[published].retired = world->retired;
	poor_fan_publish(&world->fan, world->next);
	world->next = POOR_FAN_NO_SLOT;
	++world->alive;
	reclaim(world, true);
	pages_fit(world);
}

EXPORT void
h_world_abort(struct h_world *world)
{
	const struct h_world_layout *layout = &world->layout;
	const uint64_t id_high = world->slots[poor_fan_published(&world->fan)].id_high;

	for (struct poor_world_node *node = world->retired; node != nullptr; node = node_link(layout, node)) {
		struct poor_world_node **children = node_children(layout, node);

		entry_of(world, node->handle)->node = node;
		foreach_array_index(array_ptr(children, node->child_count), slot) {
			struct h_world_entry *child = entry_of(world, children[slot]->handle);

			child->parent = node->handle;
			child->slot = (uint32_t)slot;
		}
	}
	for (struct poor_world_node *node = world->born, *link; node != nullptr; node = link) {
		link = node_link(layout, node);
		if (node->id > id_high && node_header(layout, node)->state != NODE_REPLACED)
			index_free(world, node->handle);
		node_free(world, layout, node);
	}
	slot_free(world, world->next, true);
	world->next = POOR_FAN_NO_SLOT;
	pages_fit(world);
}

static bool
node_owned(const struct h_world *world, const struct poor_world_node *node)
{
	assert(world->next != POOR_FAN_NO_SLOT);
	return node->born == world->epoch;
}

static inline struct poor_world_node *
node_copy(struct h_world *world, const struct h_world_layout *layout, struct poor_world_node *node, size_t capacity)
{
	const size_t bytes = node_bytes(layout, node->child_count);
	struct poor_world_node *copy;
	struct node_header *header;
	uint8_t order;

	copy = node_alloc(world, layout, capacity > node->child_count ? capacity : node->child_count);
	if (copy == nullptr)
		return nullptr;
	header = node_header(layout, copy);
	order = header->order;
	memcpy(header, node_header(layout, node), (bytes + BLOCK_UNIT - 1) / BLOCK_UNIT * BLOCK_UNIT);
	assert(header->state == NODE_LIVE);
	header->order = order;
	copy->born = world->epoch;
	node_push(layout, &world->born, copy);
	return copy;
}

static void
node_retire(struct h_world *world, const struct h_world_layout *layout, struct poor_world_node *node,
	    enum node_state state)
{
	if (node_owned(world, node)) {
		node_header(layout, node)->state = state;
		++world->stale;
	} else {
		node_push(layout, &world->retired, node);
	}
}

static void
path_unwind(struct h_world *world, const struct h_world_layout *layout, const struct poor_world_node *until)
{
	while (world->born != until) {
		struct poor_world_node *copy = world->born;
		struct poor_world_node *node = world->retired;

		assert(copy->handle == node->handle);
		world->born = node_link(layout, copy);
		world->retired = node_link(layout, node);
		entry_of(world, node->handle)->node = node;
		node_free(world, layout, copy);
	}
}

static void
path_disown(struct h_world *world, const struct h_world_layout *layout, const struct poor_world_node *until)
{
	struct poor_world_node *top = world->retired;
	struct h_world_entry *entry;

	if (world->born == until)
		return;
	entry = entry_of(world, top->handle);
	if (entry->parent == POOR_WORLD_NO_HANDLE) {
		version_next(world)->root = top;
	} else {
		struct poor_world_node *parent = entry_of(world, entry->parent)->node;

		node_children(layout, parent)[child_slot(layout, parent, entry)] = top;
	}
	path_unwind(world, layout, until);
}

/* When a copy fails, every copy before it replaced a node of an older version,
 * so path_unwind() finds them in pairs. */
static struct poor_world_node *
path_touch(struct h_world *world, const struct h_world_layout *layout, uint32_t handle, size_t capacity)
{
	struct h_world_entry *entry = entry_of(world, handle);
	struct poor_world_node *node = entry->node;
	const struct poor_world_node *born = world->born;
	struct poor_world_node *owned;
	struct poor_world_node *copy;

	if (node_owned(world, node) && (capacity == 0 || node_capacity(layout, node) >= capacity))
		return node;
	owned = copy = node_copy(world, layout, node, capacity);
	if (copy == nullptr)
		return nullptr;
	for (;;) {
		struct h_world_entry *above =
			entry->parent != POOR_WORLD_NO_HANDLE ? entry_of(world, entry->parent) : nullptr;
		struct poor_world_node *parent = above != nullptr ? above->node : nullptr;
		const uint32_t slot = parent != nullptr ? child_slot(layout, parent, entry) : 0;
		struct poor_world_node *parent_copy;

		node_retire(world, layout, node, NODE_REPLACED);
		entry->node = copy;
		if (parent == nullptr) {
			version_next(world)->root = copy;
			break;
		}
		if (node_owned(world, parent)) {
			node_children(layout, parent)[slot] = copy;
			break;
		}
		parent_copy = node_copy(world, layout, parent, 0);
		if (parent_copy == nullptr) {
			path_unwind(world, layout, born);
			return nullptr;
		}
		node_children(layout, parent_copy)[slot] = copy;
		entry = above;
		node = parent;
		copy = parent_copy;
	}
	return owned;
}

static void
children_insert(const struct h_world_layout *layout, struct poor_world_node *parent, uint32_t index,
		struct poor_world_node *child)
{
	++parent->child_count;
	array_insert(array_ptr(node_children(layout, parent), parent->child_count), index, child);
}

static void
children_remove(const struct h_world_layout *layout, struct poor_world_node *parent, uint32_t index)
{
	struct poor_world_node **children = node_children(layout, parent);

	array_remove_ref(array_ptr(children, parent->child_count), &children[index]);
	--parent->child_count;
}

EXPORT struct poor_world_node *
h_world_create(struct h_world *world, uint32_t parent, uint32_t index)
{
	const struct h_world_layout *layout = &world->layout;
	struct poor_world_version *next = version_next(world);
	const size_t capacity =
		parent != POOR_WORLD_NO_HANDLE ? (size_t)entry_alive(world, parent)->node->child_count + 1 : 0;
	struct poor_world_node *above = nullptr;
	struct poor_world_node *node;
	struct h_world_entry *entry;

	assert(parent != POOR_WORLD_NO_HANDLE ? index < capacity : next->root == nullptr && index == 0);
	node = index_reserve(world) ? node_alloc(world, layout, 0) : nullptr;
	if (node != nullptr && parent != POOR_WORLD_NO_HANDLE) {
		above = path_touch(world, layout, parent, capacity);
		if (above == nullptr) {
			node_free(world, layout, node);
			node = nullptr;
		}
	}
	if (node == nullptr) {
		index_unreserve(world);
		return nullptr;
	}
	node_push(layout, &world->born, node);
	node->born = next->epoch;
	node->id = ++world->slots[world->next].id_high;
	node->handle = index_alloc(world);
	node->child_count = 0;
	entry = entry_of(world, node->handle);
	entry->node = node;
	entry->parent = parent;
	entry->slot = index;
	if (above != nullptr)
		children_insert(layout, above, index, node);
	else
		next->root = node;
	++next->node_count;
	next->handle_bound = world->index.page_count * PAGE_ENTRIES;
	return node;
}

static void
subtree_retire(struct h_world *world, const struct h_world_layout *layout, struct poor_world_node *victim)
{
	struct poor_world_version *next = version_next(world);
	struct poor_world_node *node = victim;

	for (;;) {
		while (node->child_count != 0) {
			node = node_children(layout, node)[0];
			entry_of(world, node->handle)->slot = 0;
		}
		for (;;) {
			struct h_world_entry *entry = entry_of(world, node->handle);
			const uint32_t slot = entry->slot;
			struct poor_world_node *parent;

			entry->node = nullptr;
			entry->slot = world->dead;
			world->dead = node->handle;
			--next->node_count;
			node_retire(world, layout, node, NODE_DESTROYED);
			if (node == victim)
				return;
			parent = entry_of(world, entry->parent)->node;
			if (slot + 1 < parent->child_count) {
				node = node_children(layout, parent)[slot + 1];
				entry_of(world, node->handle)->slot = slot + 1;
				break;
			}
			node = parent;
		}
	}
}

EXPORT bool
h_world_destroy(struct h_world *world, uint32_t handle)
{
	const struct h_world_layout *layout = &world->layout;
	struct h_world_entry *entry = entry_alive(world, handle);
	struct poor_world_node *parent;

	if (entry->parent == POOR_WORLD_NO_HANDLE) {
		version_next(world)->root = nullptr;
	} else {
		parent = path_touch(world, layout, entry->parent, 0);
		if (parent == nullptr)
			return false;
		children_remove(layout, parent, child_slot(layout, parent, entry));
	}
	subtree_retire(world, layout, entry->node);
	return true;
}

EXPORT struct poor_world_node *
h_world_copy(struct h_world *world, uint32_t handle)
{
	return path_touch(world, &world->layout, handle, 0);
}

[[maybe_unused]] static bool
handle_inside(const struct h_world *world, uint32_t handle, uint32_t ancestor)
{
	for (; handle != POOR_WORLD_NO_HANDLE; handle = entry_of(world, handle)->parent) {
		if (handle == ancestor)
			return true;
	}
	return false;
}

EXPORT bool
h_world_move(struct h_world *world, uint32_t handle, uint32_t parent,
	     uint32_t index)
{
	const struct h_world_layout *layout = &world->layout;
	struct h_world_entry *entry = entry_alive(world, handle);
	const uint32_t origin = entry->parent;
	const size_t count = entry_alive(world, parent)->node->child_count;
	const struct poor_world_node *born;
	struct poor_world_node *from;
	struct poor_world_node *to;

	assert(origin != POOR_WORLD_NO_HANDLE);
	if (origin == parent) {
		const uint32_t slot = child_slot(layout, entry_of(world, parent)->node, entry);

		assert(index < count);
		if (slot == index)
			return true;
		to = path_touch(world, layout, parent, 0);
		if (to == nullptr)
			return false;
		children_remove(layout, to, slot);
		children_insert(layout, to, index, entry->node);
		entry->slot = index;
		return true;
	}
	assert(index <= count && !handle_inside(world, parent, handle));
	born = world->born;
	if (path_touch(world, layout, origin, 0) == nullptr)
		return false;
	to = path_touch(world, layout, parent, count + 1);
	if (to == nullptr) {
		path_disown(world, layout, born);
		return false;
	}
	from = entry_of(world, origin)->node;
	children_remove(layout, from, child_slot(layout, from, entry));
	children_insert(layout, to, index, entry->node);
	entry->parent = parent;
	entry->slot = index;
	return true;
}
