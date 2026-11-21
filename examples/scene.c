/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include <poor_stdio.h>
#include <poor_world.h>

constexpr size_t SLAB = 65536;

struct window {
	const char *title;
	struct poor_world_node node;
};

struct frame {
	struct poor_world_version version;
	const char *reason;
};

poor_world_declare(scene, struct window, node, struct frame, version, 4);

struct reader {
	struct poor_world_hold hold;
	struct poor_world_pin pin;
	struct poor_world_diff_scratch scratch;
	uint32_t slots[64];
	struct poor_world_diff_match matches[128];
};

static scene s;
static size_t fed, returned;

static void
feed(void)
{
	const size_t bytes = poor_world_wanted(&s);
	void *memory = aligned_alloc(SLAB, bytes);

	if (memory == nullptr)
		abort();
	poor_world_feed(&s, memory, bytes);
	fed += bytes;
}

static void
give_back(void)
{
	size_t bytes;

	for (void *memory; (memory = poor_world_take(&s, &bytes)) != nullptr; returned += bytes)
		free(memory);
}

static uint32_t
open_window(uint32_t parent, const char *title)
{
	const uint32_t on_top = parent != POOR_WORLD_NO_HANDLE ? poor_world_node(&s, parent)->node.child_count : 0;
	struct window *window;

	while ((window = poor_world_create(&s, parent, on_top)) == nullptr)
		feed();
	window->title = title;
	return window->node.handle;
}

static void
rename_window(uint32_t handle, const char *title)
{
	struct window *window;

	while ((window = poor_world_edit(&s, handle)) == nullptr)
		feed();
	window->title = title;
}

static void
raise_window(uint32_t handle)
{
	const uint32_t parent = poor_world_parent(&s, handle);

	while (!poor_world_move(&s, handle, parent, poor_world_node(&s, parent)->node.child_count - 1))
		feed();
}

static void
close_window(uint32_t handle)
{
	while (!poor_world_destroy(&s, handle))
		feed();
}

static void
show_tree(const struct window *window, const struct window *parent)
{
	println("  + ", window->title, parent != nullptr ? " in " : "", parent != nullptr ? parent->title : "");
	for (uint32_t i = 0; i < window->node.child_count; i++)
		show_tree(poor_world_child(&s, window, i), window);
}

static void
show_changes(struct reader *reader, const struct window *was, const struct window *is)
{
	struct poor_world_diff diff;

	if (was->title != is->title)
		println("  ~ ", was->title, " is now ", is->title);
	poor_world_diff_begin(&s, &diff, &reader->scratch, was, is, false);
	while (poor_world_diff_next(&diff)) {
		const struct window *old = poor_world_diff_old(&s, &diff), *new = poor_world_diff_new(&s, &diff);

		if (old == nullptr) {
			show_tree(new, is);
		} else if (new == nullptr) {
			println("  - ", old->title);
		} else {
			if (poor_world_diff_moved(&diff))
				println("  ^ ", new->title, " restacked in ", is->title);
			show_changes(reader, old, new);
		}
	}
}

static void
draw(struct reader *reader)
{
	const struct frame *frame = poor_world_latest(&s, &reader->hold);
	const struct frame *seen = poor_world_pinned(&s, &reader->pin);

	if (frame == seen) {
		println("no new frame");
		return;
	}
	println("frame ", frame->version.epoch, ", ", frame->reason, ": ", frame->version.node_count, " windows");
	if (seen == nullptr)
		show_tree(poor_world_root(&s, frame), nullptr);
	else
		show_changes(reader, poor_world_root(&s, seen), poor_world_root(&s, frame));
	poor_world_pin(&s, &reader->hold, &reader->pin);
}

int
main(void)
{
	struct reader reader = { .hold = POOR_WORLD_HOLD_INIT, .pin = POOR_WORLD_PIN_INIT };
	uint32_t desktop, terminal, browser, downloads;

	poor_world_diff_scratch_init(&reader.scratch, 64, reader.slots, 128, reader.matches);
	poor_world_init(&s, SLAB)->reason = "nothing yet";

	poor_world_begin(&s)->reason = "session started";
	desktop = open_window(POOR_WORLD_NO_HANDLE, "desktop");
	terminal = open_window(desktop, "terminal");
	browser = open_window(desktop, "browser");
	downloads = open_window(browser, "downloads");
	poor_world_publish(&s);
	draw(&reader);

	poor_world_begin(&s)->reason = "terminal raised, page loaded";
	raise_window(terminal);
	rename_window(browser, "browser: example.org");
	poor_world_publish(&s);
	draw(&reader);

	poor_world_begin(&s)->reason = "dialog opened, downloads closed";
	open_window(terminal, "save as");
	close_window(downloads);
	poor_world_publish(&s);
	draw(&reader);

	poor_world_begin(&s)->reason = "never shown";
	close_window(browser);
	poor_world_abort(&s);
	draw(&reader);

	poor_world_begin(&s)->reason = "browser closed";
	close_window(browser);
	poor_world_publish(&s);
	println("the reader still has frame ", poor_world_held(&s, &reader.hold)->version.epoch, " with ",
		poor_world_root(&s, poor_world_held(&s, &reader.hold))->node.child_count, " windows on the desktop; ",
		poor_world_stats(&s).versions, " versions are alive");
	draw(&reader);

	poor_world_release(&s, &reader.hold);
	poor_world_unpin(&s, &reader.pin);
	poor_world_reclaim(&s);
	println(poor_world_stats(&s).versions, " version is alive, ", poor_world_stats(&s).nodes, " nodes");

	poor_world_fini(&s);
	give_back();
	println("fed ", fed, " bytes, got back ", returned);
	return fed == returned ? 0 : 1;
}
