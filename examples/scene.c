/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include <poor_stdio.h>
#include <poor_world.h>

constexpr size_t SLAB = 4096;
constexpr uint32_t ICONS = 200;
constexpr uint32_t LISTED = 4;

struct window {
	const char *title;
	struct poor_world_node node;
};

struct frame {
	struct poor_world_version version;
	const char *reason;
};

poor_world_declare(scene, struct window, node, struct frame, version, 4);
poor_world_define(scene);

struct reader {
	struct poor_world_hold hold;
	struct poor_world_pin pin;
	struct poor_world_diff_scratch scratch;
	uint32_t slots[256];
	struct poor_world_diff_match matches[512];
};

static scene s;
static size_t fed, returned;

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

static size_t
give_back(void)
{
	size_t bytes, total = 0;

	for (void *memory; (memory = scene_take(&s, &bytes)) != nullptr; total += bytes)
		free(memory);
	returned += total;
	return total;
}

static uint32_t
open_window(uint32_t parent, const char *title)
{
	const uint32_t on_top = parent != POOR_WORLD_NO_HANDLE ? scene_node(&s, parent)->node.child_count : 0;
	struct window *window;

	while ((window = scene_create(&s, parent, on_top)) == nullptr)
		feed();
	window->title = title;
	return window->node.handle;
}

static void
rename_window(uint32_t handle, const char *title)
{
	struct window *window;

	while ((window = scene_edit(&s, handle)) == nullptr)
		feed();
	window->title = title;
}

static void
raise_window(uint32_t handle)
{
	const uint32_t parent = scene_parent(&s, handle);

	while (!scene_move(&s, handle, parent, scene_node(&s, parent)->node.child_count - 1))
		feed();
}

static void
close_window(uint32_t handle)
{
	while (!scene_destroy(&s, handle))
		feed();
}

static void
show_tree(const struct window *window, const struct window *parent)
{
	println("  + ", window->title, parent != nullptr ? " in " : "", parent != nullptr ? parent->title : "");
	if (window->node.child_count > LISTED) {
		println("    with ", window->node.child_count, " windows in it");
		return;
	}
	for (uint32_t i = 0; i < window->node.child_count; i++)
		show_tree(scene_child(window, i), window);
}

static void
show_changes(struct reader *reader, const struct window *was, const struct window *is)
{
	struct poor_world_diff diff;

	if (was->title != is->title)
		println("  ~ ", was->title, " is now ", is->title);
	scene_diff_begin(&diff, &reader->scratch, was, is, false);
	while (scene_diff_next(&diff)) {
		const struct window *old = scene_diff_old(&diff), *new = scene_diff_new(&diff);

		if (old == nullptr) {
			show_tree(new, is);
		} else if (new == nullptr) {
			println("  - ", old->title);
		} else {
			if (scene_diff_moved(&diff))
				println("  ^ ", new->title, " restacked in ", is->title);
			show_changes(reader, old, new);
		}
	}
}

static void
draw(struct reader *reader)
{
	const struct frame *frame = scene_latest(&s, &reader->hold);
	const struct frame *seen = scene_pinned(&s, &reader->pin);

	if (frame == seen) {
		println("no new frame");
		return;
	}
	println("frame ", frame->version.epoch, ", ", frame->reason, ": ", frame->version.node_count, " windows");
	if (seen == nullptr)
		show_tree(scene_root(frame), nullptr);
	else
		show_changes(reader, scene_root(seen), scene_root(frame));
	scene_pin(&s, &reader->hold, &reader->pin);
}

int
main(void)
{
	struct reader reader = { .hold = POOR_WORLD_HOLD_INIT, .pin = POOR_WORLD_PIN_INIT };
	uint32_t desktop, terminal, browser, downloads, launcher;
	size_t early;

	scene_diff_scratch_init(&reader.scratch, 256, reader.slots, 512, reader.matches);
	scene_init(&s, SLAB)->reason = "nothing yet";

	scene_begin(&s)->reason = "session started";
	desktop = open_window(POOR_WORLD_NO_HANDLE, "desktop");
	terminal = open_window(desktop, "terminal");
	browser = open_window(desktop, "browser");
	downloads = open_window(browser, "downloads");
	scene_publish(&s);
	draw(&reader);

	scene_begin(&s)->reason = "terminal raised, page loaded";
	raise_window(terminal);
	rename_window(browser, "browser: example.org");
	scene_publish(&s);
	draw(&reader);

	scene_begin(&s)->reason = "dialog opened, downloads closed";
	open_window(terminal, "save as");
	close_window(downloads);
	scene_publish(&s);
	draw(&reader);

	scene_begin(&s)->reason = "launcher opened";
	launcher = open_window(desktop, "launcher");
	for (uint32_t i = 0; i < ICONS; i++)
		open_window(launcher, "icon");
	scene_publish(&s);
	draw(&reader);

	scene_begin(&s)->reason = "launcher closed";
	close_window(launcher);
	scene_publish(&s);
	println("gave back ", give_back(), " bytes while the reader still has the launcher");
	draw(&reader);
	scene_reclaim(&s);
	early = give_back();
	println("gave back ", early, " bytes more after the reader let go of it");

	scene_begin(&s)->reason = "never shown";
	close_window(browser);
	scene_abort(&s);
	draw(&reader);

	scene_begin(&s)->reason = "browser closed";
	close_window(browser);
	scene_publish(&s);
	println("the reader still has frame ", scene_held(&s, &reader.hold)->version.epoch, " with ",
		scene_root(scene_held(&s, &reader.hold))->node.child_count, " windows on the desktop; ",
		scene_stats(&s).versions, " versions are alive");
	draw(&reader);

	scene_release(&s, &reader.hold);
	scene_unpin(&s, &reader.pin);
	scene_reclaim(&s);
	println(scene_stats(&s).versions, " version is alive, ", scene_stats(&s).nodes, " nodes");

	scene_fini(&s);
	give_back();
	println("fed ", fed, " bytes, got back ", returned);
	return early != 0 && fed == returned ? 0 : 1;
}
