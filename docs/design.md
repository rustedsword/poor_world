# poor_world: design

How the library works, what it guarantees and what each operation costs. The API is described in
[poor_world.h](../include/poor_world.h). Functions are named here as for the `scene` of the example there.

## One header

The code is ordinary functions in the header that take the sizes and offsets of your types as arguments.
`poor_world_declare()` makes the tree type, writes those sizes down as constants (`scene_h_node_header` and so on),
declares one function per operation and defines the small ones inline: the lookups, everything a reader calls, the first
check of `scene_edit()`, and the part of `scene_diff_next()` that skips unchanged children in a list of fewer than 64.
`poor_world_define()` defines the rest in the file it is put in. Each is one line that calls the code in the header with
the constants of the tree type.

Every function in the header that takes such a constant is always inlined, so it ends up inside a function of one tree
type and the constant is never passed at run time. This holds for several tree types in one file too. The three large
pieces that several operations share are made once per tree type: copying the path to a node, freeing old versions, and
undoing a failed copy. Code that does not depend on your types (memory, the table of handles, matching the children in a
diff) is ordinary functions, so every file with a `poor_world_define()` has its own copy of it.

That file also decides how the tree is built. Its `assert()` checks the arguments. Every block is taken and given
back by code in that file, so under AddressSanitizer the memory of the tree is watched if that file is built with it.

## Versions

A version is a root pointer plus your fields. Everything reachable from a published root is never written again.

A transaction copies the current version into a free slot and gives it the next epoch. To change a node the writer
copies it, then its parent with the pointer to the copy, and so on up to the root. Each copy is marked with the epoch
of the transaction (`born`), so a node with that epoch is already the transaction's own and is changed in place. A
transaction therefore copies each node at most once, and a publish that changes one node copies one path.

A node is one block: 16 bytes that only the writer uses (the link for its lists and the size of the block), your
struct, and then the children as an array of pointers. A node gets a block that fits its children; when the array is
full the node moves to the next larger block. The block shrinks the next time the node is copied.

What the writer keeps per version (its place in the list of live versions, the nodes waiting to be freed) is in an
array next to the versions, not in them. So nothing a reader can reach is written after publish.

## Readers

All versions live in the `slots` of the tree, each padded to whole cache lines: the writer filling one never shares a
line with one a reader has. Which of them readers get is kept by a fan, `poor_fan.h` from poor_base. One 64-bit word
holds the slot of the published version in its low 16 bits and, above them, the number of holds taken since it was
published. Each slot has a count of holds (low 32 bits) and pins (high 32 bits).

- `scene_latest()` adds one to the word and reads the slot from the result, in one atomic operation. No publish can
  slip between reading the slot and being counted. If the hold already has the published slot it does nothing.
- `scene_publish()` swaps the word for the new slot and adds the holds the old word collected to the old slot's count.
- `scene_release()`, `scene_pin()` and `scene_unpin()` change the count of a slot.

A release may land before the publish that moves the holds from the word into the count, so a count can be below zero
for a moment. The published slot is therefore never treated as free, whatever its count says. A slot other than the
published one gets no new holds, so once its count is zero it stays zero.

Nothing here loops or waits. With `H` holds and `P` pins in use, `H + P + 2` slots are always enough: one for each of
them, the published version, and the transaction.

## Freeing nodes

Nodes have no reference counts. When a transaction replaces or destroys a node that older versions share, the node
goes on the list of the version that was current: that version is the newest one that can reach it.

The writer keeps the live versions in a list from newest to oldest. When a version is no longer held or pinned, each
node on its list was reachable from this version and maybe from older ones. A node born at or before the epoch of the
next older live version is still reachable from that one and moves to its list. Any other node is freed.

So a node is freed as soon as the last version that can reach it is let go, in whatever order readers let go, and a
reader that holds an old version keeps only the nodes of that version.

The writer learns that a version was let go by reading its count: one count for every older version that is still
alive, at each publish and in `scene_reclaim()`. The slot of a freed version goes on the writer's own list of free
slots, and a transaction takes the one that has been free the longest. One slot goes ahead of the others: that of a
version nobody held when the next one was published. Most likely no reader took it at all, and the writer still has it
in its cache. So `scene_begin()` reads nothing that readers write unless no slot is free, and what a publish costs
grows with the versions still in use, not with the number of slots.

Nodes made by a transaction and replaced or destroyed again inside it are freed at publish. An abort frees every node
the transaction made.

## Handles

A handle is an index into a table that only the writer uses: for each node its current copy, its parent and a hint of
its place among its siblings. This makes `scene_node()`, `scene_parent()` and the walk from a node to the root direct,
with no search from the root. The hint is checked and corrected when it is used, because inserting or removing a
sibling shifts the others.

The table is one block of 16 bytes per handle, so a lookup is one load. It doubles when it is full, which copies it.
Free entries are a list, and the ones at the end of the table are taken out of it at publish; when the table is at
most a quarter full it moves to a smaller block, and an empty tree has no table. The handles of destroyed nodes
become free at publish, not earlier, so an abort can bring the nodes back.

The smaller block needs memory, and at that publish there may be none: the destroyed nodes still belong to the old
version. The table then stays as it is, and the move is tried again at a later publish or `scene_reclaim()`, once a
slab or span has become unused.

A table larger than a slab is one span of several slabs, so the tree asks for that much at once. A block that is a
whole number of slabs needs one slab more, for the 64-byte header of its span. A table that would get such a block
gets the size below instead, when it grows and when it shrinks, which keeps a table of almost one slab inside one
slab. A table of four slabs or more is a whole number of slabs in every size and takes the extra slab anyway.

The tree asks for memory only when an operation fails, so a table that shrinks but still needs several slabs has to
find an unused span it fits in. It takes the first one, which can be much larger than it needs, and with none it
stays as large as it was.

An abort does not rebuild the table. Every node the transaction replaced or destroyed is on one list; for each of them
the entry is pointed back at it, and the entries of its children get their parent and place back. That undoes every
change the transaction could have made to the table.

## Memory

The library takes memory only from `scene_feed()`.

- Block sizes go up in steps of 64 bytes to 512, then in four steps for every doubling, so a block is at most a
  quarter larger than what is asked for.
- A slab serves blocks of one size and counts the ones in use. The first 64 bytes of the slab hold that state, and a
  block finds its slab by rounding its address down to the slab size.
- A block larger than a slab gets a span of its own, a whole number of slabs.
- A slab with no blocks in use is idle: `scene_take()` returns it, or the next slab that is needed takes it, for any
  size.
- New blocks come from the slab that most recently stopped being full, so slabs that are emptying are left to empty.

An operation copies the path one node at a time, from the node up, and takes the block for each copy when it gets
there. If a block is missing, the copies made so far are taken back and the operation fails with the tree as it was.
`scene_create()` makes room in the handle table and takes the block of the new node first, and gives the node back the
same way; a table that grew stays grown. This is why `scene_edit()` of an ancestor cannot fail after `scene_edit()` of
a node: the ancestors were copied by the same operation.

`scene_move()` to another parent copies two paths, the one of the old parent first. If the second cannot be copied,
the first is taken back as well.

`scene_wanted()` is the size of the slab or span that was missing. One operation can need several, of different block
sizes, so feed and retry in a loop.

Under AddressSanitizer every block that is not in use is poisoned, so a read of a freed node is reported even though
the memory is yours.

## What changed

A `struct poor_world_diff` compares the children of one node in two versions.

1. Children at the same place from the start, and then from the end, are paired while they are the same node (same
   `id`). Pairs that are the very same pointer are skipped: nothing below them changed.
2. If what is left is the same children with one of them moved to the other end, that one is `moved`.
3. Otherwise the old children are entered in a table by handle, the new ones are looked up in it, and the longest run
   of pairs that kept their order is found. The pairs outside it are `moved`. Children found on one side only are
   reported as added or removed.

Steps 2 and 3 are finished before the first result is returned, so the table is clean while you go into the children,
and one scratch serves the whole walk.

## Cost

`d` is the depth of a node and `c` the number of children of its parent.

| | |
|---|---|
| `scene_latest()`, `scene_release()`, `scene_pin()`, `scene_unpin()` | one atomic operation |
| `scene_node()`, `scene_parent()` | one load |
| `scene_edit()` | copies `d` nodes the first time in a transaction, nothing after that |
| `scene_create()`, `scene_destroy()`, `scene_move()` | `scene_edit()` of the parent, plus moving up to `c` pointers |
| `scene_publish()` | the nodes the transaction replaced or destroyed, also the ones it made if one of those is among them, and one load per older version still in use |
| `scene_abort()` | the nodes the transaction made, replaced or destroyed |
| `scene_diff_next()` over a node | the children that are not the same pointer; all `c` when the order changed |

Copying a node copies its children array, so a change under a parent with thousands of children copies thousands of
pointers.

What these take in nanoseconds, and what readers cost the writer, is measured in [bench](../bench/README.md).

## What it does not do

- One writer. A second thread that changes the tree needs your own lock around transactions.
- One transaction at a time. It is a state of the tree and not an object, and the tree does not say whether one is
  open. Calls in the wrong order are caught only by `assert()`.
- A tree stays where it is. It keeps pointers to its own arrays and its lists point back at it, so it cannot be copied
  or moved after `scene_init()`, and there is no static initializer.
- It does not pack memory by itself. A slab with one node in use stays, until that node is edited or destroyed.
- Handles cannot be packed at all: the handle table reaches up to the highest handle that is alive. After 10000
  nodes shrink to 100 scattered ones, the nodes fit in one slab again once they are edited, but the table stays as
  large as it was if one of the last handles is among them.
- A parent with very many children is copied whole when one of them changes.
- It does not walk the tree for you. Reading is plain pointers, and how deep you recurse is up to you.
