# Benchmarks

Three programs that each print what one part of the tree costs. Nothing here is checked: the tests are in
[tests](../tests) and they do not measure.

| benchmark | measures |
|---|---|
| scene | a publish that changes one node, finding that change from the root, and `poor_world_node()` |
| memory | what a tree takes, what comes back when it shrinks, and what scattered survivors keep |
| readers | `poor_world_latest()` in up to 8 reader threads, and what the readers cost the writer |

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
`poor_world_node()` of a random handle. `memory` is how much the tree asked for by the end.

The clock is read around every publish and every diff. What reading it twice takes is measured at the start and
subtracted, which matters at these sizes: it is 41 ns here.

## memory

10200 nodes of 40 bytes in slabs of 64 KiB, as 200 groups of 50 under one root. It prints how much memory the tree
holds, after taking back everything `poor_world_take()` gives, at each step of two stories: the tree is filled and
emptied, and the tree is filled and all but one leaf in 100 are destroyed. Nothing is timed.

## readers

A tree of two nodes. Every thread has a core to itself: the writer the fastest one, the readers the next ones, by
`acpi_cppc/highest_perf`. SMT siblings stay unused. The readers call `poor_world_latest()` in a loop for 100 ms:

- with the writer doing nothing, so there is never a new version;
- the same, but with `poor_world_release()` after every call, so every call takes the version again;
- with the writer publishing as fast as it can. This also shows what a publish takes then, and how many of the
  versions a reader got to see.

A reader that takes a version writes the word that all readers and the writer share, and the count of that version.
These rows show what that costs when it happens all the time. A compositor does both once per frame.

## Numbers

On a Ryzen AI 9 HX 370 held at 2.0 GHz, with GCC 16.2. Its CPUs 0 to 3 are the fast cores and share one L3 cache; the
others are compact cores with another, so the fourth reader is the first one in another cache.

    reading the clock twice takes 41 ns, subtracted from publish and diff
    2000 windows, 60 per parent, commit: publish  224 ns (214..243), diff  199 ns (198..202), 1.00 changes
    2000 windows, 60 per parent, move  : publish  185 ns (182..187), diff  181 ns (181..183), 1.00 changes
    2000 windows, 60 per parent, raise : publish  202 ns (200..204), diff  284 ns (282..286), 0.98 changes
    2000 windows, 60 per parent, lookup:    5 ns (5..7), memory 2944 KiB
    2000 windows, 2000 per parent, commit: publish  333 ns (329..367), diff 1816 ns (1812..1831), 1.00 changes
    2000 windows, 2000 per parent, move  : publish  291 ns (289..297), diff 1801 ns (1798..1860), 1.00 changes
    2000 windows, 2000 per parent, raise : publish  913 ns (858..971), diff 5487 ns (5456..5529), 1.00 changes
    2000 windows, 2000 per parent, lookup:    5 ns (5..9), memory 2816 KiB

    40-byte nodes in slabs of 64 KiB: 200 groups of 50 under one root
    only the root                                  128 KiB
    all of them                                  1088 KiB, 96 bytes for each of 10200 nodes
    all but the root destroyed                    192 KiB
    after the next change to the root             128 KiB
    one leaf in 100 left                         1216 KiB
    after a change to every node that is left     384 KiB, of which 80 pages of handles for 301 nodes hold 160 KiB

    the writer on CPU 0, readers on CPUs 3 2 1 4 5 6 7 8; 100 ms a run, median of 5 runs (lowest..highest)
             the writer does nothing                             the writer publishes as fast as it can
    readers  latest()                  latest() and release()    latest()                  publish()                 versions a reader saw
          0                                                                                67.0 ns (67.0..67.0)
          1  2.0 ns (2.0..2.0)         19.3 ns (19.2..19.3)      5.2 ns (4.8..5.3)         186.0 ns (171.0..192.0)   83.0 % (71.0..87.0)
          2  2.0 ns (2.0..2.0)         100.4 ns (99.0..104.8)    12.3 ns (11.0..12.4)      259.0 ns (259.0..265.0)   79.0 % (78.0..80.0)
          3  2.0 ns (2.0..2.0)         121.8 ns (116.4..127.5)   17.3 ns (16.2..17.5)      318.0 ns (316.0..324.0)   74.0 % (73.0..76.0)
          4  2.1 ns (2.1..2.2)         390.2 ns (372.5..396.5)   31.9 ns (30.1..32.2)      574.0 ns (571.0..579.0)   68.0 % (67.0..68.0)
          8  2.5 ns (2.0..2.5)         509.3 ns (452.4..513.1)   30.1 ns (24.5..31.4)      875.0 ns (732.0..885.0)   58.0 % (58.0..58.0)

What they show:

- A publish that changes one node is 0.2 to 0.35 µs, and finding the change about the same in groups of 60. Under one
  parent with 2000 children every publish copies 2000 pointers and every diff walks them: 1.8 µs to find a change, and
  a raise is 0.9 µs to publish and 5.5 µs to find.
- A node of 40 bytes takes 96 bytes, with its block, its handle and its place in the parent. Destroying the tree gives
  back all of it after the next change to the root. One leaf in 100 left keeps everything until what is left is
  edited; then the nodes pack, and of the 384 KiB that stay, 160 KiB are the pages of their handles.
- `poor_world_latest()` with nothing new is a load and a compare: about 2 ns with any number of readers.
- Taking a version is not free when readers do it together: 19 ns alone, about 100 ns with two readers and 450 to
  510 ns with eight.
- Readers that take every version slow the writer down: a publish is about 70 ns with no reader, about 190 ns with
  one and 730 to 890 ns with eight.

The reader figures also move with where the compiler puts the reader's loop. Built by hand with that loop shifted a few
bytes at a time, the same instructions took 1.1 to 2.0 ns for `poor_world_latest()` with nothing new. With eight
readers and a writer that publishes, `poor_world_latest()` took 18 to 29 ns and a publish 760 to 870 ns.
