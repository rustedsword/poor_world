/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "bench.h"

constexpr uint32_t MAX_READERS = 8;
constexpr uint32_t MAX_CPUS = 1024;
constexpr uint64_t RUN_NS = UINT64_C(100000000);
constexpr size_t SLAB = 1 << 16;

struct window {
	struct poor_world_node node;
	int32_t x, y;
};

struct frame {
	struct poor_world_version version;
};

poor_world_declare(scene, struct window, node, struct frame, version, MAX_READERS + 2);
poor_world_define(scene);

enum phase : int {
	PHASE_WAIT,
	PHASE_RUN,
	PHASE_STOP,
};

struct core {
	uint32_t cpu;
	uint32_t first_sibling;
	uint32_t rank;
};

struct reader {
	alignas(64) pthread_t thread;
	uint32_t cpu;
	bool lets_go;
	uint64_t calls;
	uint64_t versions;
	uint64_t ns;
};

struct result {
	uint64_t call_ps;
	uint64_t publish_ns;
	uint64_t seen_percent;
};

static scene s;
static uint32_t child;
static struct core cores[MAX_CPUS];
static uint32_t core_count;
static struct reader readers[MAX_READERS];
alignas(64) static _Atomic enum phase phase;
alignas(64) static _Atomic uint32_t ready;

static bool
read_number(uint32_t cpu, const char *what, uint32_t *number)
{
	char path[128];
	FILE *file;
	bool ok;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/%s", cpu, what);
	file = fopen(path, "r");
	if (file == nullptr)
		return false;
	ok = fscanf(file, "%u", number) == 1;
	fclose(file);
	return ok;
}

static bool
core_found(uint32_t first_sibling)
{
	for (uint32_t i = 0; i < core_count; i++) {
		if (cores[i].first_sibling == first_sibling)
			return true;
	}
	return false;
}

static void
find_cores(void)
{
	cpu_set_t allowed;

	if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
		abort();
	for (uint32_t cpu = 0; cpu < MAX_CPUS && cpu < CPU_SETSIZE; cpu++) {
		uint32_t first_sibling, rank = 0, at = core_count;

		if (!CPU_ISSET(cpu, &allowed) || !read_number(cpu, "topology/thread_siblings_list", &first_sibling)
		    || core_found(first_sibling))
			continue;
		read_number(cpu, "acpi_cppc/highest_perf", &rank);
		for (; at > 0 && cores[at - 1].rank < rank; at--)
			cores[at] = cores[at - 1];
		cores[at] = (struct core){ .cpu = cpu, .first_sibling = first_sibling, .rank = rank };
		++core_count;
	}
}

static void
stay_on(uint32_t cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		abort();
}

static void
feed(void)
{
	const size_t bytes = scene_wanted(&s);
	void *memory = aligned_alloc(SLAB, bytes);

	if (memory == nullptr)
		abort();
	scene_feed(&s, memory, bytes);
}

static uint32_t
create(uint32_t parent)
{
	struct window *window;

	while ((window = scene_create(&s, parent, 0)) == nullptr)
		feed();
	window->x = window->y = 0;
	return window->node.handle;
}

static void
publish(void)
{
	struct window *window;

	scene_begin(&s);
	while ((window = scene_edit(&s, child)) == nullptr)
		feed();
	++window->x;
	scene_publish(&s);
}

static void *
read_versions(void *argument)
{
	struct reader *reader = argument;
	struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
	uint64_t calls = 0, versions = 0, epoch = UINT64_MAX, start;

	stay_on(reader->cpu);
	atomic_fetch_add(&ready, 1);
	while (atomic_load_explicit(&phase, memory_order_acquire) == PHASE_WAIT)
		;
	start = bench_now();
	while (atomic_load_explicit(&phase, memory_order_relaxed) == PHASE_RUN) {
		const struct frame *frame = scene_latest(&s, &hold);

		versions += frame->version.epoch != epoch;
		epoch = frame->version.epoch;
		if (reader->lets_go)
			scene_release(&s, &hold);
		++calls;
	}
	reader->ns = bench_now() - start;
	scene_release(&s, &hold);
	reader->calls = calls;
	reader->versions = versions;
	return nullptr;
}

static struct result
run(uint32_t count, bool lets_go, bool publishing)
{
	uint64_t published = 0, calls = 0, ns = 0, seen = 0, start, spent;

	atomic_store(&phase, PHASE_WAIT);
	atomic_store(&ready, 0);
	for (uint32_t i = 0; i < count; i++) {
		readers[i].cpu = cores[1 + i].cpu;
		readers[i].lets_go = lets_go;
		if (pthread_create(&readers[i].thread, nullptr, read_versions, &readers[i]) != 0)
			abort();
	}
	while (atomic_load(&ready) != count)
		;
	start = bench_now();
	atomic_store(&phase, PHASE_RUN);
	do {
		for (uint32_t i = 0; publishing && i < 16; i++, published++)
			publish();
		spent = bench_now() - start;
	} while (spent < RUN_NS);
	atomic_store(&phase, PHASE_STOP);
	for (uint32_t i = 0; i < count; i++) {
		if (pthread_join(readers[i].thread, nullptr) != 0)
			abort();
		calls += readers[i].calls;
		ns += readers[i].ns;
		seen += readers[i].versions;
	}
	scene_reclaim(&s);
	return (struct result){
		.call_ps = calls != 0 ? ns * 1000 / calls : 0,
		.publish_ns = published != 0 ? spent / published : 0,
		.seen_percent = published != 0 && count != 0 ? seen * 100 / count / published : 0,
	};
}

static void
show(struct bench_runs *runs, uint64_t divisor, const char *unit, const char *end)
{
	char cell[64];

	bench_sort(runs);
	snprintf(cell, sizeof(cell), "%.1f %s (%.1f..%.1f)", (double)bench_median(runs) / (double)divisor, unit,
		 (double)bench_low(runs) / (double)divisor, (double)bench_high(runs) / (double)divisor);
	printf(end != nullptr ? "  %s%s" : "  %-24s", cell, end);
}

static void
measure(uint32_t count)
{
	struct bench_runs hold, take, busy, publish_ns, seen;

	foreach_array_index(hold.values, i) {
		const struct result quiet = run(count, false, false), churn = run(count, true, false);
		const struct result both = run(count, false, true);

		hold.values[i] = quiet.call_ps;
		take.values[i] = churn.call_ps;
		busy.values[i] = both.call_ps;
		publish_ns.values[i] = both.publish_ns;
		seen.values[i] = both.seen_percent;
	}
	printf("%7u", count);
	if (count == 0) {
		printf("  %-24s  %-24s  %-24s", "", "", "");
		show(&publish_ns, 1, "ns", "\n");
		return;
	}
	show(&hold, 1000, "ns", nullptr);
	show(&take, 1000, "ns", nullptr);
	show(&busy, 1000, "ns", nullptr);
	show(&publish_ns, 1, "ns", nullptr);
	show(&seen, 1, "%", "\n");
}

int
main(void)
{
	static constexpr uint32_t counts[] = { 0, 1, 2, 3, 4, 8 };
	size_t bytes;

	find_cores();
	if (core_count < 2) {
		printf("needs two cores\n");
		return 77;
	}
	stay_on(cores[0].cpu);
	printf("the writer on CPU %u, readers on CPUs", cores[0].cpu);
	for (uint32_t i = 1; i < core_count && i <= MAX_READERS; i++)
		printf(" %u", cores[i].cpu);
	printf("; %llu ms a run, median of %u runs (lowest..highest)\n", (unsigned long long)(RUN_NS / 1000000),
	       BENCH_REPEATS);
	printf("%7s  %-50s  %s\n", "", "the writer does nothing", "the writer publishes as fast as it can");
	printf("%7s  %-24s  %-24s  %-24s  %-24s  %s\n", "readers", "latest()", "latest() and release()", "latest()",
	       "publish()", "versions a reader saw");

	scene_init(&s, SLAB);
	scene_begin(&s);
	child = create(create(POOR_WORLD_NO_HANDLE));
	scene_publish(&s);

	foreach_array_ref(counts, count) {
		if (*count < core_count)
			measure(*count);
	}
	scene_fini(&s);
	for (void *memory; (memory = scene_take(&s, &bytes)) != nullptr;)
		free(memory);
	return 0;
}
