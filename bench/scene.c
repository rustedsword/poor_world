/* SPDX-License-Identifier: MIT */
#include "bench.h"

constexpr uint32_t MAX_WINDOWS = 2000;
constexpr uint32_t ROUNDS = 20000;
constexpr size_t SLAB = 1 << 16;

struct window {
	struct poor_world_node node;
	int32_t x, y;
	char payload[224];
};

struct frame {
	struct poor_world_version version;
};

static_assert(sizeof(struct window) == 256);

poor_world_declare(scene, struct window, node, struct frame, version, 4);
poor_world_define(scene);

enum action {
	ACTION_COMMIT,
	ACTION_MOVE,
	ACTION_RAISE,
	ACTIONS,
};

static const char *const action_names[ACTIONS] = { "commit", "move", "raise" };

static scene s;
static uint32_t windows[MAX_WINDOWS];
static uint32_t subs[MAX_WINDOWS];
static uint32_t slots[4 * MAX_WINDOWS];
static struct poor_world_diff_match matches[8 * MAX_WINDOWS];
static uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t timer_cost;
static volatile uint64_t sink;
static size_t fed;

static uint64_t
rng(void)
{
	return bench_random(&seed);
}

static void
feed(void)
{
	const size_t bytes = scene_wanted(&s);
	void *memory = aligned_alloc(SLAB, bytes);

	if (memory == nullptr)
		abort();
	scene_feed(&s, memory, bytes);
	fed += bytes;
}

static uint32_t
create(uint32_t parent, uint32_t index)
{
	struct window *window;

	while ((window = scene_create(&s, parent, index)) == nullptr)
		feed();
	window->x = (int32_t)(rng() % 4096);
	window->y = (int32_t)(rng() % 4096);
	window->payload[0] = 0;
	return window->node.handle;
}

static void
apply(enum action action, uint32_t which)
{
	struct window *window;
	uint32_t parent;

	switch (action) {
	case ACTION_COMMIT:
		while ((window = scene_edit(&s, subs[which])) == nullptr)
			feed();
		++window->payload[0];
		break;
	case ACTION_MOVE:
		while ((window = scene_edit(&s, windows[which])) == nullptr)
			feed();
		window->x = (int32_t)(rng() % 4096);
		break;
	case ACTION_RAISE:
		parent = scene_parent(&s, windows[which]);
		while (!scene_move(&s, windows[which], parent, scene_node(&s, parent)->node.child_count - 1))
			feed();
		break;
	case ACTIONS:
		break;
	}
}

static uint64_t
changes(struct poor_world_diff_scratch *scratch, const struct window *old, const struct window *next)
{
	struct poor_world_diff diff;
	uint64_t count = old->x != next->x || old->payload[0] != next->payload[0];

	scene_diff_begin(&diff, scratch, old, next, false);
	while (scene_diff_next(&diff)) {
		const struct window *was = scene_diff_old(&diff), *is = scene_diff_new(&diff);

		if (was != nullptr && is != nullptr && !scene_diff_moved(&diff))
			count += changes(scratch, was, is);
		else
			++count;
	}
	return count;
}

static void
show(const char *what, struct bench_runs *runs)
{
	bench_sort(runs);
	printf("%s %4llu ns (%llu..%llu)", what, (unsigned long long)bench_median(runs), (unsigned long long)bench_low(runs),
	       (unsigned long long)bench_high(runs));
}

static void
bench(uint32_t count, uint32_t per_group)
{
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	struct poor_world_pin pin = POOR_WORLD_PIN_INIT;
	struct poor_world_diff_scratch scratch;
	struct bench_runs lookup;
	uint32_t layer, group;
	uint64_t found = 0;
	size_t bytes;

	scene_init(&s, SLAB);
	fed = 0;
	scene_begin(&s);
	layer = group = create(create(POOR_WORLD_NO_HANDLE, 0), 0);
	for (uint32_t i = 0; i < count; i++) {
		if (per_group != 0 && i % per_group == 0)
			group = create(layer, i / per_group);
		windows[i] = create(group, per_group != 0 ? i % per_group : i);
		subs[i] = create(windows[i], 0);
	}
	scene_publish(&s);
	scene_diff_scratch_init(&scratch, 4 * MAX_WINDOWS, slots, 8 * MAX_WINDOWS, matches);

	for (enum action action = 0; action < ACTIONS; action++) {
		struct bench_runs publish, diff;
		uint64_t changed = 0;

		foreach_array_index(publish.values, run) {
			uint64_t publish_ns = 0, diff_ns = 0;

			for (uint32_t round = 0; round < ROUNDS; round++) {
				const uint32_t which = (uint32_t)(rng() % count);
				const uint64_t start = bench_now();

				scene_begin(&s);
				apply(action, which);
				scene_publish(&s);
				publish_ns += bench_now() - start;
			}
			for (uint32_t round = 0; round < ROUNDS; round++) {
				const struct frame *old = scene_latest(&s, &hold), *next;
				uint64_t start;

				scene_pin(&s, &hold, &pin);
				scene_begin(&s);
				apply(action, (uint32_t)(rng() % count));
				scene_publish(&s);
				next = scene_latest(&s, &hold);
				start = bench_now();
				changed += changes(&scratch, scene_root(old), scene_root(next));
				diff_ns += bench_now() - start;
				scene_unpin(&s, &pin);
			}
			publish.values[run] = publish_ns / ROUNDS - timer_cost;
			diff.values[run] = diff_ns / ROUNDS - timer_cost;
		}
		printf("%4u windows, %2u per parent, %-6s:", count, per_group != 0 ? per_group : count, action_names[action]);
		show(" publish", &publish);
		show(", diff", &diff);
		printf(", %.2f changes\n", (double)changed / (ROUNDS * BENCH_REPEATS));
	}

	foreach_array_index(lookup.values, run) {
		const uint64_t start = bench_now();

		for (uint32_t round = 0; round < ROUNDS; round++)
			found += scene_node(&s, subs[rng() % count])->node.handle;
		lookup.values[run] = (bench_now() - start) / ROUNDS;
	}
	printf("%4u windows, %2u per parent, lookup:", count, per_group != 0 ? per_group : count);
	show("", &lookup);
	printf(", memory %zu KiB\n", fed >> 10);
	sink = found;

	scene_release(&s, &hold);
	scene_fini(&s);
	for (void *memory; (memory = scene_take(&s, &bytes)) != nullptr;)
		free(memory);
}

int
main(void)
{
	static constexpr uint32_t counts[] = { 16, 256, MAX_WINDOWS };

	timer_cost = bench_timer_cost();
	printf("256-byte nodes, every window has one child, %u rounds, median of %u runs (lowest..highest)\n", ROUNDS,
	       BENCH_REPEATS);
	printf("reading the clock twice takes %llu ns, subtracted from publish and diff\n", (unsigned long long)timer_cost);
	foreach_array_ref(counts, count) {
		bench(*count, 60);
		bench(*count, 0);
	}
	return 0;
}
