# efsw-fuzz

A file system fuzzer for efsw. It makes random changes to real watched directories, then
applies the events efsw delivered, in the order they were delivered, to a model of the watched
trees. After each round the model must equal the real trees. If it doesn't, the events could not
have rebuilt the tree. That catches events in an order a listener can't apply, paths that never
existed, and missing events.

It found most of the ordering bugs of `Options::ReportCrossDirectoryMoves` on the polling
backends, and some in inotify, that targeted tests had missed.

POSIX only: Linux, macOS, the BSDs. The source, `efsw-fuzz.cpp`, describes the checks in
detail.

## Building

With premake, as part of the workspace:

```sh
premake5 gmake
cd make/linux        # or make/macosx, make/bsd
make efsw-fuzz       # bin/efsw-fuzz-debug
make config=release_x86_64 efsw-fuzz   # bin/efsw-fuzz-release
```

`premake4` works the same way. With CMake, the `efsw-fuzz` target is built when efsw is the
top-level project, unless `-DBUILD_FUZZER=OFF` or `-DBUILD_STATIC_LIBS=OFF`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target efsw-fuzz   # build/efsw-fuzz
```

Or by hand, against a built static library:

```sh
g++ -std=c++11 -O1 -g -Iinclude src/fuzz/efsw-fuzz.cpp lib/libefsw-static-release.a -lpthread -o efsw-fuzz
```

To check memory and threading errors along with event order, build the library and the fuzzer
with `-fsanitize=address,undefined` or `-fsanitize=thread`.

## Running

```sh
efsw-fuzz [seed] [rounds] [options]
```

| Option | Meaning |
| --- | --- |
| `seed` | The random seed. It fixes the operations. Default 1. |
| `rounds` | Rounds to run. Default 20. A seed stops after 3 failing rounds. |
| `--generic` | Use the generic (polling) backend instead of the native one. |
| `--ms N` | The generic backend's polling interval, in ms. Default 100. |
| `--maxops N` | At most N operations per round. Default 8. |
| `--optall` | All four registrations opt in to `ReportCrossDirectoryMoves`; by default `w3` doesn't. |
| `--nested` | Also register the root, not recursively, so the `w*` registrations are nested in it. |
| `--strict` | Don't accept the polling backends' conventions; see below. |
| `--verbose` | Also print the rounds that pass. |

It works in a new directory under `/tmp` with four recursive registrations, `w0` to `w3`, and an
unwatched `out` directory that entries can leave to and come back from. Each round makes 1 to
`--maxops` operations: create a file, make a directory, rename (into any directory, sometimes to
a new name), remove a tree, append to a file, or sleep part of a polling interval, so that the
operations fall into different passes.

It exits with 0 if no round failed, 1 if one did, and 2 on a usage or setup error. The last
line is always:

```
seed 51: 0 failing rounds of 25
```

### Layouts

These five layouts cover the cases the fuzzer was used for. Each runs 25 rounds per seed:

| Layout | Options | Exercises |
| --- | --- | --- |
| Busiest | `--generic --optall --maxops 12` | Cross-watch moves between all registrations, many per pass |
| Generic | `--generic` | A registration without the option among ones with it |
| Nested generic | `--generic --nested` | Nested registrations on the polling backend |
| Native | none | The platform's backend: inotify, FSEvents or kqueue |
| Nested native | `--nested` | Nested registrations on the platform's backend |

### Campaigns

`campaign.sh` runs a range of seeds in parallel, keeps one log per seed, and lists the seeds
that failed or didn't finish:

```sh
src/fuzz/campaign.sh bin/efsw-fuzz-release /tmp/fuzz-busiest 1 400 -j 8 -- --generic --optall --maxops 12
```

`-j` sets how many seeds run at once (default 8), and `-r` the rounds per seed (default 25).
The fuzzer spends most of its time waiting for efsw, so a campaign uses little CPU, and more
jobs than cores is fine. On Linux, 400 seeds of the busiest layout took about 11 minutes with
`-j 8`, while another campaign of the same size ran alongside.

## Reading a failure

A failing round prints the operations, every event delivered, and the errors:

```
=== seed 280 round 3 FAILED
-- ops:
  rename /tmp/efsw-fuzz-pVayub/w2/n5/n10 -> /tmp/efsw-fuzz-pVayub/out/n10 (0)
  ...
-- events:
  [w2] Delete /tmp/efsw-fuzz-pVayub/w2/n5/n10
  ...
  [w2] Moved /tmp/efsw-fuzz-pVayub/w2/n5/n27/n44 from /tmp/efsw-fuzz-pVayub/w2/n5/n10/d/f
-- errors:
  Moved from missing /tmp/efsw-fuzz-pVayub/w2/n5/n10/d/f -> /tmp/efsw-fuzz-pVayub/w2/n5/n27/n44
```

The `(0)` after a rename is its return value. The errors:

| Error | Meaning |
| --- | --- |
| `Add of existing P` | An Add for a path the listener already has: a Delete is missing, or came late. |
| `Delete of missing P` | A Delete for a path the listener doesn't have. |
| `Moved from missing O -> P` | A move from a path the listener doesn't have. Usually events out of order: the source's directory was reported deleted or renamed first. |
| `Moved onto existing O -> P` | A move onto a path the listener has. |
| `Moved across non-opted registrations` | A Moved between registrations that didn't both opt in. |
| `event outside its registration` | An event whose path is outside the registration that reported it. |
| `missing from replay: P` | A real entry no event reported. |
| `extra in replay: P` | An entry the events left that is gone. |

After a failing round, the model is reset to the real trees and the next round starts from
there. A round that got none of its events, or only part of them, followed by a round whose
events start with the missing ones, means events arrived after the fuzzer stopped waiting for
them. That happens under heavy load: it is late delivery, not an ordering bug.

Two conventions of the polling backends are accepted unless `--strict` is given:
- a directory's Delete may come before the Deletes of its contents;
- a directory reported as an Add, or moved in from where nothing reported its contents, takes
  its contents with it without events.

## Seeds and timing

A seed fixes the operations, not their timing. Whether a seed fails can depend on how its
operations fall into polling passes or inotify reads, and so on the machine's load. Keep this in
mind when reading results:

- **Rerun a failing seed alone**, several times. A seed that fails alone is easy to trace. One
  that only fails under load is still a finding until it's explained.
- **To compare two builds**, run their campaigns side by side, at the same time, on the same
  seeds, so both get the same load. Counts from runs at different times or loads aren't
  comparable.
- **A fix is confirmed by a deterministic test**, not by a seed passing: once a failure is
  understood, reproduce it in the unit tests and check the test fails without the fix. For the
  polling backends, `src/unit_tests/file_action_batch.cpp` drives `FileActionBatch` with fake
  watches and exact scan orders. For inotify, `src/unit_tests/inotify_moved_directories.cpp`
  holds the event thread to batch events deterministically.

## Notes

- The fuzzer leaves nothing behind when it finishes. A run that is killed leaves its directory
  in `/tmp/efsw-fuzz-*`.
- The cleaned-up fuzzer draws the same random numbers in the same order as the original review
  tool, so seed numbers in earlier reports still give the same operations.
- Campaigns so far ran on Linux, with inotify and the generic backend. On macOS and FreeBSD the
  fuzzer builds, and the generic backend (`--generic`) passes the same seeds. The native
  backends there:
  - **FSEvents** doesn't pass: it may report more actions than occurred, as the main README
    says, such as an `Add` for a file that was only written, or a `Delete` for a file created
    and removed between two events. The replay reports those as errors.
  - **kqueue** scans like the generic backend, every `--ms`, and the fuzzer waits for it as
    such on the BSDs. On FreeBSD, seeds 1 to 200 pass.
