/* SPDX-License-Identifier: MIT */
#ifndef POOR_WORLD_INTERNAL_H
#define POOR_WORLD_INTERNAL_H

#include <stdbit.h>

#include <poor_array.h>
#include <poor_asan.h>

#include "poor_world.h"

#define EXPORT [[gnu::visibility("default")]]

constexpr size_t BLOCK_UNIT = 64;
constexpr size_t SPAN_HEADER = 64;
constexpr uint32_t PAGE_ENTRIES = 128;
constexpr uint32_t PAGE_ORDER = 15;
constexpr uint32_t EVEN_ORDERS = 8;
constexpr uint32_t ORDER_STEPS = 4;

static inline size_t
block_bytes(uint32_t order)
{
	if (order < EVEN_ORDERS)
		return BLOCK_UNIT * (order + 1);
	return (BLOCK_UNIT * EVEN_ORDERS << ((order - EVEN_ORDERS) / ORDER_STEPS)) / ORDER_STEPS
	       * (ORDER_STEPS + 1 + (order - EVEN_ORDERS) % ORDER_STEPS);
}

static inline uint32_t
block_order(size_t bytes)
{
	const size_t units = (bytes + BLOCK_UNIT - 1) / BLOCK_UNIT;
	const unsigned width = stdc_bit_width(units - 1);
	const size_t step = units > EVEN_ORDERS ? (size_t)1 << (width - 3) : 1;

	if (units <= EVEN_ORDERS)
		return (uint32_t)units - 1;
	return EVEN_ORDERS + (width - 4) * ORDER_STEPS + (uint32_t)((units - ((size_t)1 << (width - 1)) + step - 1) / step) - 1;
}

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

#ifdef H_WORLD_FAULTS
extern uint64_t h_world_fault_countdown;
extern uint64_t h_world_faults;
#endif

[[nodiscard]] void *h_world_block_alloc(struct h_world_memory *memory, uint32_t order);
void h_world_block_free(struct h_world_memory *memory, void *block, uint32_t order);
void h_world_memory_init(struct h_world_memory *memory, size_t slab_bytes);
void h_world_memory_fini(struct h_world_memory *memory);

#endif
