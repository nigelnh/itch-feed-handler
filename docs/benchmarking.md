# Benchmark methodology and engineering log

This document separates measured facts from expectations. Results are only entered after a
command has run on the named machine and dataset.

## Methodology

Build an optimized binary:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
```

Run all four benchmark paths against the same uncompressed BinaryFILE:

```bash
./build-release/itch_benchmark path/to/file 5
```

Focused modes used for one-change experiments are:

```bash
./build-release/itch_benchmark path/to/file 5 --decode-only
./build-release/itch_benchmark path/to/file 1 1742866 --replay-only
```

The latter compares ordinary buffered replay with a buffered replay that reserves the given
order-map capacity. The one-time `reserve()` call is inside the timed interval.

Each iteration opens or maps the file afresh and measures:

1. **Buffered decode-only:** buffered file read, BinaryFILE framing, exact-length validation, and
   typed ITCH decoding. It does not apply messages to market state.
2. **Buffered full replay:** the same read/framing/decoding work plus order lookup and L2 updates.
3. **Mmap decode-only:** map the raw file, frame it with non-owning byte spans, and decode it.
4. **Mmap full replay:** the mapped framing/decoding path plus the same order-book updates.

The tool reports every iteration and uses the median elapsed time as its main result. The
fastest result is retained as additional context. Checksums keep the measured work observable;
the harness requires matching message counts and checksums between equivalent buffered and
mapped passes. Final invariant
validation happens outside the timed full-replay interval.

Opening the file or creating the mapping happens before timing. Actual page faults caused by
touching file data remain inside the timed region, as they should for end-to-end input
comparison. The current harness executes all buffered decode passes, all buffered replay
passes, all mmap decode passes, and then all mmap replay passes. This fixed order makes cache
state a confounder for a file comparable to physical memory, so large-file single runs are
labeled exploratory rather than definitive.

The first pass may be slower because file pages and executable code are cold. Runs should be
performed with minimal competing workload. For multi-gigabyte real sessions, one iteration
may be appropriate during development; final published numbers should use multiple runs when
time permits and state whether the OS page cache was warm.

Compressed input is never benchmarked as parser input. Treat these as separate jobs:

```text
.gz -> gzip decompression -> raw BinaryFILE
raw BinaryFILE -> feed-handler benchmark
```

## Environment

Recorded September 15, 2026:

| Item | Value |
| --- | --- |
| Machine | MacBook Pro, MacBookPro18,3 |
| CPU | Apple M1 Pro, 10 cores (8 performance + 2 efficiency) |
| Memory | 32 GB |
| OS | macOS 26.5.2, build 25F84, arm64 |
| Compiler | Apple Clang 15.0.0 (`clang-1500.3.9.4`) |
| CMake | 4.4.3 |
| Language/build | C++20, `Release` |

## Baseline results: deterministic synthetic fixture

Fixture command:

```bash
./build-release/itch_generate_synthetic /private/tmp/itch-baseline-1m.bin 200000
```

The resulting uncompressed BinaryFILE was 34,604,382 bytes and contained 1,000,130
messages. It used 64 synthetic symbols. Each cycle contained an add, partial execution,
partial cancel, replace, and full execution-with-price. All orders were closed by the end.

This distribution is deliberately deterministic and useful for regression measurements,
but it is **not representative of a real trading day**. In particular, 20% of its repeating
messages are replaces.

The first five-run buffered baseline, before the mmap implementation, produced:

| Version | Decode-only msg/s | Full replay msg/s | Decode ns/msg | Replay ns/msg |
| --- | ---: | ---: | ---: | ---: |
| Correct baseline | 14.366 million | 8.963 million | 69.607 | 111.564 |

These are medians. The fastest results were 14.475 million decode messages/s and 9.004
million full-replay messages/s.

## Baseline profile: deterministic synthetic fixture

macOS Instruments was unavailable because the machine has Command Line Tools rather than
the full Xcode installation. The built-in `sample` profiler was used instead:

```bash
sample itch_replay 5 1 -wait -mayDie -file /private/tmp/itch-replay-sample.txt
./build-release/itch_replay /private/tmp/itch-profile-50m.bin
```

The profiled fixture contained 50,000,130 messages (1,730,004,382 bytes), and the profiler
captured 4,109 one-millisecond stack samples. The run completed at 8.127 million messages/s.

Approximate inclusive sample groups from the call graph:

| Area | Samples | Approximate share |
| --- | ---: | ---: |
| Buffered framing/read path | 1,623 | 39.5% |
| Apply/order-book path | 1,518 | 36.9% |
| Typed decode path | 370 | 9.0% |
| Frame/message destruction and frees | 393 | 9.6% |

The groups overlap internally but are separate children of the replay loop in the sampled
call graph. Important top-of-stack evidence included file reads, per-frame `operator new`
and frees, symbol-book hash lookup, order reductions, and tree-node insertion/removal.

Interpretation: the baseline performs two formatted stream reads and allocates a new payload
vector for every message. That is a measured cost worth testing with memory mapping. The
profile also shows meaningful state-update cost, but the synthetic message distribution
overweights replace and reduction operations; container changes should wait for a real-feed
profile.

## Baseline correctness and profile: official Nasdaq session

The official `01302019.NASDAQ_ITCH50.gz` sample was downloaded from Nasdaq's public archive,
verified with `gzip -t`, and decompressed before replay. The compressed file was
4,764,426,091 bytes; the raw BinaryFILE was 11,245,883,092 bytes.

The correct buffered replay processed all 368,366,634 records with no decode or order-book
error. It reconstructed 8,695 symbols and ended with zero active orders. Relevant counts were:

| Event | Count |
| --- | ---: |
| Unsupported messages skipped | 5,230,894 |
| System events | 6 |
| Stock directories | 8,714 |
| Trading actions | 8,805 |
| Adds (`A` + `F`) | 164,696,353 |
| Executions (`E` + `C`) | 8,255,881 |
| Cancels | 4,669,874 |
| Deletes | 158,273,361 |
| Replaces | 27,222,746 |

The public sample ends immediately after the typed `S/C` End of Messages event and does not
contain BinaryFILE's zero-length terminator. The replay accepts physical EOF only in that
narrow case and emits a warning; EOF after any other message is still a framing error.

The buffered Release replay used for the real profile completed in 221.813 seconds at 1.661
million messages/s (602.153 ns/message). macOS `sample` captured 4,052 one-millisecond stacks
during its first five seconds:

| Replay-loop area | Approximate share |
| --- | ---: |
| Apply/order-book operations | 67.2% |
| Buffered framing and reads | 18.5% |
| Typed decoding | 6.8% |
| Per-message destruction/frees | 6.5% |
| Other replay-loop instructions | 1.0% |

The strongest top-of-stack signals were price-level insertion, order deletion/addition,
symbol-book hashing and lookup, allocation/free, file reads, and order-map rehashing. This
real feed keeps far more simultaneous state than the synthetic fixture, so the standard
containers dominate more of the run. The profile still gives a concrete reason to test
removing buffered framing allocations, but it does not predict that mmap will necessarily
win on a multi-gigabyte file.

## Optimization experiment table

The mmap experiment changes only input ownership and framing. `MappedFile` owns the mapping;
`MappedFrameCursor` returns a `std::span` into that mapping, so it does not allocate or copy a
payload for each record. Those spans must not outlive the mapping. Typed messages still own
their strings, and the book representation and containers are unchanged.

The synthetic comparison below is a new five-iteration run made after the mmap path was
implemented. The two rows therefore come from the same invocation and environment:

| Version | Decode-only msg/s | Full replay msg/s | Change vs baseline |
| --- | ---: | ---: | ---: |
| Buffered baseline | 14.437 million | 8.880 million | — |
| Memory-mapped input | 50.573 million | 16.806 million | decode +250.3%; replay +89.3% |
| Direct owning alpha copy (buffered) | 14.728 million | 8.983 million | decode +2.0%; replay +1.2% |
| Mmap + direct owning alpha copy | 57.549 million | 17.570 million | vs mmap: decode +13.8%; replay +4.5% |
| Zero-copy alpha fields | Not implemented | Not implemented | Not measured |

Medians were 69.266 ns/message buffered versus 19.773 ns/message mapped for decode-only,
and 112.607 versus 59.503 ns/message for full replay. The first buffered-decode iteration was
slower than its warmed iterations, which is why the median rather than the best run is the
primary number.

The official Nasdaq comparison is kept separate because its behavior was different. This was
one exploratory iteration in the fixed order described above:

| Version | Decode-only msg/s | Full replay msg/s | Change vs baseline |
| --- | ---: | ---: | ---: |
| Buffered baseline | 12.013 million | 1.677 million | — |
| Memory-mapped input | 10.937 million | 1.526 million | decode -9.0%; replay -9.0% |

The corresponding times were 83.243 ns/message and 596.415 ns/message for buffered input,
versus 91.436 and 655.254 ns/message for mapped input. The negative result is retained. A
plausible explanation is that the 34.6 MB fixture was cached and magnified per-frame allocation
savings, while the 11.25 GB session made demand paging and page-cache state significant. That
is an inference, not a demonstrated cause. Multiple counterbalanced large-file iterations or
an mmap-specific profile are needed before drawing a general conclusion.

An mmap-specific five-second profile was subsequently captured from `itch_replay --mmap`.
Of 4,154 one-millisecond on-CPU stacks, approximately 82.7% were below book application,
15.6% below typed decoding, and 0.9% below `MappedFrameCursor::next`; the remainder was loop
overhead. This confirms that mapped framing is cheap in CPU terms and shifts attention to the
book and decoder. It does **not** explain mmap's worse wall time: sampling on-CPU stacks is not
a reliable accounting of time blocked on file-backed page faults. A system-level I/O trace or
counterbalanced runs would be needed to validate that hypothesis.

## Optimization experiment: reserve the order map

The baseline profile caught order-map rehashing during early growth. Rehashing allocates a
larger bucket array and redistributes every existing key. Reserving enough buckets once could
avoid those growth events, but an oversized reserve also allocates and initializes memory that
may not be useful.

The replay now records peak simultaneous orders. On the official session the observed peak was
1,742,866, compared with 164,696,353 cumulative adds. Capacity must be based on the first number,
not the second. A focused one-iteration comparison timed the startup reserve and both complete
buffered replays:

| Version | Full replay msg/s | ns/message | Wall time | Change |
| --- | ---: | ---: | ---: | ---: |
| Buffered baseline | 1.667 million | 599.993 | 221.017 s | — |
| Reserve 1,742,866 orders | 1.660 million | 602.309 | 221.871 s | -0.4% |

The event checksum, 368,366,634-message count, peak count, and final invariants agreed. This
optimization did not help the full run and remains optional rather than becoming the default.
The likely reason is that rehashing is concentrated during growth and represents too little of
the whole session to repay the up-front bucket allocation, but a single run cannot establish
that cause.

A sanity experiment on the synthetic fixture also reserved exactly its observed peak of one.
The five-run medians were 8.813 million messages/s without reserve and 8.751 million with it
(-0.7%), small enough to treat as neutral/noisy.

## Optimization experiment: direct owning alpha copy

The first decoder built alpha strings by reserving capacity, appending each byte with
`push_back`, and then popping trailing spaces. `read_alpha` appeared in both profiles. The
replacement scans backward over padding and constructs the final `std::string` with one bounded
copy. This is not zero-copy: the typed message still owns its string. That is deliberate because
it preserves the decoder API's lifetime guarantee even when the buffered frame disappears on
the next loop iteration.

On the synthetic fixture, five-run medians before and after this one code change were:

| Path | Before msg/s | After msg/s | Change |
| --- | ---: | ---: | ---: |
| Buffered decode | 14.437 million | 14.728 million | +2.0% |
| Buffered full replay | 8.880 million | 8.983 million | +1.2% |
| Mmap decode | 50.573 million | 57.549 million | +13.8% |
| Mmap full replay | 16.806 million | 17.570 million | +4.5% |

The official-session parser-only one-run values moved from 12.013 to 13.732 million messages/s
for buffered input and from 10.937 to 12.207 million for mmap. These before/after values came
from separate single-run invocations and remain exploratory. A subsequent buffered full replay
was 1.639 million messages/s, below the prior 1.661–1.700 range; the expected small end-to-end
effect was lost in run-to-run variance. The mmap full replay was 1.467 million messages/s. The
defensible conclusion is that direct construction improved the repeatable synthetic benchmark
and the real parser-only observation, but did not demonstrate a real full-replay gain.

## Optimization experiment: intern symbols in hot order state

The real profiles repeatedly showed symbol hashing and symbol-to-book lookup. The baseline
stored an owning `std::string` in every live order, then used that string to find the symbol's
book on every execute, cancel, delete, and replace.

The optimized representation assigns each new symbol a small numeric `SymbolId`. A vector entry
owns the symbol text and its `LimitOrderBook`; each live order stores the ID, side, price, and
remaining quantity. A later event finds its order by reference number and indexes the book vector
directly. This makes hot order values smaller and removes symbol hashing from non-add events.
The external API is unchanged: queries and snapshots still produce owning symbol strings.
Numeric IDs also avoid storing pointers that could be invalidated or accidentally copied.

Tests force 256 new symbols after an early AAPL order, then execute and replace that early order.
All Debug, Release, and UndefinedBehaviorSanitizer tests passed.

On the synthetic fixture, the first five-run before/after comparison was:

| Path | Before msg/s | Symbol IDs msg/s | Change |
| --- | ---: | ---: | ---: |
| Buffered decode | 14.728 million | 14.802 million | +0.5% |
| Buffered full replay | 8.983 million | 10.087 million | +12.3% |
| Mmap decode | 57.549 million | 57.052 million | -0.9% |
| Mmap full replay | 17.570 million | 21.759 million | +23.8% |

The nearly flat decode numbers are a useful control: symbol interning only changes market state,
so parser throughput should not move. Full replay improved substantially on this fixture.

The official buffered session completed at 1.676 million messages/s (596.713 ns/message,
219.809 seconds), with all earlier event counts, the 1,742,866-order peak, and the final invariant
check unchanged. That wall time is effectively the same as the earlier 219.699-second buffered
baseline and faster than the immediately preceding 224.713-second run. Given the observed
run-to-run spread, no real-session full-replay improvement is claimed from this one measurement.

The post-interning buffered profile captured 3,688 stacks. Approximate replay-loop shares were
70.2% book application, 18.7% buffered framing/read, 3.3% typed decoding, 6.1% destruction/free,
and 1.7% other. Symbol hashing no longer appeared on non-add paths; the remaining symbol map lookup
is performed while interning add messages. Price-level operations, order-map nodes, allocation,
and buffered framing remain visible.

Once the 11.25 GB file was resident in the OS page cache, post-layout full replay was measured in
an mmap, buffered, mmap sequence. The first mmap run took 166.424 seconds at 2.213 million
messages/s; buffered took 196.827 seconds at 1.872 million messages/s; the repeated mmap run took
166.572 seconds at 2.211 million messages/s. The mmap repeats differ by less than 0.1%, and mmap
was 18.3% faster than the adjacent buffered run. These are explicitly warm-cache results and do
not invalidate the earlier negative mmap observation under different cache state.

## Reverted experiment: combine level validation and mutation

The book currently performs one ordered-map search to validate that aggregate quantity can be
reduced, then a second search to mutate the level. A one-search implementation looked attractive,
but a seven-run measurement did not validate it: medians were 9.307 million buffered and 20.227
million mmap messages/s, compared with 10.087 and 21.759 million in the preceding run.

An A-B-A check restored the original two-search code and produced 9.047 million buffered and
20.183 million mmap messages/s. Parser-only rates had also fallen across these invocations, showing
that machine-state drift was large enough to confound this small change. With no reproducible win,
the one-search implementation was reverted and the clearer validate-then-mutate structure was
kept. Future micro-optimizations need interleaved A/B binaries or lower-noise process controls.
