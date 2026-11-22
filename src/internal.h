/* SPDX-License-Identifier: MIT */
#ifndef POOR_WORLD_INTERNAL_H
#define POOR_WORLD_INTERNAL_H

#include <stdbit.h>

#include <poor_array.h>
#include <poor_asan.h>
#include <poor_list.h>

#include "poor_world.h"

#define EXPORT [[gnu::visibility("default")]]

constexpr size_t BLOCK_UNIT = 64;
constexpr size_t SPAN_HEADER = 64;
constexpr uint32_t PAGE_ENTRIES = H_WORLD_PAGE_ENTRIES;
constexpr uint32_t PAGE_ORDER = 15;
constexpr uint32_t EVEN_ORDERS = 8;
constexpr uint32_t ORDER_STEPS = 4;

static inline size_t
block_bytes(uint32_t order)
{
	if (order < EVEN_ORDERS)
		return BLOCK_UNIT * (order + 1);
	return (BLOCK_UNIT * EVEN_ORDERS / ORDER_STEPS << ((order - EVEN_ORDERS) / ORDER_STEPS))
	       * (ORDER_STEPS + 1 + (order - EVEN_ORDERS) % ORDER_STEPS);
}

static inline uint32_t
block_order(size_t bytes)
{
	const size_t last = (bytes - 1) / BLOCK_UNIT;
	const unsigned width = stdc_bit_width(last);

	assert(bytes != 0);
	if (last < EVEN_ORDERS)
		return (uint32_t)last;
	return ORDER_STEPS * width - 12 + (uint32_t)(last >> (width - 3));
}

static_assert(EVEN_ORDERS == 8 && ORDER_STEPS == 4);

enum node_state : uint8_t {
	NODE_LIVE,
	NODE_REPLACED,
	NODE_DESTROYED,
	NODE_FREE,
};

struct node_header {
	struct poor_world_node *link;
	uint8_t order;
	enum node_state state;
};

static_assert(sizeof(struct node_header) == H_WORLD_HEADER);

struct span {
	struct poor_list_node link;
	struct poor_list_node fed;
	void *free;
	size_t bytes;
	uint32_t live;
	uint32_t carved;
	uint32_t capacity;
	uint32_t order;
};

static_assert(sizeof(struct span) <= SPAN_HEADER);

poor_list_define(h_world_spans, struct span, link);
poor_list_define(h_world_fed, struct span, fed);

#ifdef H_WORLD_FAULTS
extern uint64_t h_world_fault_countdown;
extern uint64_t h_world_faults;
#endif

[[nodiscard]] struct span *h_world_span_open(struct h_world_memory *memory, uint32_t order);
void h_world_span_close(struct h_world_memory *memory, struct span *span);
void h_world_memory_init(struct h_world_memory *memory, size_t slab_bytes);
void h_world_memory_fini(struct h_world_memory *memory);

[[nodiscard]] static inline void *
h_world_block_alloc(struct h_world_memory *memory, uint32_t order)
{
	struct span *span = poor_list_first(&memory->partial[order]);
	void *block;

#ifdef H_WORLD_FAULTS
	if (h_world_fault_countdown != 0 && --h_world_fault_countdown == 0) {
		++h_world_faults;
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
		poor_asan_unpoison_region(block, block_bytes(order));
		span->free = *(void **)block;
	} else {
		block = (unsigned char *)span + SPAN_HEADER + span->carved++ * block_bytes(order);
		poor_asan_unpoison_region(block, block_bytes(order));
	}
	if (++span->live == span->capacity)
		poor_list_remove(&memory->partial[order], span);
	return block;
}

static inline void
h_world_block_free(struct h_world_memory *memory, void *block)
{
	struct span *span = (void *)((unsigned char *)block - ((uintptr_t)block & (memory->slab_bytes - 1)));

	*(void **)block = span->free;
	span->free = block;
	poor_asan_poison_region(block, block_bytes(span->order));
	if (span->live == span->capacity)
		poor_list_prepend(&memory->partial[span->order], span);
	if (--span->live == 0)
		h_world_span_close(memory, span);
}

#endif
