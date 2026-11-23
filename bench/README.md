# Benchmarks

Three programs that each print what one part of the tree costs. Nothing here is checked: the tests are in
[tests](../tests) and they do not measure.

| benchmark | measures |
|---|---|
| scene | a publish that changes one node, finding that change from the root, and `scene_node()` |
| memory | what a tree takes, what comes back when it shrinks, and what scattered survivors keep |
| readers | `scene_latest()` in up to 8 reader threads, and what the readers cost the writer |

## Running

    $ meson setup build-release/ -Dbuildtype=release -Db_ndebug=true -Dbench=true
    $ meson test -C build-release/ --benchmark -v

takes about 12 seconds. The programs take no arguments and can be run by hand: `build-release/bench/scene`, `memory`
and `readers`. Build them without assertions as above. The tests refuse to build that way, so tests and benchmarks need
a build directory each.

Every timed figure is the median of 5 runs, with the lowest and the highest in brackets.

## scene

A scene of 16, 256 and 2000 windows of 256 bytes, each with one child, either in groups of 60 or all under one parent.
Three kinds of change, 20000 times each on a random window:

- `commit` edits the child of a window, the deepest node.
- `move` edits a window.
- `raise` moves a window to the top of its parent.

`publish` is a whole transaction with one such change. `diff` is the walk from the root of the version before to the
root of the version after that finds the change, and `changes` how many it found on average. `lookup` is
`scene_node()` of a random handle. `memory` is how much the tree asked for by the end.

The clock is read around every publish and every diff. What reading it twice takes is measured at the start and
subtracted, which matters at these sizes: it is 41 ns here.

## memory

10200 nodes of 40 bytes in slabs of 64 KiB, as 200 groups of 50 under one root. It prints how much memory the tree
holds, after taking back everything `tree_take()` gives, at each step of two stories: the tree is filled and
emptied, and the tree is filled and all but one leaf in 100 are destroyed. Nothing is timed.

## readers

A tree of two nodes. Every thread has a core to itself: the writer the fastest one, the readers the next ones, by
`acpi_cppc/highest_perf`. SMT siblings stay unused. The readers call `scene_latest()` in a loop for 100 ms:

- with the writer doing nothing, so there is never a new version;
- the same, but with `scene_release()` after every call, so every call takes the version again;
- with the writer publishing as fast as it can. This also shows what a publish takes then, and how many of the
  versions a reader got to see.

A reader that takes a version writes the word that all readers and the writer share, and the count of that version.
These rows show what that costs when it happens all the time. A compositor does both once per frame.

## Numbers

On a Ryzen AI 9 HX 370 held at 2.0 GHz, with GCC 16.2. Its CPUs 0 to 3 are the fast cores and share one L3 cache; the
others are compact cores with another, so the fourth reader is the first one in another cache.

    reading the clock twice takes 41 ns, subtracted from publish and diff
    2000 windows, 60 per parent, commit: publish  140 ns (134..158), diff  138 ns (138..139), 1.00 changes
    2000 windows, 60 per parent, move  : publish  103 ns (101..105), diff  128 ns (128..130), 1.00 changes
    2000 windows, 60 per parent, raise : publish  126 ns (125..128), diff  186 ns (185..188), 0.98 changes
    2000 windows, 60 per parent, lookup:    5 ns (5..6), memory 2880 KiB
    2000 windows, 2000 per parent, commit: publish  256 ns (252..265), diff  795 ns (794..799), 1.00 changes
    2000 windows, 2000 per parent, move  : publish  222 ns (221..227), diff  790 ns (787..794), 1.00 changes
    2000 windows, 2000 per parent, raise : publish  860 ns (810..869), diff 2819 ns (2792..2827), 1.00 changes
    2000 windows, 2000 per parent, lookup:    5 ns (5..7), memory 2880 KiB

    40-byte nodes in slabs of 64 KiB: 200 groups of 50 under one root
    only the root                                   64 KiB
    all of them                                  1088 KiB, 102 bytes for each of 10200 nodes
    all but the root destroyed                    128 KiB
    after the next change to the root              64 KiB
    one leaf in 100 left                         1216 KiB
    after a change to every node that is left     384 KiB, 301 nodes with handles up to 10151

    the writer on CPU 0, readers on CPUs 3 2 1 4 5 6 7 8; 100 ms a run, median of 5 runs (lowest..highest)
             the writer does nothing                             the writer publishes as fast as it can
    readers  latest()                  latest() and release()    latest()                  publish()                 versions a reader saw
          0                                                                                35.0 ns (35.0..36.0)
          1  1.6 ns (1.6..1.6)         19.3 ns (19.3..19.4)      4.8 ns (4.7..5.2)         192.0 ns (181.0..194.0)   72.0 % (71.0..73.0)
          2  1.6 ns (1.6..1.6)         93.2 ns (83.3..95.5)      9.8 ns (9.5..10.2)        231.0 ns (231.0..232.0)   77.0 % (76.0..77.0)
          3  1.6 ns (1.6..1.7)         116.8 ns (116.7..128.0)   15.6 ns (14.9..16.0)      289.0 ns (286.0..290.0)   69.0 % (69.0..70.0)
          4  1.7 ns (1.7..1.8)         380.6 ns (371.2..394.4)   20.5 ns (19.9..21.6)      512.0 ns (509.0..533.0)   65.0 % (64.0..72.0)
          8  1.9 ns (1.6..1.9)         507.7 ns (396.9..543.6)   18.5 ns (15.3..18.8)      837.0 ns (690.0..847.0)   55.0 % (54.0..56.0)

What they show:

- A publish that changes one node is 0.10 to 0.25 µs, and finding the change 0.13 to 0.19 µs in groups of 60. Under
  one parent with 2000 children every publish copies 2000 pointers and every diff walks them: 0.8 µs to find a change,
  and a raise is 0.86 µs to publish and 2.8 µs to find.
- A node of 40 bytes takes 102 bytes, with its block, its handle and its place in the parent. Destroying the tree gives
  back all of it after the next change to the root. One leaf in 100 left keeps everything until what is left is
  edited; then the nodes pack, and of the 384 KiB that stay, 256 KiB are the handle table, which still reaches up to
  handle 10151.
- `scene_latest()` with nothing new is a load and a compare: between 1 and 2 ns with any number of readers.
- Taking a version is not free when readers do it together: 19 ns alone, about 90 ns with two readers and 400 to
  540 ns with eight.
- Readers that take every version slow the writer down: a publish is about 35 ns with no reader, about 190 ns with
  one and 690 to 850 ns with eight.
- That cost depends on the pace of the writer. Built by hand with a delay before each transaction, a writer that
  takes 48 ns per publish alone takes 160 ns with one reader, and one that takes 38 ns alone takes 190 ns: it
  publishes again before the reader has let go of the version before, and then the count of that version is in the
  reader's cache.

The reader figures also move with where the compiler puts the reader's loop. Built by hand with that loop shifted a few
bytes at a time, the same instructions took 1.1 to 2.0 ns for `scene_latest()` with nothing new.
