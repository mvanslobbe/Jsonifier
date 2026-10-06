# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader tuned for small documents;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,080 of 1,344 converged single-document results against simdjson On Demand, tied 103 and lost 161. On reverse-order reads it won 299 of 317. On Windows/MSVC it won 290 of 298 single-document results, tied 4 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests, Jsonifier won 77 of 90 converged results, tied 4 and lost 9, and won every streaming test that converged on Windows/MSVC.

---

## 1. Background: why On Demand is hard to beat

simdjson's On Demand API does not build a DOM. Stage 1 produces a tape of structural positions, and stage 2 is a cursor over that tape driven by the caller. A value that is never requested is skipped over by structural depth, without being parsed. Reading in document order costs roughly one pass over the tape.

The price is a strictly forward-only API. Each value can be consumed once, and reading an object's fields out of order means rescanning the object for every key.

## 2. Design goals

1. **Keep On Demand's shape.** Existing On Demand code should port to `jsonifier::generic` mostly mechanically.
2. **Allow values to be read again.** A value handle remembers where it sits on the tape and can re-seek to it, instead of being invalidated.
3. **Make in-order reads free.** The common case must cost no more than a cursor increment and a key compare.
4. **Keep out-of-order reads close to in-order speed.** Their cost should stay close to linear in the size of the object, no matter what order the caller reads keys in.

## 3. Architecture

### 3.1 The tape

Stage 1 is Jsonifier's existing structural indexer (see *Two Stages, On Demand*, §4). It writes one `uint32_t` offset per structural character, and appends a sentinel entry `tape[count] = length`. Because of the sentinel, stage 2 never has to bounds-check the end of the tape on its hot paths.

Small documents are routed to a dedicated POD reader. When the input is shorter than one SIMD step, the fixed cost of setting up the full-width reader outweighs the actual work, and that reader's step sizes are chosen to fit the input.

### 3.2 A shared forward cursor

`document_state` holds one cursor into the tape and one depth counter. Every object, array and value handle refers to that shared state; a handle does not own its own iterator.

When a handle is used, it *settles* first:

- If the shared state is deeper than the handle, the state finishes the open containers up to the handle's depth.
- If the depth still doesn't match, the handle rewinds the cursor to its own opening position.

In-order reads never trigger either branch. Out-of-order reads pay only for the distance they actually move. On the hot paths, the cursor and depth are copied into locals, advanced there, and written back to the shared state once, so the compiler can keep them in registers.

### 3.3 Lazy errors

Accessors return `[[nodiscard]] error_code` and write their result to an out-parameter; they don't wrap results in a `result<T>`. A structural error is stored in the handle and passed along to every handle derived from it. `document::atEnd()` checks that the whole document was consumed. Errors therefore cost one well-predicted branch per access.

### 3.4 Finding fields

`findFieldUnordered(key)` runs in three tiers:

1. **The expected field.** In-order reads almost always find the requested key right at the cursor, possibly after skipping one comma. This tier needs one tape read and one key compare.
2. **Scan and wrap.** If the key isn't at the cursor, scan forward to the end of the object, then wrap around to the start of the object and scan up to where the cursor began.
3. **Escape-aware retry.** Only if both scans miss, rescan the object comparing keys with escape sequences decoded. Escaped keys are rare, so the common path never pays for decoding.

Once an object has shown that it is being read out of order, tier 2 is replaced by a probe of the field index described in §5.

## 4. Zero-copy strings

`getString(std::string_view&)` returns a view straight into the input buffer whenever the string contains no escapes. It finds where the string ends with SIMD stop masks (a mask marking where a quote, backslash or control character appears):

- The first register is handled outside the loop ("peeled"), so short strings never enter the loop at all.
- The leftover bytes at the end are handled by a lookup table instead of a byte-by-byte loop.

Non-ASCII bytes go through UTF-8 validation on the view path. The `std::string` getter is split differently: pure-ASCII strings take the fast path, and everything else goes through the one-pass unescape-and-copy scanner. This keeps validation work off the path that already has to copy the bytes.

Strings that do contain escapes are decoded into an arena. Each string's tape offset maps to a fixed slot in the arena, so decoding the same value twice yields the same view. Earlier designs let the arena wrap around, which could overwrite views the caller still held; the fixed slots remove that failure.

**A GCC pitfall.** With `-funroll-loops`, GCC unrolls an un-peeled SIMD scan loop into a long prologue that runs even for short strings, which are most of the strings in the suite. The fix is to peel the first iteration by hand and use the table-driven tail described above.

## 5. The field index: making out-of-order reads cheap

Reading in reverse order is the worst case for tier 2 in §3.4. Each lookup scans past every key that comes after the target, so reading all *n* keys of an object in reverse costs O(n²).

### 5.1 The map

`field_index_map` is an insert-only, SwissTable-style hash map built specifically for this job:

- **Control bytes** come in 16-byte groups. An empty slot holds `-128`. An occupied slot holds `H2 = hash & 0x7F`, so every occupied control byte is non-negative.
- **Each entry** stores `{hash, object tape index, key tape index}`. It doesn't copy the key text; the key is compared against the tape directly.
- **The hash** mixes the key bytes with the object's tape index. The same key name in two different objects therefore hashes to different entries, and one map can serve the whole document. Short keys go through a single `mix64` mixing step; keys longer than 16 bytes are hashed a block at a time, murmur-style.
- **Probing** compares a whole control group against `H2` in one step, through the library's own SIMD wrappers (`gatherValuesU`, `opCmpEqRaw`, `opBitMaskRaw`), which lower to:
  - x86: `_mm_cmpeq_epi8` + `_mm_movemask_epi8`, one bit per slot;
  - NEON: `vceqq_u8` + a `vshrn`-narrowed nibble mask, four bits per slot, since NEON has no movemask instruction;
  - SVE2 (128-bit vectors): `svcmpeq_u8`, packed into the same four-bits-per-slot nibble mask;
  - any other target: the library's scalar SIMD emulation.
- **Reset** between documents clears the control bytes with `memset`, and only if the map was actually used for the previous document.

### 5.2 The lesson: whole-document indexing lost

The first version recorded every key the scanner skipped past, in every object. On Marine IK Reverse that made the parser **2.3x slower** than having no index at all, because Marine IK consists mostly of small objects with 3–6 fields each. For objects that small, re-scanning a few keys is cheaper than hashing, inserting and probing them.

### 5.3 Waiting until an object earns an index

The first fix was a counter on each object handle:

- The handle counts how many keys it has scanned past.
- Until that count reaches a threshold *T*, lookups use the plain scan-and-wrap path.
- After that, later scans record every key they pass into the map, and later lookups probe the map first.

Small objects never touch the map; large objects get O(1) lookups after one pass over their keys. The first A/B of that counter, run with *T* = 16:

| Test | Compiler | No index | Whole-doc index | Index after 16 skipped keys | vs simdjson |
|---|---|---|---|---|---|
| Marine IK Reverse (min) | GCC 16 | 7.49 ms | 17.1 ms | 6.24 ms | +79% |
| Marine IK Reverse (pretty) | GCC 16 | 8.77 ms | 18.0 ms | 7.42 ms | +61% |
| Marine IK Reverse (min) | Clang 24 | 8.03 ms | 19.2 ms | 6.48 ms | +42% |
| Marine IK Reverse (pretty) | Clang 24 | 8.86 ms | 20.0 ms | 7.25 ms | +40% |

*The "no index" and "index after 16 skipped keys" columns come from one A/B run and the "whole-doc index" column from another; each value is the median of 3 rounds, built with the CI flags. The "vs simdjson" column compares the "index after 16 skipped keys" build against simdjson in that same run.*

### 5.4 Choosing the threshold: a parameter sweep

*T* trades two costs. Set too high, objects with many fields rescan for longer before the map activates. Set too low, medium-sized objects pay for recording scans (hashing and inserting every key they pass) that a short rescan would have beaten. We swept *T* ∈ {2, 4, 8, 16} on four builds, running the full suite at each value, with the counter-only trigger above. The table shows Jsonifier's reverse-order margin over simdjson on the two tests that are sensitive to the threshold:

| Build | Test | *T* = 16 | *T* = 8 | *T* = 4 | *T* = 2 |
|---|---|---|---|---|---|
| Linux / GCC | Marine IK Reverse (min) | +74% | +82% | +11% | +39% |
| Linux / GCC | Random Reverse (min) | +29% | +37% | +42% | **−26%** |
| Linux / Clang | Marine IK Reverse (min) | +39% | +51% | **−14%** | +13% |
| Linux / Clang | Random Reverse (min) | +34% | +39% | +55% | **−25%** |
| macOS / Clang | Marine IK Reverse (min) | — | +55% | **−6%** | — |
| macOS / Clang | Random Reverse (min) | tie | — | +43% | **−27%** |
| macOS / GCC | Random Reverse (min) | +32% | +35% | — | **−38%** |

*"—" means the result did not converge in that run.*

Two failure modes repeat across builds:

- **At *T* = 2, Random Reverse loses on all four builds** where it converged. Random's objects are mid-sized, so almost every one crosses the threshold and pays for recording scans it didn't need.
- **At *T* = 4, Marine IK Reverse loses on two of the three builds** where it converged, for the same reason. On the third, Linux/GCC, it still wins, but its margin falls from +82% at *T* = 8 to +11%.

*T* = 8 is the lowest value with no reverse-order losses on any build. It also matched or beat *T* = 16 on most reverse tests: on Linux/Clang it lifted CitmCatalog Reverse from +85% to +106% and Discord Reverse from +307% to +365%, and in the sweep runs it turned Linux/Clang's in-order record from 21-0-2 into 22-1-0. (The sweep runs are separate from the runs reported in §7.)

The threshold is therefore chosen per OS/compiler, the same way Jsonifier chooses its stage-1 step geometry:

| Build | *T* | Basis |
|---|---|---|
| Linux / GCC | 8 | swept |
| Linux / Clang | 8 | swept |
| macOS / GCC | 8 | swept |
| macOS / Clang | 16 | swept; 8 and 16 had no reverse losses and could not be separated on this host |
| Windows / MSVC | 8 | not swept; uses the value that won on the other builds |

On macOS/Clang, 16 kept larger margins on the documents whose objects have many fields (Twitter, Discord, Mesh), while 8 won on Random, Google Maps and Instruments. Neither produced a reverse loss.

### 5.5 Requiring evidence of out-of-order access

A skipped-key count cannot tell a large object read out of order from a large object read sparsely in order. A sparse reader that asks for three fields of a 30-field object skips most of its keys on the way, so the count crosses *T* even though every lookup is found ahead of the cursor. Recording keys during those skips costs a hash and an insert per key for an index that no lookup needs.

The trigger in the measured code (§6) therefore asks for two kinds of evidence, both kept per object handle:

- **Size:** the skipped-key count has reached *T*, chosen per build as in §5.4. This is the gate that keeps small objects such as Marine IK's off the map.
- **Out-of-order access:** at least two lookups on this object have missed the expected field, scanned to the end of the object, and wrapped around to its start. A wrap is the event that only out-of-order reads produce. One wrap is not enough: a sparse reverse read of three fields often wraps once on its last lookup, whose key is first in the object, and an index built then would serve no further lookup.

Until both hold, lookups use scan-and-wrap and record nothing, so skips never pay for hashing. Once both hold, the next lookup that misses the expected field builds the object's index in one dedicated pass over the whole object, recording every key, and then probes it. Every later out-of-order lookup on that object is a single probe; a key the map does not hold falls through to the escape-aware retry of §3.4.

*T* was not re-swept after the wrap gate was added, and the wrap count of 2 is fixed on every build; it was not swept.

## 6. Methodology

- **Harness:** Json-Performance, branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 9 documents each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 154 single-document results per build at full size and another 144 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document and reused tests were added, and it has not been re-run on the measured commits.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results are dropped from all tallies. The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on a virtualized M1 and used RSE < 10% and shift < 5%.
- **Integer parsing:** Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 93 / 154 | 124 / 144 | 217 / 298 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 104 / 154 | 129 / 144 | 233 / 298 |

All builds ran against simdjson `7fa77b1`, with BenchmarkSuite `c4f600a`. The Windows/MSVC and Linux/Clang builds ran Jsonifier `8afa852`; the Linux/GCC and both macOS builds ran `b5a5bb2`, which differs from it by one line in `utf8_validation.hpp`. The x86 builds ran on an Intel Core i9-14900KF. The results were generated on October 6, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "—" means the result did not converge. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-0 | 18-0-0 | 18-0-0 | 18-0-0 | 16-0-2 | 18-0-0 | 18-0-0 | 18-0-0 | 145-2-2 |
| Linux / Clang | 5-0-0 | 17-1-0 | 18-0-0 | 15-1-2 | 14-2-2 | 7-4-7 | 17-0-1 | 8-5-5 | 11-2-5 | 112-15-22 |
| Linux / GCC | 5-0-0 | 8-2-8 | 16-0-2 | 16-1-1 | 17-1-0 | 4-2-12 | 16-0-2 | 10-1-7 | 16-1-1 | 108-8-33 |
| macOS / GCC | 5-0-0 | 5-3-0 | 9-0-0 | 7-1-1 | 13-1-0 | 11-1-3 | 15-0-0 | 13-3-0 | 15-1-0 | 93-10-4 |
| macOS / Clang | 4-0-0 | 5-2-3 | 10-0-1 | 7-1-5 | 7-1-3 | 9-4-4 | 13-1-1 | 5-4-7 | 11-2-2 | 71-15-26 |
| **All** | **24-0-0** | **51-10-11** | **71-0-3** | **63-4-9** | **69-5-5** | **47-11-28** | **79-1-4** | **54-13-19** | **71-6-8** | **529-50-87** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-0 | 18-0-0 | 18-0-0 | 18-0-0 | 16-0-2 | 18-0-0 | 18-0-0 | 18-0-0 | 145-2-2 |
| Linux / Clang | 5-0-0 | 17-1-0 | 17-1-0 | 14-2-2 | 13-3-2 | 11-2-5 | 18-0-0 | 10-3-5 | 13-1-4 | 118-13-18 |
| Linux / GCC | 5-0-0 | 10-2-6 | 16-2-0 | 16-0-2 | 18-0-0 | 4-4-10 | 16-0-2 | 8-4-6 | 17-1-0 | 110-13-26 |
| macOS / GCC | 5-0-0 | 7-0-3 | 7-1-0 | 11-1-0 | 10-2-1 | 11-1-2 | 15-0-1 | 12-3-1 | 16-0-0 | 94-8-8 |
| macOS / Clang | 2-1-1 | 11-2-1 | 9-0-1 | 11-3-2 | 6-2-3 | 10-4-2 | 15-0-2 | 10-4-3 | 10-1-5 | 84-17-20 |
| **All** | **22-1-1** | **61-7-10** | **67-4-1** | **70-6-6** | **65-7-6** | **52-11-21** | **82-0-5** | **58-14-15** | **74-3-9** | **551-53-74** |


Each cell is wins-ties-losses.

Across all 1,344 converged single-document results, Jsonifier won 1,080, tied 103 and lost 161: 529-50-87 with fresh output objects and 551-53-74 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 138 of 146 converged full-size reverse results (4 ties, 4 losses) and 161 of 171 small reverse results (1 tie, 9 losses). Eleven of those 13 losses are on Canada or Google Maps; the other two are Mesh Small Reverse (pretty) on macOS/Clang, fresh (−23.6%) and reused (−49.0%).
- **Windows/MSVC lost 4 of 298 single-document results,** all of them Canada Small read in order, by −0.9% to −3.3%; its 4 ties are full-size Canada read in order. It won every reverse, sparse and sparse-reverse test, at both sizes and in both allocation modes, and every POD test.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6.6–10.3% for Jsonifier and 4.6–7.5% for simdjson, depending on the build. Jsonifier's median margin over simdjson moves by +1.1 to +1.7 percentage points on the non-MSVC builds and by +6.9 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +1514.1% to +1704.5%, and Mesh Small Sparse (min) from +91.8% to +140.2%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +60.2% / +63.3% | +73.7% / +55.9% | +37.7% / +44.7% | +35.7% / +35.7% | +2.8% / tie |
| Double | +26.1% / +32.7% | +66.9% / +65.4% | +49.7% / +50.0% | +95.3% / +94.1% | +64.9% / — |
| Int64 | +25.7% / +24.7% | +71.2% / +86.3% | +38.1% / +40.5% | +26.1% / +26.1% | +6.6% / +8.3% |
| String | +2.1% / +13.2% | +10.1% / +10.5% | +4.7% / +2.7% | +68.9% / +36.7% | — / −12.3% |
| Uint64 | +94.4% / +99.6% | +86.0% / +84.5% | +60.7% / +71.4% | +52.2% / +109.8% | +26.3% / +32.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | tie / tie | tie / +7.0% | −8.8% / −3.8% | tie / — | +12.9% / +44.1% |
| Canada (pretty) | tie / tie | +2.9% / +3.6% | −4.2% / −9.0% | — / — | — / tie |
| CitmCatalog (min) | +22.9% / +24.3% | +7.1% / +5.4% | +8.7% / +12.0% | +5.7% / +27.0% | — / +145.0% |
| CitmCatalog (pretty) | +23.1% / +23.2% | +3.9% / +4.9% | +8.6% / +6.1% | +9.2% / +9.3% | tie / +4.9% |
| Discord (min) | +47.1% / +66.8% | +6.0% / +23.3% | +3.2% / +3.5% | — / +42.0% | +17.2% / +2.8% |
| Discord (pretty) | +42.5% / +60.4% | +4.6% / +16.9% | −4.5% / +4.4% | tie / — | +7.5% / +13.1% |
| Google Maps (min) | +27.3% / +34.0% | +20.5% / +21.3% | +13.9% / +11.9% | +6.6% / +59.6% | — / — |
| Google Maps (pretty) | +15.3% / +19.6% | +10.9% / +15.1% | +17.4% / +13.9% | +18.1% / +29.0% | −40.8% / tie |
| Instruments (min) | +38.4% / +42.6% | +12.3% / +17.5% | +1.7% / tie | — / — | — / +14.3% |
| Instruments (pretty) | +36.4% / +39.8% | +1.9% / +9.5% | tie / +5.5% | — / — | −15.9% / +25.7% |
| Marine IK (min) | +7.4% / +8.4% | +4.4% / +5.0% | −5.2% / −4.8% | — / — | — / — |
| Marine IK (pretty) | +9.5% / +10.6% | +4.1% / +4.9% | −3.3% / −3.9% | — / −3.9% | — / — |
| Mesh (min) | +29.3% / +33.6% | +5.1% / +7.6% | tie / tie | — / −20.2% | — / — |
| Mesh (pretty) | +25.5% / +27.6% | +2.3% / tie | −5.7% / −6.5% | tie / −5.2% | — / +15.0% |
| Random (min) | +43.0% / +53.6% | +21.0% / +36.9% | +18.3% / +30.3% | — / — | +81.8% / +31.5% |
| Random (pretty) | +41.2% / +51.0% | +30.9% / +37.4% | +21.9% / +29.0% | +42.9% / +58.6% | +19.2% / +23.1% |
| Twitter (min) | +32.7% / +43.7% | +4.5% / +11.9% | −2.7% / +3.8% | — / — | −2.4% / +8.2% |
| Twitter (pretty) | +32.9% / +42.4% | +3.5% / +11.6% | −9.2% / −6.6% | — / +21.2% | tie / −1.5% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +16.9% / +17.7% | +3.2% / +3.0% | −2.6% / tie | — / — | — / +30.7% |
| Canada (pretty) | +16.3% / +18.6% | +2.8% / +4.0% | −4.2% / tie | +26.4% / — | — / — |
| CitmCatalog (min) | +297.0% / +307.4% | +72.8% / +69.8% | +74.4% / +79.1% | +62.7% / +63.0% | — / — |
| CitmCatalog (pretty) | +279.9% / +290.6% | +58.3% / +64.5% | +67.6% / +77.8% | +62.5% / — | +83.8% / — |
| Discord (min) | +1514.1% / +1704.5% | +221.1% / +255.0% | +282.1% / +330.2% | +295.9% / +323.4% | +246.3% / +273.6% |
| Discord (pretty) | +1468.5% / +1654.8% | +238.5% / +251.9% | +297.2% / +305.3% | — / +376.5% | +219.2% / — |
| Google Maps (min) | +101.4% / +104.1% | +5.2% / tie | +15.1% / +27.3% | — / tie | +5.7% / −5.9% |
| Google Maps (pretty) | +97.4% / +102.2% | +7.1% / +4.3% | +14.6% / +10.9% | +42.0% / — | −17.6% / +5.2% |
| Instruments (min) | +656.3% / +676.2% | +124.0% / +129.9% | +121.5% / +125.2% | +58.1% / +157.2% | +137.7% / — |
| Instruments (pretty) | +642.3% / +668.6% | +118.9% / +108.6% | +125.3% / +135.9% | +134.6% / — | — / +90.0% |
| Marine IK (min) | +56.4% / +60.7% | +33.9% / +36.7% | +67.6% / +72.1% | — / — | — / — |
| Marine IK (pretty) | +58.0% / +60.6% | +41.6% / +39.2% | +54.1% / +60.7% | — / — | — / +21.8% |
| Mesh (min) | +299.8% / +324.8% | +33.3% / +34.5% | +52.6% / +57.2% | — / +60.0% | — / — |
| Mesh (pretty) | +278.5% / +304.9% | +26.7% / +33.5% | +49.8% / +39.7% | +24.9% / — | +45.7% / — |
| Random (min) | +218.7% / +247.7% | +23.1% / +27.8% | +21.7% / +25.5% | — / — | +22.2% / +37.0% |
| Random (pretty) | +220.4% / +249.2% | +23.5% / +30.0% | +22.3% / +21.9% | +18.7% / +12.8% | +19.6% / +20.7% |
| Twitter (min) | +700.3% / +777.9% | +176.9% / +191.0% | +128.5% / +128.6% | — / — | +156.6% / +172.3% |
| Twitter (pretty) | +661.4% / +753.0% | +202.4% / +213.2% | +115.3% / +119.3% | — / +145.2% | +145.1% / +116.4% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +53.2% / +53.4% | +5.7% / +6.9% | +18.8% / +16.3% | +27.8% / — | +35.4% / — |
| Canada (pretty) | +49.3% / +49.1% | +13.0% / +13.0% | +10.5% / +10.0% | — / — | — / +7.9% |
| CitmCatalog (min) | +219.3% / +233.1% | +24.0% / +24.4% | +16.7% / +18.0% | — / — | +21.5% / +21.1% |
| CitmCatalog (pretty) | +178.0% / +181.7% | +25.3% / +22.2% | +12.4% / +16.6% | — / +14.5% | +16.7% / +11.9% |
| Discord (min) | +29.1% / +37.3% | −7.1% / −6.9% | +4.7% / +4.8% | — / +16.8% | — / +7.5% |
| Discord (pretty) | +26.5% / +37.0% | +6.2% / tie | +3.4% / +3.3% | +30.8% / +20.3% | tie / +3.7% |
| Google Maps (min) | +51.3% / +56.6% | tie / −1.7% | −3.9% / −6.7% | — / +16.9% | −5.4% / tie |
| Google Maps (pretty) | +45.0% / +47.8% | −2.8% / tie | tie / −1.7% | −8.7% / +11.6% | −11.0% / +16.3% |
| Instruments (min) | +35.4% / +41.8% | +1.9% / +5.3% | +12.5% / +2.4% | +22.6% / +26.2% | −8.5% / −17.6% |
| Instruments (pretty) | +38.4% / +44.5% | +4.9% / +7.6% | +4.5% / +2.6% | +70.4% / +15.4% | −14.6% / −17.1% |
| Marine IK (min) | +36.8% / +40.0% | +17.8% / +15.2% | +39.5% / +35.4% | — / tie | +27.8% / +37.9% |
| Marine IK (pretty) | +42.4% / +41.0% | +18.6% / +19.1% | +20.2% / +23.2% | tie / — | — / +41.4% |
| Mesh (min) | +143.0% / +149.4% | +15.2% / +18.1% | +16.9% / +15.5% | +27.5% / — | — / — |
| Mesh (pretty) | +127.5% / +132.2% | +17.6% / +14.6% | +14.7% / +12.2% | — / +29.2% | +8.9% / +8.2% |
| Random (min) | +56.0% / +63.7% | +14.2% / +18.1% | +8.1% / +12.4% | — / +75.0% | +16.0% / +13.8% |
| Random (pretty) | +58.2% / +64.5% | +13.5% / +18.8% | +6.7% / +13.4% | +29.2% / +45.5% | — / +9.7% |
| Twitter (min) | +144.7% / +151.8% | +7.1% / +4.5% | +15.1% / +14.5% | — / — | +6.5% / tie |
| Twitter (pretty) | +124.3% / +136.9% | +1.8% / +6.2% | +7.3% / +6.3% | +7.5% / +49.9% | −8.4% / tie |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +251.7% / +254.7% | tie / tie | +21.2% / +23.3% | +22.5% / +22.3% | +16.2% / — |
| Canada (pretty) | +174.1% / +174.6% | +5.7% / +6.9% | +13.4% / +9.5% | +80.7% / tie | −4.5% / −6.3% |
| CitmCatalog (min) | +226.1% / +234.2% | +26.5% / +27.6% | +31.7% / +39.8% | — / — | — / — |
| CitmCatalog (pretty) | +191.6% / +194.0% | +25.3% / +25.8% | +32.5% / +33.2% | +59.6% / +18.7% | — / — |
| Discord (min) | +123.8% / +149.8% | −2.8% / −5.5% | +10.4% / +11.9% | tie / +54.9% | — / +8.1% |
| Discord (pretty) | +120.4% / +140.7% | −3.2% / tie | +4.2% / +12.3% | +7.6% / +10.9% | +26.7% / +12.6% |
| Google Maps (min) | +77.9% / +82.9% | +2.3% / tie | +7.5% / +11.4% | +9.8% / +91.9% | −18.2% / — |
| Google Maps (pretty) | +63.6% / +62.9% | +4.4% / −1.1% | tie / +6.2% | — / −17.2% | −26.6% / −18.4% |
| Instruments (min) | +184.6% / +200.8% | +14.4% / +9.3% | +26.7% / +26.6% | +77.6% / — | — / −5.1% |
| Instruments (pretty) | +173.1% / +191.9% | +5.4% / +9.3% | +22.2% / +23.1% | — / +37.2% | — / +27.3% |
| Marine IK (min) | +238.1% / +234.9% | +15.7% / +15.6% | +67.5% / +63.8% | +25.8% / +14.5% | — / — |
| Marine IK (pretty) | +186.2% / +192.2% | +13.0% / +17.3% | +32.8% / +37.0% | +91.2% / +35.5% | — / tie |
| Mesh (min) | +132.2% / +151.5% | +15.1% / +17.1% | +17.4% / +19.4% | +31.0% / — | +31.7% / +19.6% |
| Mesh (pretty) | +126.1% / +131.5% | +14.4% / +13.4% | +8.2% / +10.8% | +10.2% / tie | +12.0% / — |
| Random (min) | +78.6% / +85.2% | +9.2% / +8.0% | +11.6% / +12.8% | +52.7% / — | tie / +12.5% |
| Random (pretty) | +79.5% / +84.1% | +11.9% / +8.2% | +12.9% / +8.3% | — / +21.2% | +8.9% / +19.9% |
| Twitter (min) | +192.6% / +205.2% | +9.0% / +11.2% | +14.1% / +15.0% | +35.3% / +9.2% | +9.5% / tie |
| Twitter (pretty) | +179.1% / +189.7% | tie / +9.2% | +10.4% / +13.7% | +76.7% / — | +26.7% / — |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | −3.3% / −1.7% | +3.3% / tie | −12.0% / −12.1% | +8.4% / — | −22.7% / — |
| Canada (pretty) | −0.9% / −1.1% | +1.7% / +1.2% | −10.8% / −12.8% | −11.3% / +8.8% | −32.5% / tie |
| CitmCatalog (min) | +15.0% / +20.4% | −8.2% / −2.1% | −13.9% / tie | tie / — | tie / −3.6% |
| CitmCatalog (pretty) | +10.7% / +17.6% | −6.9% / −8.3% | −4.9% / −4.6% | −2.0% / tie | −9.2% / tie |
| Discord (min) | +25.7% / +52.5% | tie / +6.6% | −3.8% / tie | +21.9% / +58.5% | — / +36.6% |
| Discord (pretty) | +17.8% / +47.5% | −1.6% / +1.1% | −4.0% / tie | +13.8% / +23.7% | tie / tie |
| Google Maps (min) | +26.6% / +31.3% | +16.6% / +21.6% | +11.3% / +13.0% | +26.7% / +31.8% | −4.0% / — |
| Google Maps (pretty) | +18.8% / +22.2% | +12.2% / +15.3% | +9.4% / +12.3% | +27.8% / +41.5% | tie / +22.9% |
| Instruments (min) | +35.2% / +40.9% | +1.4% / +1.0% | tie / tie | −5.8% / +17.0% | +6.7% / +25.2% |
| Instruments (pretty) | +29.9% / +29.5% | −2.5% / −2.4% | tie / −2.2% | +14.4% / — | +11.9% / +11.6% |
| Marine IK (min) | +0.9% / +4.3% | tie / tie | −5.2% / −4.2% | +24.0% / +22.7% | +4.0% / +8.8% |
| Marine IK (pretty) | +5.2% / +5.6% | −4.6% / +1.6% | −9.0% / −6.0% | +9.4% / — | +9.5% / tie |
| Mesh (min) | +5.4% / +20.7% | −8.3% / −3.5% | −7.1% / −7.6% | — / −19.9% | +11.1% / +15.1% |
| Mesh (pretty) | +4.3% / +14.1% | −10.5% / −7.8% | −10.1% / −12.8% | — / −11.7% | +10.5% / −4.4% |
| Random (min) | +32.5% / +38.3% | +8.5% / +13.5% | +19.7% / +25.5% | +35.8% / +78.2% | +14.0% / +21.1% |
| Random (pretty) | +31.6% / +43.0% | +12.4% / +17.5% | +13.4% / +19.0% | — / +43.5% | +27.4% / +23.4% |
| Twitter (min) | +35.0% / +46.2% | tie / +3.6% | −3.1% / −9.5% | +45.2% / +46.9% | tie / +8.2% |
| Twitter (pretty) | +30.6% / +39.7% | tie / +2.4% | −4.9% / −6.8% | +19.8% / +14.7% | +19.5% / +22.6% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +9.8% / +13.9% | +4.2% / +1.9% | −5.6% / −5.2% | +72.5% / −10.5% | tie / −8.4% |
| Canada (pretty) | +9.5% / +14.1% | +4.4% / +1.8% | −4.8% / −6.8% | +14.9% / — | +6.6% / +12.3% |
| CitmCatalog (min) | +209.9% / +229.7% | +44.1% / +50.7% | +51.0% / +73.0% | +77.8% / — | — / — |
| CitmCatalog (pretty) | +199.2% / +225.6% | +47.2% / +53.7% | +60.1% / +62.8% | — / +92.5% | +49.8% / +38.6% |
| Discord (min) | +1183.6% / +1478.7% | +223.0% / +263.3% | +268.4% / +312.0% | +314.9% / +202.4% | +252.7% / +275.6% |
| Discord (pretty) | +1173.9% / +1472.0% | +222.9% / +291.3% | +264.5% / +283.5% | +415.2% / +394.1% | +290.9% / +286.0% |
| Google Maps (min) | +91.3% / +113.3% | +1.5% / +2.7% | +12.4% / +19.4% | +18.8% / +20.2% | +8.3% / +36.2% |
| Google Maps (pretty) | +83.9% / +105.3% | −1.3% / +1.9% | +12.8% / +13.8% | +16.8% / +19.7% | +26.4% / +21.3% |
| Instruments (min) | +662.5% / +668.4% | +141.2% / +121.2% | +134.0% / +143.2% | +158.6% / +167.8% | +82.1% / +117.4% |
| Instruments (pretty) | +648.0% / +679.9% | +112.2% / +133.0% | +120.6% / +109.7% | +146.2% / +184.0% | +104.4% / +520.6% |
| Marine IK (min) | +178.1% / +191.7% | +32.0% / +31.9% | +51.4% / +56.8% | +70.6% / +48.2% | +46.6% / +29.2% |
| Marine IK (pretty) | +178.3% / +194.4% | +34.9% / +39.7% | +58.8% / +49.2% | +45.0% / +55.1% | — / +42.9% |
| Mesh (min) | +121.3% / +187.3% | +15.8% / +21.2% | +28.1% / +35.3% | — / +77.6% | — / +36.7% |
| Mesh (pretty) | +111.3% / +164.1% | +9.1% / +17.5% | +22.2% / +28.7% | +70.4% / +36.6% | −23.6% / −49.0% |
| Random (min) | +234.6% / +274.4% | +39.5% / +44.6% | +37.5% / +37.3% | +133.9% / +72.4% | +54.8% / +42.7% |
| Random (pretty) | +236.1% / +265.7% | +36.4% / +40.1% | +34.2% / +32.9% | +48.5% / +91.3% | +26.5% / +37.5% |
| Twitter (min) | +705.9% / +777.0% | +184.1% / +178.1% | +144.0% / +149.1% | +263.7% / +179.1% | +129.0% / +151.8% |
| Twitter (pretty) | +717.7% / +763.4% | +163.8% / +173.1% | +128.6% / +157.7% | — / +215.5% | +167.0% / +148.7% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +26.7% / +21.1% | +27.3% / +23.8% | +10.7% / +10.9% | +40.4% / +41.0% | — / +20.3% |
| Canada (pretty) | +39.0% / +33.1% | +23.2% / +26.0% | +18.5% / +17.8% | +22.7% / +28.0% | −2.7% / +2.5% |
| CitmCatalog (min) | +159.7% / +185.4% | +14.2% / +22.5% | +18.1% / +13.7% | tie / tie | +10.5% / +11.0% |
| CitmCatalog (pretty) | +130.1% / +142.5% | +10.3% / +15.0% | +13.7% / +12.1% | tie / +11.7% | −2.9% / +34.3% |
| Discord (min) | +25.8% / +41.1% | tie / tie | −2.2% / −2.2% | +16.2% / +20.5% | tie / −7.8% |
| Discord (pretty) | +24.9% / +39.3% | +3.1% / −1.9% | −2.2% / −3.6% | +22.5% / −3.7% | — / tie |
| Google Maps (min) | +59.5% / +60.1% | −1.6% / tie | −8.0% / −6.7% | +10.2% / +12.8% | tie / — |
| Google Maps (pretty) | +46.0% / +47.7% | −2.4% / −0.8% | −5.1% / −8.4% | +40.0% / tie | tie / tie |
| Instruments (min) | +28.7% / +32.1% | tie / +0.8% | tie / tie | +39.2% / — | −1.8% / tie |
| Instruments (pretty) | +21.2% / +29.3% | tie / +2.1% | −3.0% / tie | +24.3% / +91.2% | −7.0% / −12.9% |
| Marine IK (min) | +13.6% / +15.7% | +6.2% / +7.4% | +18.3% / +19.5% | — / +21.0% | +38.9% / +22.1% |
| Marine IK (pretty) | +17.5% / +16.2% | +10.5% / +11.2% | +16.1% / +16.2% | — / +14.8% | −11.4% / +7.2% |
| Mesh (min) | +91.8% / +140.2% | tie / tie | +12.2% / +13.2% | +12.2% / — | +19.0% / tie |
| Mesh (pretty) | +76.7% / +118.1% | −3.1% / −0.9% | +2.3% / +5.3% | tie / tie | −10.1% / +24.4% |
| Random (min) | +41.6% / +45.3% | +2.4% / +3.7% | +3.8% / tie | +31.3% / +49.8% | +2.1% / +3.3% |
| Random (pretty) | +39.9% / +50.7% | tie / +4.8% | +3.1% / tie | +25.2% / +25.0% | +1.9% / +8.2% |
| Twitter (min) | +113.6% / +117.3% | −11.2% / −10.7% | −3.1% / −6.3% | +9.2% / +16.5% | tie / +13.4% |
| Twitter (pretty) | +97.6% / +105.5% | −9.0% / −10.9% | −9.8% / −5.2% | +21.7% / +18.0% | −11.1% / −17.7% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +138.6% / +150.4% | +24.1% / +23.0% | +22.8% / +17.6% | +53.5% / +63.3% | tie / tie |
| Canada (pretty) | +129.8% / +133.2% | +21.2% / +21.4% | +20.4% / +18.0% | +41.2% / +21.5% | — / −3.0% |
| CitmCatalog (min) | +145.6% / +147.5% | +16.2% / +17.0% | +29.9% / +28.9% | — / +24.8% | +9.6% / — |
| CitmCatalog (pretty) | +126.2% / +118.6% | +16.8% / +15.8% | +25.4% / +25.0% | +21.0% / +7.7% | +20.2% / +2.0% |
| Discord (min) | +143.4% / +164.5% | tie / +4.0% | +4.9% / +7.9% | — / +43.2% | +15.2% / +184.1% |
| Discord (pretty) | +131.8% / +159.1% | −2.5% / −2.2% | +4.2% / +7.0% | +31.6% / +46.3% | +5.3% / +15.8% |
| Google Maps (min) | +74.4% / +80.7% | −6.7% / +4.7% | +5.1% / +2.0% | +29.5% / +13.4% | −12.3% / −25.2% |
| Google Maps (pretty) | +67.5% / +70.7% | +2.8% / −1.3% | tie / +2.3% | +12.9% / +2.8% | +6.6% / −5.5% |
| Instruments (min) | +193.0% / +211.1% | +6.2% / +9.6% | +18.2% / +19.0% | +30.5% / +48.1% | tie / +13.0% |
| Instruments (pretty) | +164.0% / +186.4% | +5.1% / +10.1% | +14.4% / +16.4% | +28.9% / +34.5% | +1.8% / −5.3% |
| Marine IK (min) | +123.3% / +112.2% | +19.9% / +12.0% | +45.3% / +47.9% | +40.2% / — | — / +25.8% |
| Marine IK (pretty) | +85.6% / +105.0% | +15.3% / +13.9% | +37.3% / +41.4% | +66.4% / +42.5% | — / +17.0% |
| Mesh (min) | +105.8% / +146.6% | −3.5% / +1.1% | +13.9% / +11.7% | tie / +8.2% | +11.3% / +8.6% |
| Mesh (pretty) | +90.6% / +131.3% | −4.2% / −4.5% | +9.5% / +10.1% | +39.3% / — | −9.8% / +235.4% |
| Random (min) | +66.2% / +77.7% | +4.5% / +5.7% | +8.5% / tie | +40.7% / +33.0% | +4.2% / +5.2% |
| Random (pretty) | +67.9% / +77.7% | +2.8% / +1.8% | +4.6% / +6.5% | +20.4% / +12.3% | +5.2% / +8.5% |
| Twitter (min) | +153.1% / +155.1% | tie / −3.4% | +3.3% / +5.3% | +16.7% / +15.4% | +16.2% / — |
| Twitter (pretty) | +142.7% / +148.2% | −1.5% / tie | −0.6% / +2.4% | +23.3% / +19.4% | +8.5% / −9.3% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.6x to 18x faster at full size: +58.1% (Instruments min, macOS/GCC, fresh) up to +1704.5% (Discord min, MSVC, reused). The small documents show the same pattern, from +82.1% to +1478.7%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −17.6% to +42.0%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are a strength.** Jsonifier won 134 of 157 converged full-size sparse-reverse results across both allocation modes (12 ties, 11 losses), and 145 of 171 small ones (9 ties, 17 losses). MSVC won all 72 of its sparse-reverse results, and Linux/GCC won all of its full-size ones except one tie. Of the 11 full-size losses, 6 are on macOS/Clang and 4 on Linux/Clang, and 5 of the 11 are Google Maps.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 33.4 MB/s on MSVC against 228.3 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 524.5 and 772.8 MB/s. Every MSVC reverse result is a win, by +56.4% or more outside Canada (+9.5% to +18.6%); every MSVC sparse result is a win, by +13.6% to +233.1%, and every sparse-reverse result by +62.9% to +254.7%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 133 of 158 converged full-size sparse results (10 ties, 15 losses) and 112 of 173 small ones (27 ties, 34 losses). MSVC won every one, at both sizes. On full-size documents, 8 of the 15 losses are Google Maps, and 4 of the 7 on macOS/Clang are Instruments.
- **In-order full reads are mostly wins at full size.** Across 150 converged full-size in-order results, Jsonifier won 112, tied 17 and lost 21. MSVC went 32-4-0 and Linux/Clang 34-2-0. Linux/GCC split its in-order tests 18-4-14 across the two modes, macOS/GCC 12-3-3 and macOS/Clang 16-4-4.
- **Small documents read in order are the weakest category.** Across 170 converged results, Jsonifier won 99, tied 22 and lost 49. MSVC (32-0-4) and macOS/GCC (22-2-5) won most of theirs, but Linux/Clang went 18-6-12 and Linux/GCC 8-6-22. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The Linux losses there are modest (−1.6% to −13.9%), while the in-order small-document wins on the same builds, on Google Maps and Random, run from +8.5% to +25.5%.
- **In-order losses at full size cluster on number-heavy documents.** Canada, Marine IK and Mesh lose in order on Linux/GCC (−3.3% to −9.0%), and Marine IK and Mesh also on macOS/GCC (−3.9% to −20.2%). These documents are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. The other Linux/GCC losses are Twitter (−2.7% to −9.2%) and Discord (pretty, fresh, −4.5%).
- **The POD tests win on almost every build.** Jsonifier went 46-1-1 on them. Double won all 9 converged results (+26.1% to +95.3%), Int64 all 10 (+6.6% to +86.3%) and Uint64 all 10 (+26.3% to +109.8%). Bool won 9 and tied 1, and String won 8 of 9; the one POD loss is String on macOS/Clang (reused, −12.3%).

## 8. Streams: many documents in one buffer

Many real inputs are not one document but a sequence of them: NDJSON logs, JSON Lines exports, or documents joined by commas. simdjson handles these with `iterate_many`, which runs stage 1 over a window of the input and hands out one On Demand document at a time. `jsonifier::generic` does the same through `parser::iterateMany`, and every document it hands out is an ordinary `generic::document`, with re-readable values and the per-object field index from §5.

### 8.1 Design

```cpp
for (jsonifier::generic::document doc : parser.iterateMany(buffer)) { ... }
auto stream = parser.iterateMany<parse_options{ .newLineDelimited = false }>(buffer, batchSize);
```

- **The separator is part of the type.** `parse_options::newLineDelimited` selects it: `true` (NDJSON and other whitespace-separated streams, the `iterateMany` default) or `false` for comma-delimited streams. The two modes find document boundaries in different ways (§8.2), and the choice is made at compile time.
- **Windowed stage 1.** Stage 1 runs over one window of `batchSize` bytes (1 MiB by default), using the same indexer, and the same small-input routing, as `iterate()`. Tape offsets are relative to the window, so no single window can exceed 4 GiB, but the stream as a whole can.
- **One document per tape slice.** Each document is bound to the parser's shared cursor (§3.2), starting at its first structural. The accessors, the lazy errors and the field index work unchanged; the field index is reset per document.
- **Documents cut by the window edge.** No document is ever parsed from two windows. The next window starts at the first byte of the first document that the current window could not complete, and a document larger than a whole window returns `error_code::capacity`, as in simdjson.
- **Comma-separated streams.** In comma mode, a comma between top-level documents is accepted as a separator. A leading or doubled comma is a `tape_error`, and a trailing comma is `trailing_content`. A comma that falls at the start of a new window is still consumed as a separator.
- **Reporting.** The stream exposes `truncatedBytes()` for an unfinished final document, and each iterator exposes `currentIndex()` and `source()` for the current document's offset and raw text. After an error, the stream yields that error once and then ends.
- **Lifetime.** As with simdjson, advancing the stream invalidates the previous document; the only thing that survives is a zero-copy view of an unescaped string, which points into the caller's buffer.

### 8.2 Finding where documents end

The simplest way to find each document's end is to walk its structurals and count depth before handing the document out. That bounds every later tape scan to the document, but a reader that consumes the document in order walks the same structurals again, so every in-order document pays for two passes over its part of the tape.

The whitespace-delimited mode finds boundaries without that pass:

- **A local rule marks document starts.** Inside any container, every value is preceded on the tape by `[`, `{`, `,` or `:`. So in a whitespace-delimited stream, a structural that can start a value and directly follows a closer or a scalar begins a new top-level document. No depth needs to be tracked to recognize it.
- **One short backward scan per window.** Scanning backward from the end of the window's tape, the first position that satisfies the rule is the start of the window's last document. Everything before it is complete. That last document is deferred to the next window unless this is the final window; only when a window holds a single document, or for the final document of the final window, is a depth walk used to decide completeness. The scan usually stops within a few structurals of the window's end.
- **The reader tells the stream where the document ended.** When the caller advances, the parser's state already records how far the reads went. If the cursor is still inside the document, the stream finishes the open containers from the cursor. For an in-order reader, whose cursor rests just before the closing brace, that is about one step. If the cursor is past a closed root value, it is the end. Only a document the caller never touched is skipped from its start.

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 2.7% to 26.7% below its own NDJSON throughput on the same records, and 64.5% below it on Instruments under Linux/Clang.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 22 / 26 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 26 / 26 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 26 / 26 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 7 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 9 / 26 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 6-0-0 | 22-0-0 |
| Linux / Clang | 4-1-1 | 10-0-0 | 9-0-1 | 23-1-2 |
| Linux / GCC | 4-1-1 | 8-0-2 | 8-0-2 | 20-1-5 |
| macOS / GCC | 0-1-0 | 4-0-0 | 2-0-0 | 6-1-0 |
| macOS / Clang | 1-0-1 | 3-0-1 | 2-1-0 | 6-1-2 |
| **All** | **15-3-3** | **35-0-3** | **27-1-3** | **77-4-9** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +47.1% | +18.1% | +9.4% | — | +48.4% |
| Large Amazon Cellphones (NDJSON) | +39.4% | +8.5% | tie | — | — |
| Stream Formats Small (NDJSON) | +38.5% | −7.4% | +3.7% | tie | −27.2% |
| Stream Formats Small (comma) | +70.6% | +70.3% | +96.8% | — | — |
| Stream Formats Large (NDJSON) | +27.8% | tie | −6.3% | — | — |
| Stream Formats Large (comma) | +23.0% | +10.2% | +1.9% | — | — |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +24.4% | +2.6% | +17.9% | — | — |
| CitmCatalog (comma) | +26.9% | +12.4% | +25.4% | — | +26.6% |
| Google Maps (NDJSON) | +34.0% | +31.1% | +21.4% | — | — |
| Google Maps (comma) | +30.6% | +26.6% | +35.3% | +43.8% | — |
| Instruments (NDJSON) | +61.4% | +20.1% | −8.3% | — | — |
| Instruments (comma) | +38.5% | +12.6% | +33.8% | +35.2% | — |
| Random (NDJSON) | +45.9% | +28.9% | +23.7% | +39.1% | — |
| Random (comma) | +41.5% | +32.3% | +42.4% | — | +42.7% |
| Twitter (NDJSON) | +38.9% | +9.4% | −7.0% | +19.7% | −17.0% |
| Twitter (comma) | +32.8% | +21.4% | +11.3% | — | +17.7% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | — | +42.4% | +39.8% | — | +28.4% |
| CitmCatalog (comma) | — | +57.9% | +63.5% | — | — |
| Google Maps (NDJSON) | +57.0% | +6.4% | +7.4% | — | tie |
| Google Maps (comma) | +48.9% | +12.5% | +20.3% | — | — |
| Instruments (NDJSON) | +70.5% | −16.3% | −28.1% | — | — |
| Instruments (comma) | +78.7% | +2.5% | −13.2% | +22.1% | +20.8% |
| Random (NDJSON) | +270.7% | +46.2% | +37.1% | +57.5% | — |
| Random (comma) | +279.4% | +69.1% | +57.6% | — | — |
| Twitter (NDJSON) | — | +220.1% | +128.2% | — | — |
| Twitter (comma) | — | +242.3% | +153.8% | — | — |

conv windows-msvc: full 154/154 small 144/144 total 298/298 stream 22/26
conv linux-clang: full 154/154 small 144/144 total 298/298 stream 26/26
conv linux-gcc: full 154/154 small 144/144 total 298/298 stream 26/26
conv macos-gcc: full 93/154 small 124/144 total 217/298 stream 7/26
conv macos-clang: full 104/154 small 129/144 total 233/298 stream 9/26

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 15 of the 21 converged results in that group, tied 3 and lost 3. It won Amazon Cellphones on all four builds where it converged (+9.4% to +48.4%), and Stream Formats Small (comma) on all three (+70.3% to +96.8%). The losses are Stream Formats Small (NDJSON) on Linux/Clang (−7.4%) and macOS/Clang (−27.2%), and Stream Formats Large (NDJSON) on Linux/GCC (−6.3%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test that converged,** 22-0-0, and Linux/Clang lost two of 26.
- **In-order corpus streams are mostly wins.** Of the 38 converged in-order corpus results, Jsonifier won 35 and lost 3: Instruments (NDJSON, −8.3%) and Twitter (NDJSON, −7.0%) on Linux/GCC, and Twitter (NDJSON) on macOS/Clang (−17.0%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 27 of 31 converged reverse stream results and tied one. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +128.2% to +242.3%, while Google Maps rows, which have few, give a tie to +57.0%. All three losses are Instruments: NDJSON on Linux/Clang (−16.3%) and Linux/GCC (−28.1%), and comma on Linux/GCC (−13.2%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 122.5 MB/s under MSVC and 427.1 MB/s under Linux/Clang, while Jsonifier reads it at 464.8 and 722.3 MB/s.

## 9. Limitations

- **Linux/GCC has the most single-document losses.** It holds 59 of the 161, more than macOS/Clang (46) or Linux/Clang (40). They are concentrated on reads in document order: 22 of its 36 converged small in-order results and 14 of its 36 full-size ones, on Canada, Marine IK, Mesh, Twitter and Discord, by up to −13.9% (CitmCatalog Small, min, fresh). It also loses reverse reads of Canada at both sizes (−2.6% to −6.8%), while it won or tied every full-size sparse-reverse result.
- **Small documents read in order lose on Linux.** Linux/Clang lost 12 of its 36 converged small in-order results and Linux/GCC 22 of 36, while Linux/Clang won every full-size in-order test it did not tie. At 5 KiB per document, Jsonifier's fixed per-parse cost is the next target.
- **macOS/Clang has the largest individual losses.** The largest loss in the suite is Mesh Small Reverse (pretty, reused) at −49.0%, and Google Maps (pretty, fresh) loses in order by −40.8% while its reused run is a tie. It ran on a virtualized M1, so part of these results likely reflects host noise, but its losses are not confined to one access pattern.
- **Number-heavy documents in order** remain a common loss. Canada, Marine IK and Mesh lose in order on Linux/GCC, and Marine IK and Mesh on macOS/GCC. The cause is float-array throughput rather than out-of-order access, so it is out of scope for the field index.
- **Instruments reverse streams** lose on both Linux builds in NDJSON form (−16.3% and −28.1%) and on Linux/GCC in comma form (−13.2%), although the single-document Instruments Reverse tests win on every build that converged.
- **Convergence is uneven.** The three x86 builds converged on all 298 single-document results, but macOS/GCC met the limits on only 93 of its 154 full-size single-document results and macOS/Clang on 104, even with the looser NEON limits. The NEON builds converged on only 7 (macOS/GCC) and 9 (macOS/Clang) of the 26 streaming tests, and MSVC's CitmCatalog and Twitter reverse streams did not converge in either form.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. On the x86 in-order corpus streams this leaves comma mode 2.7% to 26.7% slower than NDJSON mode on the same records, and 64.5% slower on Instruments under Linux/Clang. The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commits.** The byte-for-byte comparison in §6 ran on `3e7bf6d`, not on `8afa852` or `b5a5bb2`, and it does not cover the small-document or reused tests.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,344 converged single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,080, tied 103 and lost 161. On reverse reads it won 299 of 317 at both sizes, and on Windows/MSVC it lost 4 single-document results of 298, all by under 3.5%. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (529-50-87 fresh, 551-53-74 reused). The main remaining targets are Linux/GCC, with 59 of the 161 losses, and small documents read in order on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. The one lesson it added repeats §5's: finding every document's end up front makes an in-order reader pay for two passes over the tape, and letting the reader's own cursor report where each document ended removes that pass for whitespace-delimited streams. Across 90 converged streaming results, Jsonifier won 77, tied 4 and lost 9, and it won every converged streaming test on Windows/MSVC. Instruments reverse streams on Linux are the main remaining streaming target.

The main engineering lesson is in §5. An index over the whole document made the target workload 2.3x *slower*, because most objects are small. The version that works builds the index lazily, per object, and only once that object has shown both that it is large and that it is being read out of order: enough skipped keys to clear *T*, and two lookups that had to wrap around. In short: make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.