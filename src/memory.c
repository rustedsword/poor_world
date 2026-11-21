/* SPDX-License-Identifier: MIT */
#include "internal.h"

struct span {
	struct poor_list_node link;
	void *free;
	size_t bytes;
	uint32_t live;
	uint32_t carved;
	uint32_t capacity;
	uint32_t order;
};

poor_list_define(h_world_spans, struct span, link);

static_assert(sizeof(struct span) <= SPAN_HEADER);

#ifdef H_WORLD_FAULTS
uint64_t h_world_fault_countdown;
uint64_t h_world_faults;
#endif

static void *
span_block(struct span *span, size_t offset)
{
	return (unsigned char *)span + SPAN_HEADER + offset;
}

static struct span *
block_span(const struct h_world_memory *memory, void *block)
{
	return (void *)((unsigned char *)block - ((uintptr_t)block & (memory->slab_bytes - 1)));
}

static h_world_spans *
spans_idle(struct h_world_memory *memory, const struct span *span)
{
	return span->bytes == memory->slab_bytes ? &memory->empty : &memory->empty_big;
}

static void
span_idle(struct h_world_memory *memory, struct span *span)
{
	span->live = 0;
	poor_list_append(spans_idle(memory, span), span);
	memory->idle_bytes += span->bytes;
}

static void
span_use(struct h_world_memory *memory, struct span *span, uint32_t order)
{
	poor_list_remove(spans_idle(memory, span), span);
	memory->idle_bytes -= span->bytes;
	span->order = order;
	span->free = nullptr;
	span->carved = 0;
	span->capacity = (uint32_t)((span->bytes - SPAN_HEADER) / (block_bytes(order)));
}

static bool
block_is_big(const struct h_world_memory *memory, uint32_t order)
{
	return block_bytes(order) > memory->slab_bytes - SPAN_HEADER;
}

static void *
big_alloc(struct h_world_memory *memory, uint32_t order)
{
	const size_t bytes = block_bytes(order);

	poor_list_foreach(&memory->empty_big, span) {
		if (span->bytes - SPAN_HEADER < bytes)
			continue;
		span_use(memory, span, order);
		span->live = 1;
		poor_list_append(&memory->full, span);
		poor_asan_unpoison_region(span_block(span, 0), bytes);
		return span_block(span, 0);
	}
	memory->wanted = (bytes + SPAN_HEADER + memory->slab_bytes - 1) / memory->slab_bytes * memory->slab_bytes;
	return nullptr;
}

void *
h_world_block_alloc(struct h_world_memory *memory, uint32_t order)
{
	const size_t bytes = block_bytes(order);
	struct span *span = poor_list_first(&memory->partial[order]);
	void *block;

#ifdef H_WORLD_FAULTS
	if (h_world_fault_countdown != 0 && --h_world_fault_countdown == 0) {
		++h_world_faults;
		memory->wanted = 0;
		return nullptr;
	}
#endif
	if (block_is_big(memory, order))
		return big_alloc(memory, order);
	if (span == nullptr) {
		span = poor_list_first(&memory->empty);
		if (span == nullptr) {
			memory->wanted = memory->slab_bytes;
			return nullptr;
		}
		span_use(memory, span, order);
		poor_list_append(&memory->partial[order], span);
	}
	if (span->free != nullptr) {
		block = span->free;
		poor_asan_unpoison_region(block, bytes);
		span->free = *(void **)block;
	} else {
		block = span_block(span, span->carved++ * bytes);
		poor_asan_unpoison_region(block, bytes);
	}
	if (++span->live == span->capacity) {
		poor_list_remove(&memory->partial[order], span);
		poor_list_append(&memory->full, span);
	}
	return block;
}

void
h_world_block_free(struct h_world_memory *memory, void *block, uint32_t order)
{
	const size_t bytes = block_bytes(order);
	struct span *span = block_span(memory, block);

	if (block_is_big(memory, order)) {
		poor_asan_poison_region(block, bytes);
		poor_list_remove(&memory->full, span);
		span_idle(memory, span);
		return;
	}
	*(void **)block = span->free;
	span->free = block;
	poor_asan_poison_region(block, bytes);
	if (span->live == span->capacity) {
		poor_list_remove(&memory->full, span);
		poor_list_prepend(&memory->partial[order], span);
	}
	if (--span->live == 0) {
		poor_list_remove(&memory->partial[order], span);
		span_idle(memory, span);
	}
}

void
h_world_memory_init(struct h_world_memory *memory, size_t slab_bytes)
{
	assert(slab_bytes >= POOR_WORLD_MIN_SLAB && stdc_has_single_bit(slab_bytes));
	assert(block_bytes(PAGE_ORDER) == PAGE_ENTRIES * sizeof(struct h_world_entry));
	memory->slab_bytes = slab_bytes;
	memory->wanted = 0;
	memory->bytes = 0;
	memory->idle_bytes = 0;
	foreach_array_ref(memory->partial, spans)
		poor_list_init(spans);
	poor_list_init(&memory->full);
	poor_list_init(&memory->empty);
	poor_list_init(&memory->empty_big);
}

static void
spans_release(struct h_world_memory *memory, h_world_spans *spans)
{
	poor_list_foreach_safe(spans, span) {
		poor_list_remove(spans, span);
		poor_asan_poison_region(span_block(span, 0), span->bytes - SPAN_HEADER);
		span_idle(memory, span);
	}
}

void
h_world_memory_fini(struct h_world_memory *memory)
{
	foreach_array_ref(memory->partial, spans)
		spans_release(memory, spans);
	spans_release(memory, &memory->full);
}

EXPORT void
h_world_feed(struct h_world *world, void *memory, size_t bytes)
{
	struct h_world_memory *m = &world->memory;
	struct span *span = memory;

	assert(bytes >= m->slab_bytes && bytes % m->slab_bytes == 0);
	assert((uintptr_t)memory % m->slab_bytes == 0);
	span->bytes = bytes;
	poor_asan_poison_region(span_block(span, 0), bytes - SPAN_HEADER);
	m->bytes += bytes;
	span_idle(m, span);
}

EXPORT void *
h_world_take(struct h_world *world, size_t *bytes)
{
	struct h_world_memory *m = &world->memory;
	struct span *span = poor_list_first(&m->empty_big);

	if (span == nullptr)
		span = poor_list_last(&m->empty);
	if (span == nullptr)
		return nullptr;
	poor_list_remove(spans_idle(m, span), span);
	m->bytes -= span->bytes;
	m->idle_bytes -= span->bytes;
	*bytes = span->bytes;
	poor_asan_unpoison_region(span, span->bytes);
	return span;
}
