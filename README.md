# Nasdaq ITCH 5.0 Feed Handler + Order Book Reconstruction

This is an incremental C++20 learning project for replaying Nasdaq TotalView-ITCH 5.0 data
and reconstructing aggregated L2 order books. The repository currently contains the correct
in-memory model, an ITCH 5.0 decoder for the required messages, a buffered BinaryFILE replay
path, an experimental memory-mapped path, deterministic tests, command-line statistics,
and measured profiling/benchmark notes.

Both synthetic and official-session measurements are documented and deliberately kept
separate. The results show why: mmap helped substantially on the small fixture, lost in one
cache-sensitive 11.25 GB run, and then won repeatably once the same file was resident in memory.

## What ITCH is and why build this

Nasdaq TotalView-ITCH is a one-way binary market-data protocol. It publishes events such as
adds, executions, cancels, deletes, and replaces rather than periodically sending a complete
order book. A consumer reconstructs the current book by decoding every relevant event in order
and maintaining its own state.

That makes a feed handler a compact systems project: correctness depends on byte layouts,
endianness, integer widths, event ordering, and coordinated data-structure updates. Performance
then depends on allocation, cache locality, I/O strategy, and container behavior. This repository
keeps those concerns visible and measures changes one at a time instead of hiding them behind a
framework.

## Architecture

```text
ITCH BinaryFILE (buffered or mmap) or handcrafted operations
                         |
                         v
             framing and ITCH decoder
                         |
                         v
             OrderId -> individual Order
             symbol, side, price, remaining quantity
                         |
                         v
              Symbol -> LimitOrderBook
                         |
             +-----------+-----------+
             |                       |
             v                       v
    bid Price -> total qty   ask Price -> total qty
       highest first             lowest first
```

The two representations answer different questions:

- The order map answers, "Which symbol, side, price, and remaining quantity belong to
  order 101?" This is necessary because execute, cancel, delete, and replace events refer
  to an order by ID.
- The price-level maps answer, "How many shares are displayed at this price?" This is the
  aggregated L2 view.

Every valid event updates both representations. For example, executing 40 shares from a
100-share order changes its remaining quantity to 60 and subtracts 40 from its price
level. A full execution removes both the order and the now-empty level.

The correct baseline stored an owning symbol string in every order. After profiling showed
repeated symbol hashing, the internal representation was changed to store a numeric `SymbolId`
in hot order state and keep each symbol string once beside its book. Public queries still return
the same owning `Order` values; this is a measured layout optimization, not a protocol shortcut.

## Why prices are integers

`Price` is a `std::uint32_t` measured in ten-thousandths of a dollar:

```text
2,001,000 / 10,000 = $200.1000
```

This matches the scale used by ITCH price fields and avoids floating-point rounding.
For example, a binary floating-point value cannot necessarily represent `$200.10`
exactly, while the integer `2,001,000` is exact.

Individual order quantities use `std::uint32_t`. Aggregated price-level quantities use
the wider `std::uint64_t`, because many individual orders can contribute to one level.

## State transitions

- `ADD`: reject a zero quantity or duplicate ID; otherwise insert the order and increase
  its price level.
- `EXECUTE`: locate the order by ID and reduce both its remaining quantity and its
  original displayed price level.
- `CANCEL`: has the same state effect as an execution in this toy model, but remains a
  separate public operation because the real feed distinguishes their meaning.
- `DELETE`: remove all remaining shares from the level and erase the order.
- `REPLACE`: validate the entire event, remove the old order and level contribution, then
  insert the new ID, price, and quantity while retaining the old symbol and side.

Impossible external events return a `BookError`. They are validated before mutation, so
a rejected event does not partially change the book. `InconsistentState` is reserved for
detecting an internal programming error where the order map and price-level maps disagree.

The book deliberately permits a negative spread (a crossed book). Spread is an observed
value, not a correctness test.

## Supported ITCH messages

The decoder follows Nasdaq's official
[TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf).
Historical record framing follows Nasdaq's
[BinaryFILE specification](https://nasdaqtrader.com/content/technicalSupport/specifications/dataproducts/binaryfile.pdf).

| Type | Meaning | Replay effect |
| --- | --- | --- |
| `A` | Add Order | Add individual order and level quantity |
| `F` | Add Order with MPID | Same book effect as `A`; attribution is decoded |
| `E` | Order Executed | Reduce the referenced order |
| `C` | Executed With Price | Reduce at the order's displayed price |
| `X` | Order Cancel | Reduce the referenced order |
| `D` | Order Delete | Remove all remaining shares |
| `U` | Order Replace | Replace ID, total quantity, and price |
| `S` | System Event | Retain the latest session event |
| `R` | Stock Directory | Retain reference data by daily stock-locate code |
| `H` | Stock Trading Action | Retain the latest state by symbol |

Other message types are counted and skipped because they are not needed to reconstruct the
displayed book. A recognized message with a non-specification length is rejected.

See [ITCH 5.0 decoding notes](docs/itch-5.0-decoding.md) for the byte layouts and design
reasoning.

## Build and run

Requirements:

- CMake 3.20 or newer
- A C++20 compiler

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/toy_order_book
```

The walkthrough adds six AAPL orders, including multiple orders at the same price, and
then applies partial execution, cancellation, deletion, replacement, and full execution.
After every operation it prints individual orders, aggregated levels, the top of book,
the spread, and the invariant-check result.

Replay an **uncompressed** historical BinaryFILE with:

```bash
./build/itch_replay path/to/file.NASDAQ_ITCH50
./build/itch_replay --mmap path/to/file.NASDAQ_ITCH50
```

The command prints message counts, order-event counts, active orders, reconstructed symbols,
elapsed time, throughput, and average nanoseconds per message. It fails with the exact record
number for truncated framing, malformed supported messages, and invalid order transitions.
Nasdaq's public 2019 sample ends with the typed `S/C` End of Messages record but no zero-length
BinaryFILE terminator. That exact ending is accepted with a warning; other missing terminators
remain errors.

Do not pass a `.gz` file directly. Decompression and feed-handler performance are separate:

```bash
gzip -dk 01302019.NASDAQ_ITCH50.gz
./build/itch_replay 01302019.NASDAQ_ITCH50
```

Nasdaq publishes large sample sessions in its
[public ITCH sample directory](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/). Keep downloaded
and decompressed market data outside the repository.

Build `Release` and compare parser-only with end-to-end replay throughput using:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/itch_benchmark path/to/file.NASDAQ_ITCH50 5
```

The benchmark measures buffered decode, buffered full replay, mmap decode, and mmap full
replay separately. It validates equivalent message counts/checksums and checks final book
invariants outside the timed replay interval.

## Measured results

On the deterministic 1,000,130-message fixture, five-run medians were:

| Input | Decode-only | Full replay |
| --- | ---: | ---: |
| Buffered, original alpha loop | 14.437 M msg/s | 8.880 M msg/s |
| Mmap, original alpha loop | 50.573 M msg/s | 16.806 M msg/s |
| Buffered, direct alpha copy | 14.728 M msg/s | 8.983 M msg/s |
| Mmap, direct alpha copy | 57.549 M msg/s | 17.570 M msg/s |
| Buffered, alpha copy + symbol IDs | 14.802 M msg/s | 10.087 M msg/s |
| Mmap, alpha copy + symbol IDs | 57.052 M msg/s | 21.759 M msg/s |

On the official 368,366,634-message, 11.25 GB raw session, one exploratory run was:

| Input | Decode-only | Full replay |
| --- | ---: | ---: |
| Buffered | 12.013 M msg/s | 1.677 M msg/s |
| Mmap | 10.937 M msg/s | 1.526 M msg/s |

Environment: Apple M1 Pro (10 cores), 32 GB RAM, macOS 26.5.2, Apple Clang 15.0.0,
C++20 Release build. The real-data run used a fixed buffered-then-mmap order, so it is not a
statistically stable claim. Exact timings, dataset construction, profiler evidence, and
cache caveats are in [the engineering log](docs/benchmarking.md).

The official session's measured peak was 1,742,866 simultaneous orders. Reserving exactly that
many order-map slots produced 1.660 M msg/s versus a paired 1.667 M msg/s baseline (-0.4%), so
reservation remains optional. The negative result is retained in the engineering log.

Symbol interning improved synthetic full replay by 12.3% buffered and 23.8% with mmap while
leaving decode-only throughput essentially flat. One official-session buffered run completed at
1.676 M msg/s, within the earlier run-to-run range, so no real-session gain is claimed yet.

After the file was resident in the OS page cache, post-layout full replays produced:

| Input | Full replay | Wall time | ns/message |
| --- | ---: | ---: | ---: |
| Buffered | 1.872 M msg/s | 196.827 s | 534.324 |
| Mmap, run 1 | 2.213 M msg/s | 166.424 s | 451.788 |
| Mmap, run 2 | 2.211 M msg/s | 166.572 s | 452.190 |

The two mmap times differ by less than 0.1%. Mmap was 18.3% faster than the adjacent buffered
run in this warm-cache setup. These numbers do not replace the earlier negative result; together
they show that input/cache state is part of the benchmark definition.

## Profiling findings

The real buffered baseline's sampled replay-loop time was approximately 67.2% order-book work,
18.5% framing/read, 6.8% decode, and 6.5% destruction/free. An mmap profile reduced cursor
framing to under 1% of sampled on-CPU stacks, leaving about 82.7% book work and 15.6% decode.
After symbol interning, repeated symbol hashes disappeared from non-add paths; price-tree work,
order-map allocation, and buffered framing remained visible. Full commands, sample counts, and
the limits of stack sampling for page-fault analysis are in the engineering log.

## Correctness strategy

The dependency-free test executable is registered with CTest. Tests cover:

- adds, aggregation, partial and full executions/cancels, deletes, and replaces;
- symbol isolation;
- best bid, best ask, empty-side behavior, and signed spreads;
- rejection of missing IDs, duplicates, zero quantities, and excessive reductions;
- no mutation after a rejected event;
- independent recomputation of every aggregate from active orders;
- deterministic replay of the same sequence.

`MarketState::check_invariants()` independently rebuilds expected price levels from all
active orders and compares them with the maintained books. This is useful as a teaching
and testing tool, but it is intentionally not called on every production update because
doing so would turn each event into a full scan.

## Current limitations

- Only the message types listed above are decoded into typed fields.
- Symbols are owning strings at the API boundary and interned numeric IDs in hot order state.
- Only standard containers are used.
- The baseline buffered replay allocates a payload vector for each framed message.
- The mmap experiment is POSIX-specific (Linux/macOS) and is not universally faster.
- Real-session cold/cache-sensitive numbers have only one iteration; warm mmap has two adjacent
  observations but is not a randomized multi-run study.
- Order-map reservation is available for experiments but was not beneficial on the measured
  official-session run.
- Alpha fields use a measured direct owning copy; non-owning alpha views are not implemented.
- The order and price-level containers remain standard-library containers.
- A one-lookup price-level reduction experiment was inconclusive and reverted.

Further container or price-level work needs lower-noise, interleaved A/B measurement. The current
implementation intentionally retains `std::unordered_map` order lookup and `std::map` levels as a
clear, defensible reference point.

## Future work

- Add interleaved A/B benchmark binaries and CPU affinity/thermal controls where the OS permits.
- Collect Linux `perf stat` counters and a flamegraph on the same official raw session.
- Test an alternative order hash table only after preserving the standard-library baseline.
- Explore price-level representations if tree-node allocation remains a measured bottleneck.
- Consider a separate non-owning decoder API for parser-only workloads, with lifetimes explicit in
  its types; keep the owning decoder as the safe default.
- Add optional top-N book output or per-symbol validation fixtures without putting output work in
  the timed replay loop.

Benchmark commands, measured environment details, synthetic baseline results, and profiling
evidence are recorded in [the engineering log](docs/benchmarking.md). Synthetic results are
explicitly separated from real Nasdaq session results.
