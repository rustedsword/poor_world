/* SPDX-License-Identifier: MIT */
#include "internal.h"

#ifdef H_WORLD_FAULTS
uint64_t h_world_fault_countdown;
uint64_t h_world_faults;
#endif

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

static struct span *
span_find(struct h_world_memory *memory, size_t bytes)
{
	if (bytes <= memory->slab_bytes - SPAN_HEADER)
		return poor_list_first(&memory->empty);
	poor_list_foreach(&memory->empty_big, span) {
		if (span->bytes - SPAN_HEADER >= bytes)
			return span;
	}
	return nullptr;
}

struct span *
h_world_span_open(struct h_world_memory *memory, uint32_t order)
{
	const size_t bytes = block_bytes(order);
	const size_t slab = memory->slab_bytes;
	struct span *span = span_find(memory, bytes);

	if (span == nullptr) {
		memory->wanted = (bytes + SPAN_HEADER + slab - 1) / slab * slab;
		return nullptr;
	}
	poor_list_remove(spans_idle(memory, span), span);
	memory->idle_bytes -= span->bytes;
	span->order = order;
	span->free = nullptr;
	span->carved = 0;
	span->capacity = bytes <= slab - SPAN_HEADER ? (uint32_t)((span->bytes - SPAN_HEADER) / bytes) : 1;
	poor_list_append(&memory->partial[order], span);
	return span;
}

void
h_world_span_close(struct h_world_memory *memory, struct span *span)
{
	poor_list_remove(&memory->partial[span->order], span);
	span_idle(memory, span);
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
	poor_list_init(&memory->empty);
	poor_list_init(&memory->empty_big);
	poor_list_init(&memory->fed);
}

void
h_world_memory_fini(struct h_world_memory *memory)
{
	foreach_array_ref(memory->partial, spans)
		poor_list_init(spans);
	poor_list_init(&memory->empty);
	poor_list_init(&memory->empty_big);
	memory->idle_bytes = 0;
	poor_list_foreach(&memory->fed, span) {
		poor_asan_poison_region((unsigned char *)span + SPAN_HEADER, span->bytes - SPAN_HEADER);
		span_idle(memory, span);
	}
}

EXPORT void
h_world_feed(struct h_world *world, void *memory, size_t bytes)
{
	struct h_world_memory *m = &world->memory;
	struct span *span = memory;

	assert(bytes >= m->slab_bytes && bytes % m->slab_bytes == 0);
	assert((uintptr_t)memory % m->slab_bytes == 0);
	span->bytes = bytes;
	poor_asan_poison_region((unsigned char *)span + SPAN_HEADER, bytes - SPAN_HEADER);
	m->bytes += bytes;
	poor_list_append(&m->fed, span);
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
	poor_list_remove(&m->fed, span);
	m->bytes -= span->bytes;
	m->idle_bytes -= span->bytes;
	*bytes = span->bytes;
	poor_asan_unpoison_region(span, span->bytes);
	return span;
}
