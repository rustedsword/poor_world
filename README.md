# poor_world

A small C library for a tree that one thread changes while other threads read it.

- The writer changes the tree in a transaction and publishes it as a new version.
- A reader takes the newest version and reads it for as long as it likes. What it holds is never written again and
  never goes away under it. Nothing waits and nothing retries: taking a version is one atomic operation.
- Versions share every node that did not change, so a publish copies only the path from the changed node to the root.
- Readers can keep older versions and ask what changed between two of them.

It was written for the scene of a Wayland compositor (the main thread changes windows, each output thread renders the
version it took), but it knows nothing about windows: the nodes are your structs.

The library never allocates memory. You give it memory in slabs, it tells you when it wants more, and it gives back
the slabs it no longer uses.

```c
struct window {
	struct poor_world_node node;
	int x, y;
};

struct frame {
	struct poor_world_version version;
	uint64_t focus;
};

/* 'scene' keeps up to 8 versions alive: enough for 3 readers that hold one version and pin one more */
poor_world_declare(scene, struct window, node, struct frame, version, 8);

static scene s;

/* the writer */
struct window *window;

poor_world_init(&s, 65536)->focus = 0;                  /* slabs of 64 KiB */
poor_world_begin(&s)->focus = 1;
while (!(window = poor_world_create(&s, POOR_WORLD_NO_HANDLE, 0)))      /* no parent: this is the root */
	poor_world_feed(&s, aligned_alloc(65536, poor_world_wanted(&s)), poor_world_wanted(&s));
window->x = window->y = 0;
poor_world_publish(&s);

/* a reader, in another thread */
struct poor_world_hold hold = POOR_WORLD_HOLD_INIT;
const struct frame *frame = poor_world_latest(&s, &hold);
const struct window *root = poor_world_root(&s, frame);

for (uint32_t i = 0; i < root->node.child_count; i++)
	draw(poor_world_child(&s, root, i));
```

The API is described in [include/poor_world.h](include/poor_world.h), above the line that says where the
implementation starts. [docs/design.md](docs/design.md) explains how it works and what each operation costs.
[examples/scene.c](examples/scene.c) is a complete program: a writer that opens, raises and closes windows, and a
reader that prints what changed between the frames it saw.

## Memory

`poor_world_create()`, `poor_world_destroy()`, `poor_world_edit()` and `poor_world_move()` fail when they need memory
the tree does not have. A failed operation changes nothing: feed what `poor_world_wanted()` says and call it again, or
go on without it. Nothing else fails.

`poor_world_take()` returns memory that is not used, whenever you ask. Memory comes back slab by slab, and a slab is
unused only when everything in it is gone:

- Nodes that live long keep their slabs. Editing a node moves it, which is a way to pack the nodes that are left.
- A node keeps the block it has until it is copied again, so a parent that lost most of its children gets a smaller
  block with the next change to it.
- Handles do not pack. They come in pages of 128, and a page stays while one of its handles is alive.

A node with very many children needs one block larger than a slab; `poor_world_wanted()` then asks for more than a
slab, still a multiple of it.

## Keeping something summed up over a subtree

Once `poor_world_edit()` or `poor_world_create()` returned a node, `poor_world_edit()` of any of its ancestors needs no
memory until the transaction ends. So change the node, then walk up with `poor_world_parent()` and
`poor_world_edit()`: bounds, counts and the like stay right in every version, and the walk cannot fail halfway.

## Building

Needs a C23 compiler, Meson 1.4 and [poor_base](https://github.com/rustedsword/poor_base), either installed or from
the submodule.

    $ git submodule update --init
    $ meson setup build/ -Dtests=true -Dexamples=true
    $ ninja -C build/
    $ meson test -C build/
    $ build/examples/scene

[bench](bench/README.md) has the benchmarks and what they measured.

The tests:

- `world-test`: every operation, versions freed in any order, 10000 children under one node, a tree 200000 deep.
- `memory-test`: memory goes back to where it started after the tree shrinks.
- `fault-test`: every allocation of every operation fails once, and the tree must be untouched each time.
- `model-test`: random transactions against a plain tree, with readers, aborts, and every diff checked.
- `threads-test`: readers in other threads while the writer publishes. Run it with `-Db_sanitize=thread`.
