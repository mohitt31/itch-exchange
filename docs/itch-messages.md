# ITCH 5.0 message inventory

Generated from `tools/itch50_fields.json`, which `tools/extract_spec.py` pulls
out of the Nasdaq TotalView-ITCH 5.0 specification PDF
(sha256 `45e0531d1b4b3beb886e9618b2ab824a5aa9bda3a99c0dff03509306e68aacc3`).

**23 message types, not the commonly cited 22.** The April 2023 revision added
`'O'`, Direct Listing with Capital Raise. All 23 are implemented and all 23
have `static_assert`s on size and on every field offset.

Counts are from the complete 30 January 2019 session, 368,366,634 messages.
"Book" marks the seven messages that mutate a limit order book; the rest are
reference data or trade reports. Applying a trade report would double count
against the executions that already moved the book.

| Type | Struct | Bytes | Fields | Book | Count | Share |
|---|---|---|---|---|---|---|
| `S` | `SystemEvent` | 12 | 5 |  | 6 | 0.0000% |
| `R` | `StockDirectory` | 39 | 18 |  | 8,714 | 0.0024% |
| `H` | `StockTradingAction` | 25 | 8 |  | 8,805 | 0.0024% |
| `Y` | `RegShoRestriction` | 20 | 6 |  | 8,821 | 0.0024% |
| `L` | `MarketParticipantPosition` | 26 | 9 |  | 193,769 | 0.0526% |
| `V` | `MwcbDeclineLevel` | 35 | 7 |  | 1 | 0.0000% |
| `W` | `MwcbStatus` | 12 | 5 |  | 0 | 0.0000% |
| `K` | `IpoQuotingPeriodUpdate` | 28 | 8 |  | 0 | 0.0000% |
| `J` | `LuldAuctionCollar` | 35 | 9 |  | 62 | 0.0000% |
| `h` | `OperationalHalt` | 21 | 7 |  | 0 | 0.0000% |
| `A` | `AddOrder` | 36 | 9 | yes | 162,970,455 | 44.2414% |
| `F` | `AddOrderWithMpid` | 40 | 10 | yes | 1,725,898 | 0.4685% |
| `E` | `OrderExecuted` | 31 | 7 | yes | 8,096,995 | 2.1981% |
| `C` | `OrderExecutedWithPrice` | 36 | 9 | yes | 158,886 | 0.0431% |
| `X` | `OrderCancel` | 23 | 6 | yes | 4,669,874 | 1.2677% |
| `D` | `OrderDelete` | 19 | 5 | yes | 158,273,361 | 42.9663% |
| `U` | `OrderReplace` | 35 | 8 | yes | 27,222,746 | 7.3901% |
| `P` | `TradeNonCross` | 44 | 10 |  | 1,326,184 | 0.3600% |
| `Q` | `CrossTrade` | 40 | 9 |  | 17,430 | 0.0047% |
| `B` | `BrokenTrade` | 19 | 5 |  | 116 | 0.0000% |
| `I` | `Noii` | 50 | 13 |  | 3,684,511 | 1.0002% |
| `N` | `RetailPriceImprovement` | 20 | 6 |  | 0 | 0.0000% |
| `O` | `DirectListingWithCapitalRaise` | 48 | 12 |  | 0 | 0.0000% |

Five types defined by the specification did not occur in this session:
`K` `N` `O` `W` `h`. They are implemented and tested against hand-built
buffers regardless.

## The common header

Every one of the 23 messages begins with the same four fields at the same
offsets. The parser reads the stock locate code and the timestamp before it
knows which message it is holding, so this is load-bearing rather than a
coincidence, and `test_wire.cpp` asserts it for all 23 structs.

| Offset | Length | Field |
|---|---|---|
| 0 | 1 | Message Type |
| 1 | 2 | Stock Locate |
| 3 | 2 | Tracking Number |
| 5 | 6 | Timestamp (48-bit, nanoseconds since midnight) |

One inconsistency in the specification itself: the Reg SHO table calls the
second field **Locate Code** where every other table calls the same field at
the same offset **Stock Locate**. It is normalised in `tools/gen_wire.py`,
and the uniform-header test is what found it.

## Field detail

Offsets and lengths below come out of the PDF mechanically. A handful of
names are truncated by the document's column width and are completed from the
same spec by an explicit override table in `tools/gen_wire.py`, so every such
completion is visible.

### `S` SystemEvent (12 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 1 | Alpha | Event Code |

### `R` StockDirectory (39 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Market Category |
| 20 | 1 | Alpha |  |
| 21 | 4 | Integer | Round Lot Size |
| 25 | 1 | Alpha | Round Lots Only |
| 26 | 1 | Alpha | Issue Classification |
| 27 | 2 | Alpha | Issue Sub-Type |
| 29 | 1 | Alpha | Authenticity |
| 30 | 1 | Alpha | Short Sale |
| 31 | 1 | Alpha | IPO Flag |
| 32 | 1 | Alpha | LULDReference |
| 33 | 1 | Alpha | ETP Flag |
| 34 | 4 | Integer | ETP Leverage |
| 38 | 1 | Alpha | Inverse Indicator |

### `H` StockTradingAction (25 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Trading State |
| 20 | 1 | Alpha | Reserved |
| 21 | 4 | Alpha | Reason |

### `Y` RegShoRestriction (20 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Locate Code |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Reg SHO Action |

### `L` MarketParticipantPosition (26 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 4 | Alpha | MPID |
| 15 | 8 | Alpha | Stock |
| 23 | 1 | Alpha | Primary Market |
| 24 | 1 | Alpha | Market Maker |
| 25 | 1 | Alpha | Market |

### `V` MwcbDeclineLevel (35 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Price | Level 1 |
| 19 | 8 | Price | Level 2 |
| 27 | 8 | Price | Level 3 |

### `W` MwcbStatus (12 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 1 | Alpha | Breached Level |

### `K` IpoQuotingPeriodUpdate (28 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 4 | Integer | IPO Quotation |
| 23 | 1 | Alpha | IPO Quotation |
| 24 | 4 | Price | IPO Price |

### `J` LuldAuctionCollar (35 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 4 | Price | Auction Collar Reference Price |
| 23 | 4 | Price | Upper Auction Collar Price |
| 27 | 4 | Price | Lower Auction Collar Price |
| 31 | 4 | Integer | Auction Collar |

### `h` OperationalHalt (21 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Market Code |
| 20 | 1 | Alpha | Operational |

### `A` AddOrder (36 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference |
| 19 | 1 | Alpha | Buy/Sell Indicator |
| 20 | 4 | Integer | Shares |
| 24 | 8 | Alpha | Stock |
| 32 | 4 | Price | Price |

### `F` AddOrderWithMpid (40 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference |
| 19 | 1 | Alpha | Buy/Sell Indicator |
| 20 | 4 | Integer | Shares |
| 24 | 8 | Alpha | Stock |
| 32 | 4 | Price | Price |
| 36 | 4 | Alpha | Attribution |

### `E` OrderExecuted (31 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference |
| 19 | 4 | Integer | Executed Shares |
| 23 | 8 | Integer | Match Number |

### `C` OrderExecutedWithPrice (36 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference |
| 19 | 4 | Integer | Executed Shares |
| 23 | 8 | Integer | Match Number |
| 31 | 1 | Alpha | Printable |
| 32 | 4 | Price | Execution Price |

### `X` OrderCancel (23 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference Number |
| 19 | 4 | Integer | Cancelled Shares |

### `D` OrderDelete (19 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference Number |

### `U` OrderReplace (35 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Original Order |
| 19 | 8 | Integer | New Order |
| 27 | 4 | Integer | Shares |
| 31 | 4 | Price | Price |

### `P` TradeNonCross (44 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Order Reference |
| 19 | 1 | Alpha | Buy/Sell Indicator |
| 20 | 4 | Integer | Shares |
| 24 | 8 | Alpha | Stock |
| 32 | 4 | Price | Price |
| 36 | 8 | Integer | Match Number |

### `Q` CrossTrade (40 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Shares |
| 19 | 8 | Alpha | Stock |
| 27 | 4 | Price | Cross Price |
| 31 | 8 | Integer | Match Number |
| 39 | 1 | Alpha | Cross Type |

### `B` BrokenTrade (19 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Match Number |

### `I` Noii (50 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Integer | Paired Shares |
| 19 | 8 | Integer | Imbalance Shares |
| 27 | 1 | Alpha | Imbalance Direction |
| 28 | 8 | Alpha | Stock |
| 36 | 4 | Price | Far Price |
| 40 | 4 | Price | Near Price |
| 44 | 4 | Price | Current Reference |
| 48 | 1 | Alpha | Cross Type |
| 49 | 1 | Alpha | Price Variation |

### `N` RetailPriceImprovement (20 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Interest Flag |

### `O` DirectListingWithCapitalRaise (48 bytes)

| Offset | Len | Type | Field |
|---|---|---|---|
| 0 | 1 | Alpha | Message Type |
| 1 | 2 | Integer | Stock Locate |
| 3 | 2 | Integer | Tracking Number |
| 5 | 6 | Integer | Timestamp |
| 11 | 8 | Alpha | Stock |
| 19 | 1 | Alpha | Open Eligibility Status |
| 20 | 4 | Price | Minimum Allowable Price |
| 24 | 4 | Price | Maximum Allowable Price |
| 28 | 4 | Price | Near Execution Price |
| 32 | 8 | Integer | Near Execution Time |
| 40 | 4 | Price | Lower Price Range Collar |
| 44 | 4 | Price | Upper Price Range Collar |

