# ITCH 5.0 decoding notes

These notes record the protocol facts used by this implementation. The source of truth is
Nasdaq's official [TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf).
Historical files use Nasdaq's official [BinaryFILE format](https://nasdaqtrader.com/content/technicalSupport/specifications/dataproducts/binaryfile.pdf).

## From file bytes to a typed message

```text
raw historical file
        |
        |  2-byte big-endian payload length (BinaryFILE envelope)
        v
one ITCH payload
        |
        |  byte 0 selects the message layout
        v
exact-length check
        |
        |  explicit offset reads; no pointer casting
        v
typed C++ message
        |
        v
ReplayEngine -> MarketState -> aggregated books
```

Framing and ITCH are separate layers. The two-byte length is not part of an ITCH message;
it is the historical file's envelope. The BinaryFILE specification says a zero length marks
a complete session and its absence indicates an incomplete file. Nasdaq's public 2019 sample
was observed to omit that marker while ending with the ITCH `S/C` End of Messages record.
The implementation accepts this narrow case with a warning because the ITCH specification
states that `S/C` is always the final message. EOF without either marker remains an error.

## Common ITCH header

Every supported ITCH payload begins with the same 11 bytes:

| Field | Offset | Bytes | C++ type |
| --- | ---: | ---: | --- |
| Message type | 0 | 1 | `char` |
| Stock locate | 1 | 2 | `std::uint16_t` |
| Tracking number | 3 | 2 | `std::uint16_t` |
| Timestamp | 5 | 6 | `std::uint64_t` |

The timestamp occupies only six bytes on the wire but is stored in an eight-byte integer in
memory. It is nanoseconds since midnight, not a Unix timestamp.

## Required message layouts

The total size below is the ITCH payload size and excludes BinaryFILE's two-byte length.

| Type | Message | Total bytes | Fields after the common header |
| --- | --- | ---: | --- |
| `S` | System Event | 12 | event code: 1 |
| `R` | Stock Directory | 39 | stock and reference-data fields |
| `H` | Stock Trading Action | 25 | stock: 8, state: 1, reserved: 1, reason: 4 |
| `A` | Add Order | 36 | order ID: 8, side: 1, shares: 4, stock: 8, price: 4 |
| `F` | Add Order with MPID | 40 | `A` fields, attribution: 4 |
| `E` | Order Executed | 31 | order ID: 8, shares: 4, match: 8 |
| `C` | Executed With Price | 36 | `E` fields, printable: 1, execution price: 4 |
| `X` | Order Cancel | 23 | order ID: 8, canceled shares: 4 |
| `D` | Order Delete | 19 | order ID: 8 |
| `U` | Order Replace | 35 | old ID: 8, new ID: 8, shares: 4, price: 4 |

All integer fields are unsigned, big-endian values. Alpha fields are ASCII, left justified,
and padded on the right with spaces. The decoder scans backward over only that trailing padding,
then copies the meaningful bytes into an owning `std::string` in one operation. The result does
not refer to the input buffer and remains valid after the frame or mmap view goes away.

## Why the decoder reads bytes explicitly

It may be tempting to cast the input pointer to a packed C++ struct. That creates several
problems:

- the host machine may be little-endian while the feed is big-endian;
- some fields, especially the six-byte timestamp, are not normal C++ integer sizes;
- an address inside the byte buffer may not satisfy the alignment required by a struct;
- struct padding is chosen by the compiler unless deliberately controlled;
- a pointer cast can violate C++ object-lifetime and aliasing rules.

The baseline loops over each integer's bytes and shifts them into an ordinary unsigned
integer. This is easy to audit. If profiling later shows this code matters, an optimized
decoder can be compared against this known-correct implementation.

## Book semantics that are easy to get wrong

`C` includes an execution price because the trade happened away from the order's displayed
price. It still reduces aggregate depth at the original displayed price stored with the
order. Moving the order to the execution price would corrupt the book.

`U` omits symbol and side because Nasdaq states those attributes cannot change during a
replace. The replay engine therefore retrieves them from the original order, removes that
order, and creates the new reference number with the new total quantity and price.

Unsupported ITCH message types are counted and skipped. A real session contains many types
that do not affect displayed order-book reconstruction. In contrast, a supported type with
the wrong length is rejected because decoding it with the wrong layout would be unsafe.
