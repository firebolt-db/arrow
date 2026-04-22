<!---
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# Firebolt's fork of Apache Arrow

This repository is Firebolt's fork of [Apache Arrow](https://arrow.apache.org),
currently based on the `apache-arrow-20.0.0` tag. Only the `cpp/` subtree is
consumed downstream (by Firebolt's query engine); the other language bindings
and ecosystem files are left in place but are neither built nor maintained here.

The rest of this document describes **what this fork changes and why**. Most of
it would not make sense to upstream: these changes exist to make Arrow fit
Firebolt's execution model.

## Relationship to upstream

- Base: `apache-arrow-20.0.0`.
- Branching model: we track upstream release tags and carry Firebolt patches on
  top. When bumping to a new upstream version, rebase (or re-cherry-pick) this
  set of changes.
- Only `cpp/` is compiled and shipped. Changes outside of `cpp/` (e.g. CI
  configuration) exist only to keep this repository's own CI working.

## The big theme: corded buffers

Firebolt's external scan flow (S3, GCS, etc., driven by our Buffer Manager)
does **not** fetch whole files. It fetches them as a sequence of fixed-size
chunks (typically 2 MB) that are **non-contiguous in memory** and may arrive
out of order. Upstream Arrow's Parquet reader assumes a `RandomAccessFile`
that hands back contiguous byte ranges, so we had to teach it to read from a
list of slices without copying into a contiguous staging buffer first.

Everything with "corded" in its name exists to support that. It's a bit of a
misnomer since the cord data structure (aka rope) is something entirely
different.

### New primitives

- `arrow::CordedBuffer` (`cpp/src/arrow/corded_buffer.{h,cc}`): a non-owning,
  non-contiguous buffer: a `std::span<const Slice>` plus a current read
  position. Supports `Peek`, `Advance`, zero-copy reads within a single slice,
  and copying reads that span slices.
- `arrow::io::CordedInputStream` and `arrow::io::CordedRandomAccessFile`
  (`cpp/src/arrow/io/interfaces.h`, `cpp/src/arrow/io/memory.{h,cc}`): the
  streaming / random-access file interfaces adapted for corded data.
- Corded-aware decompression: `cpp/src/arrow/util/compression_corded.cc` and
  `compression_snappy.cc`, plus hooks in `compression.{h,cc}`.

### Parquet reader changes built on top of the primitives

- Parquet page reader reads directly from corded buffers, with a variant in
  place rather than fake `arrow::Buffer` wrappers around slices. CRC32 checks
  are preserved.
- Footer / metadata parsing works from corded buffers. The happy path (entire
  footer in one slice) avoids copying; only a multi-slice footer is copied
  into a temporary contiguous buffer.
- A `ReaderProperties::corded_buffer` knob turns on the corded code path.
- `FileReader::GetColumnReader(int row_group, int column, ...)` sets up a
  per-row-group column reader. Complements our chunked fetch model: we can
  materialize one column of one row group without the whole-file assumptions
  of the default reader.

The test strategy for all of this is to run the existing Parquet reader/writer
tests through corded buffers at several slice sizes (typically 10 bytes, 42
bytes, 10 KiB) so we exercise both "fits in one slice" and "spans many slices"
paths. See `cpp/src/parquet/test_corded_file.{h,cc}`.

## Fast Parquet metadata reader

Separate from the corded work, there is an in-progress C++ implementation of
selective Parquet footer parsing, similar to what is described in
https://arrow.apache.org/blog/2025/10/23/rust-parquet-metadata/

- `ReaderProperties::set_firebolt_columns_filter()` takes an
  `unordered_set<string_view>` of top-level column names. During thrift
  deserialization of `FileMetaData`, columns not in the set are **skipped** in
  the schema, in per-row-group `column_chunks`, and in `column_orders`,
  without allocating for the names/stats/etc. that we're about to throw away.
- `SchemaElement` gained a `firebolt_leaf_index` so that, even when elements
  are skipped, the remaining leaf columns still map correctly onto the
  leaf-indexed row-group metadata.
- Nested column filtering is not yet implemented, the filter is top-level
  only. A struct with 100 fields still parses all 100 even if you only want
  one. Noted for a later pass.
- Measured impact: ~8× on a "count(*) over many-column Parquet files"
  scenario (19 s -> 2.5 s in a customer workload benchmark).
- This is currently entirely name-based, so not suitable for Iceberg scans,
  which have to resolve columns based on field IDs rather than names.
- Future optimization: wire two sets, one for columns to scan and one for
  columns to filter on, and read statistics only for those columns to filter on.

### Vendored Thrift

To add the primitives the fast-metadata path needed (`readStringView`,
skipping strings without materializing them), we need to patch Thrift. We
previously used a public mirror we couldn't push to. Now:

- A trimmed-down Apache Thrift C++ source tree is copied into
  `third_party/thrift/` (started from Apache Thrift commit
  `2a93df80f27739ccabb5b885cb12a8dc7595ecdf`, then pruned aggressively).
- Thrift sources are compiled **into** the Parquet library. Arrow's
  `ThirdpartyToolchain.cmake` no longer treats Thrift as an external dependency;
  no `thrift::thrift` link target is produced.
- `thrift_internal.h` always uses `TConfiguration` to lift size limits, since we
  no longer have a generated `thrift/config.h`.
- Additional cleanup of vendored Thrift: dropped `TVirtualProtocol` (unused
  fallback class), dropped unused network transports, dropped the
  recursion-depth tracker.

## Memory management

- `FireboltAllocator` / `firebolt_memory_pool`
  (`cpp/src/arrow/memory_pool.{h,cc}`) is an `arrow::MemoryPool` that routes all
  allocations through `operator new` / `operator delete`. This is deliberate: it
  lets Firebolt's `MemoryTracker` (which hooks `new`/`delete`) see every Arrow
  allocation. jemalloc is still the underlying allocator in non-sanitizer
  builds, so `ReleaseUnused()` delegates to the jemalloc pool.
- The default "system" pool on Firebolt builds is switched to the Firebolt pool,
  so Arrow code that uses `default_memory_pool()` is automatically tracked.

## Concurrency / threading

Arrow is built with `ARROW_ENABLE_THREADS=false` as we don't want Arrow spawning
thread pools; our execution engine schedules its own work. But some of Arrow's
single-threaded code paths (notably `SerialExecutor`) rely on **static global
state** that assumes only one thread-at-a-time calls into Arrow at all. That's
not true for us: multiple Firebolt threads call into Arrow independently, and
TSAN caught the race.

- Added `ARROW_ENABLE_CONCURRENT_SERIAL_EXECUTOR` (on by default; see
  `cpp/cmake_modules/DefineOptions.cmake`, `config.h.cmake`). When enabled,
  the static-globals path in `SerialExecutor` and a few dependents is
  disabled in favor of code that is safe for concurrent callers.
- Removed `AfterForkState`, as upstream PR `apache/arrow#14594` already
  documented it as dead, and it broke our gtest death tests.

## Correctness fixes carried on the fork

- **Dictionary-encoded booleans** (`cpp/src/parquet/decoder.cc` +
  `encoding_test.cc`). Upstream refuses to read these. Snowflake produces them
  in Iceberg tables, so we need to be able to read them.
- **Unaligned buffers** (`cpp/src/arrow/ipc/reader.cc`). Flight / raw socket
  reads can hand us a `Splice()` that is not aligned to the type it
  contains. UBSAN complains; old compilers can SIGSEGV even on x86. If a buffer
  isn't 8-byte aligned, we copy it into a 64-byte-aligned buffer.
- **Overly-large IPC allocations** (`cpp/src/arrow/ipc/reader.cc`).  Added
  upfront allocation-size checks so the ASan/UBSan fuzzers stop tripping on
  maliciously crafted IPC frames.

## ORC adapter changes

- Timestamp resolution on import is **microseconds**, not nanoseconds
  (`cpp/src/arrow/adapters/orc/util.cc`, `adapter_test.cc`). Our engine's
  native timestamp type is microseconds; nanos widened unnecessarily and
  tripped range checks on valid values.
- Fix NULL-timestamp conversion: the previous code touched the value even
  when the row was null.

## Build / CI

None of this is functionally interesting; it exists to keep this repo
compilable and its CI green.

- `cpp/` requires C++20 (needed for `std::span`, used by `CordedBuffer`).
- LLVM/clang 18; sanitizer builds run on Ubuntu 24.04 (22.04's boost is too
  old for clang-18).
- CI is stripped to C++ and Linux only, no macOS, no Windows, no other language
  bindings, no examples. See `.github/workflows/`.
- A handful of C++20 deprecation warnings are silenced with `#pragma GCC
  diagnostic ignored` in places where the upstream fix hasn't landed.

## Navigating the fork

- `git log apache-arrow-20.0.0..HEAD -- cpp/` is the authoritative list of
  Firebolt's changes.
- The largest single change is the corded-buffer work.  Read
  `cpp/src/arrow/corded_buffer.h` first, then the Parquet-side integration in
  `cpp/src/parquet/file_reader.cc` and `column_reader.cc`.
- The fast-metadata work. Start at
  `ReaderProperties::set_firebolt_columns_filter` and follow the filter into
  `cpp/src/generated/parquet_types.tcc`.

## License

Apache Arrow is licensed under Apache 2.0 (see [LICENSE.txt](LICENSE.txt) and
[NOTICE.txt](NOTICE.txt)). All Firebolt modifications in this fork are also
distributed under the Apache 2.0 license.
