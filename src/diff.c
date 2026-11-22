/* SPDX-License-Identifier: MIT */
#include "internal.h"

enum phase : uint8_t {
	PHASE_HEAD,
	PHASE_TAIL,
	PHASE_ADDED,
	PHASE_REMOVED,
	PHASE_COMMON,
	PHASE_DONE,
};

EXPORT void
poor_world_diff_scratch_init(struct poor_world_diff_scratch *scratch, uint32_t slot_count, uint32_t *slots,
			     uint32_t match_count, struct poor_world_diff_match *matches)
{
	fill_array(array_ptr(slots, slot_count), 0);
	scratch->slots = slots;
	scratch->matches = matches;
	scratch->slot_count = slot_count;
	scratch->match_count = match_count;
	scratch->top = 0;
}

EXPORT void
h_world_diff_begin(struct poor_world_diff *diff, struct poor_world_diff_scratch *scratch, const struct poor_world_node *old,
		   const struct poor_world_node *next, size_t children_at, bool all)
{
	diff->scratch = scratch;
	diff->old = h_world_children(old, children_at);
	diff->new = h_world_children(next, children_at);
	diff->old_count = old->child_count;
	diff->new_count = next->child_count;
	diff->head = 0;
	diff->tail = 0;
	diff->base = scratch->top;
	diff->phase = PHASE_HEAD;
	diff->all = all;
}

static void
mark_stable(uint32_t count, struct poor_world_diff_match *matches)
{
	uint32_t length = 0;

	foreach_array_index(array_ptr(matches, count), k) {
		uint32_t low = 0, high = length;

		while (low < high) {
			const uint32_t middle = (low + high) / 2;

			if (matches[matches[middle].tail].old_index < matches[k].old_index)
				low = middle + 1;
			else
				high = middle;
		}
		matches[low].tail = (uint32_t)k;
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

static bool
same(const struct poor_world_node *old, const struct poor_world_node *next)
{
	return old == next || old->id == next->id;
}

static bool
match_rotation(uint32_t count, struct poor_world_node *const *old, struct poor_world_node *const *new,
	       struct poor_world_diff_match *matches)
{
	bool up = same(old[0], new[count - 1]), down = same(old[count - 1], new[0]);

	for (uint32_t i = 1; i < count && (up || down); i++) {
		up = up && same(old[i], new[i - 1]);
		down = down && same(old[i - 1], new[i]);
	}
	if (!up && !down)
		return false;
	for (uint32_t j = 0; j < count; j++) {
		matches[j].old_index = up ? (j + 1) % count : (j + count - 1) % count;
		matches[j].new_index = j;
		matches[j].stable = j != (up ? count - 1 : 0);
	}
	return true;
}

[[gnu::noinline]] static void
match_middle(struct poor_world_diff *diff)
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
	if (old_count == new_count && old_count >= 2 && match_rotation(old_count, old, new, matches)) {
		diff->common = old_count;
		diff->removed = 0;
		diff->at = new_count;
		scratch->top = diff->base + new_count;
		return;
	}
	foreach_array_index(array_ptr(old, old_count), i) {
		assert(old[i]->handle < scratch->slot_count);
		slots[old[i]->handle] = (uint32_t)i + 1;
	}
	foreach_array_index(array_ptr(new, new_count), j) {
		const uint32_t slot = new[j]->handle < scratch->slot_count ? slots[new[j]->handle] : 0;

		if (slot != 0 && old[slot - 1]->id == new[j]->id) {
			matches[common].old_index = slot - 1;
			matches[common].new_index = (uint32_t)j;
			++common;
			slots[new[j]->handle] = 0;
		} else {
			matches[new_count - 1 - (j - common)].new_index = (uint32_t)j;
		}
	}
	foreach_array_index(array_ptr(old, old_count), i) {
		if (slots[old[i]->handle] != 0) {
			slots[old[i]->handle] = 0;
			matches[new_count + removed++].old_index = (uint32_t)i;
		}
	}
	mark_stable(common, matches);
	diff->common = common;
	diff->removed = removed;
	diff->at = new_count;
	scratch->top = diff->base + new_count + removed;
}

static bool
yield(struct poor_world_diff *diff, const struct poor_world_node *old, const struct poor_world_node *next, bool moved)
{
	diff->old_node = old;
	diff->new_node = next;
	diff->moved = moved;
	return true;
}

static uint32_t
unchanged_head(struct poor_world_node *const *old, struct poor_world_node *const *new, uint32_t count)
{
	uint32_t run = 0;

	while (run < count && old[run] == new[run])
		++run;
	return run;
}

static uint32_t
unchanged_tail(struct poor_world_node *const *old, struct poor_world_node *const *new, uint32_t count)
{
	uint32_t run = 0;

	while (run < count && old[-(ptrdiff_t)run] == new[-(ptrdiff_t)run])
		++run;
	return run;
}

static bool
scan_head(struct poor_world_diff *diff, uint32_t shared)
{
	struct poor_world_node *const *old = &diff->old[diff->head], *const *new = &diff->new[diff->head];
	uint32_t head = diff->head;

	if (!diff->all)
		head += unchanged_head(old, new, shared - head);
	diff->head = head;
	if (head == shared || !same(diff->old[head], diff->new[head]))
		return false;
	diff->head = head + 1;
	return yield(diff, diff->old[head], diff->new[head], false);
}

static bool
scan_tail(struct poor_world_diff *diff, uint32_t shared)
{
	const uint32_t rest = shared - diff->head;
	uint32_t tail = diff->tail;
	const struct poor_world_node *old, *next;

	if (!diff->all && tail < rest)
		tail += unchanged_tail(&diff->old[diff->old_count - 1 - tail], &diff->new[diff->new_count - 1 - tail],
				       rest - tail);
	diff->tail = tail;
	if (tail == rest)
		return false;
	old = diff->old[diff->old_count - 1 - tail];
	next = diff->new[diff->new_count - 1 - tail];
	if (!same(old, next))
		return false;
	diff->tail = tail + 1;
	return yield(diff, old, next, false);
}

static bool
scan_common(struct poor_world_diff *diff, const struct poor_world_diff_match *matches)
{
	struct poor_world_node *const *olds = &diff->old[diff->head], *const *news = &diff->new[diff->head];
	const uint32_t common = diff->common;
	const bool all = diff->all;

	for (uint32_t at = diff->at; at < common; at++) {
		const struct poor_world_diff_match *match = &matches[at];
		const struct poor_world_node *old = olds[match->old_index], *next = news[match->new_index];

		if (!match->stable || all || old != next) {
			diff->at = at + 1;
			return yield(diff, old, next, !match->stable);
		}
	}
	return false;
}

EXPORT void
poor_world_diff_stop(struct poor_world_diff *diff)
{
	diff->scratch->top = diff->base;
	diff->phase = PHASE_DONE;
}

[[gnu::noinline]] static bool
next_matched(struct poor_world_diff *diff)
{
	const uint32_t new_count = diff->new_count - diff->head - diff->tail;
	const struct poor_world_diff_match *matches = &diff->scratch->matches[diff->base];

	switch ((enum phase)diff->phase) {
	case PHASE_ADDED:
		if (diff->at > diff->common)
			return yield(diff, nullptr, diff->new[diff->head + matches[--diff->at].new_index], false);
		diff->at = 0;
		diff->phase = PHASE_REMOVED;
		[[fallthrough]];
	case PHASE_REMOVED:
		if (diff->at < diff->removed)
			return yield(diff, diff->old[diff->head + matches[new_count + diff->at++].old_index], nullptr, false);
		diff->at = 0;
		diff->phase = PHASE_COMMON;
		[[fallthrough]];
	case PHASE_COMMON:
		if (scan_common(diff, matches))
			return true;
		poor_world_diff_stop(diff);
		return false;
	default:
		return false;
	}
}

EXPORT bool
poor_world_diff_next(struct poor_world_diff *diff)
{
	const uint32_t shared = diff->old_count < diff->new_count ? diff->old_count : diff->new_count;

	if (diff->phase == PHASE_HEAD) {
		if (scan_head(diff, shared))
			return true;
		diff->phase = PHASE_TAIL;
	}
	if (diff->phase == PHASE_TAIL) {
		if (scan_tail(diff, shared))
			return true;
		if (diff->head + diff->tail == diff->old_count && diff->head + diff->tail == diff->new_count) {
			diff->phase = PHASE_DONE;
			return false;
		}
		match_middle(diff);
		diff->phase = PHASE_ADDED;
	}
	return next_matched(diff);
}
