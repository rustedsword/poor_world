/* SPDX-License-Identifier: MIT */
#ifndef POOR_WORLD_BENCH_H
#define POOR_WORLD_BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <poor_array.h>
#include <poor_world.h>

constexpr uint32_t BENCH_REPEATS = 5;

struct bench_runs {
	uint64_t values[BENCH_REPEATS];
};

static inline uint64_t
bench_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static inline uint64_t
bench_random(uint64_t *seed)
{
	*seed ^= *seed << 13;
	*seed ^= *seed >> 7;
	*seed ^= *seed << 17;
	return *seed;
}

[[maybe_unused]] static uint64_t
bench_timer_cost(void)
{
	constexpr uint32_t rounds = 100000;
	uint64_t lowest = UINT64_MAX;

	for (uint32_t batch = 0; batch < 20; batch++) {
		uint64_t total = 0;

		for (uint32_t i = 0; i < rounds; i++) {
			const uint64_t start = bench_now();

			total += bench_now() - start;
		}
		if (total / rounds < lowest)
			lowest = total / rounds;
	}
	return lowest;
}

static inline void
bench_sort(struct bench_runs *runs)
{
	foreach_array_index(runs->values, i) {
		for (size_t j = i; j > 0 && runs->values[j - 1] > runs->values[j]; j--) {
			const uint64_t swap = runs->values[j];

			runs->values[j] = runs->values[j - 1];
			runs->values[j - 1] = swap;
		}
	}
}

static inline uint64_t
bench_median(const struct bench_runs *runs)
{
	return runs->values[BENCH_REPEATS / 2];
}

static inline uint64_t
bench_low(const struct bench_runs *runs)
{
	return runs->values[0];
}

static inline uint64_t
bench_high(const struct bench_runs *runs)
{
	return runs->values[BENCH_REPEATS - 1];
}

#endif
