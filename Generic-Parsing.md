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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,141 of 1,410 converged single-document results against simdjson On Demand, tied 65 and lost 204. On reverse-order reads it won 308 of 333. On Windows/MSVC it won 282 of 291 single-document results, tied 6 and lost 3.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests, Jsonifier won 76 of 89 converged results, tied 6 and lost 7, and won every streaming test that converged on Windows/MSVC.

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
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document and reused tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results are dropped from all tallies. The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on a virtualized M1 and used RSE < 10% and shift < 5%.
- **Integer parsing:** Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 147 / 154 | 144 / 144 | 291 / 298 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 150 / 154 | 134 / 144 | 284 / 298 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 145 / 154 | 137 / 144 | 282 / 298 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 127 / 154 | 143 / 144 | 270 / 298 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 143 / 154 | 140 / 144 | 283 / 298 |

All builds ran Jsonifier `f356735` against simdjson `2a690bc`, with BenchmarkSuite `4e7c701`. (The MSVC build's report names `62765c9`, an earlier version of the same commit with identical source code.) The x86 builds ran on an Intel Core i9-14900KF. The results were generated on October 3, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "—" means the result did not converge. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 4-0-1 | 15-2-0 | 16-0-0 | 17-0-0 | 18-0-0 | 17-1-0 | 18-0-0 | 18-0-0 | 18-0-0 | 141-3-1 |
| Linux / Clang | 5-0-0 | 16-1-0 | 15-1-1 | 16-2-0 | 17-1-0 | 5-1-10 | 14-1-2 | 9-4-4 | 9-2-6 | 106-13-23 |
| Linux / GCC | 4-0-0 | 7-0-10 | 15-0-2 | 17-0-0 | 17-1-0 | 5-2-9 | 16-0-2 | 13-1-1 | 18-0-0 | 112-4-24 |
| macOS / GCC | 3-1-1 | 12-0-3 | 13-0-0 | 13-1-0 | 14-1-0 | 16-1-1 | 18-0-0 | 18-0-0 | 14-2-2 | 121-6-7 |
| macOS / Clang | 3-0-2 | 7-1-6 | 13-0-1 | 9-0-9 | 7-0-10 | 12-3-3 | 16-0-1 | 9-2-7 | 10-2-5 | 86-8-44 |
| **All** | **19-1-4** | **57-4-19** | **72-1-4** | **72-3-9** | **73-3-10** | **55-8-23** | **82-1-5** | **67-7-12** | **69-6-13** | **566-34-99** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 4-0-1 | 16-2-0 | 16-0-0 | 18-0-0 | 17-0-0 | 16-1-1 | 18-0-0 | 18-0-0 | 18-0-0 | 141-3-2 |
| Linux / Clang | 5-0-0 | 16-0-2 | 14-2-0 | 16-1-1 | 14-1-3 | 7-0-10 | 15-0-2 | 9-3-6 | 10-1-4 | 106-8-28 |
| Linux / GCC | 5-0-0 | 8-0-7 | 15-0-2 | 15-0-2 | 18-0-0 | 6-1-11 | 16-0-2 | 15-0-3 | 16-0-0 | 114-1-27 |
| macOS / GCC | 4-1-0 | 13-2-2 | 11-0-1 | 14-0-2 | 15-0-0 | 15-2-1 | 18-0-0 | 17-1-0 | 15-2-0 | 122-8-6 |
| macOS / Clang | 3-0-1 | 9-2-6 | 15-1-2 | 9-0-9 | 8-2-8 | 13-0-4 | 16-1-1 | 9-2-7 | 10-3-4 | 92-11-42 |
| **All** | **21-1-2** | **62-6-17** | **71-3-5** | **72-1-14** | **72-3-11** | **57-4-27** | **83-1-5** | **68-6-16** | **69-6-8** | **575-31-105** |

Each cell is wins-ties-losses.

Across all 1,410 converged single-document results, Jsonifier won 1,141, tied 65 and lost 204: 566-34-99 with fresh output objects and 575-31-105 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 143 of 156 converged full-size reverse results (4 ties, 9 losses) and 165 of 177 small reverse results (2 ties, 10 losses). All but one of those 19 losses are on Canada or Google Maps; the exception is Random (pretty, reused) on macOS/Clang (−9.0%).
- **Windows/MSVC lost 3 of 291 single-document results:** the String POD test, fresh (−5.1%) and reused (−7.4%), and Canada Small (min, reused) by −1.3%. It won every reverse, sparse and sparse-reverse test that converged, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6–9% for Jsonifier and 5–7% for simdjson, depending on the build. Jsonifier's median margin over simdjson moves by +0.8 to +1.8 percentage points on the non-MSVC builds and by +6.1 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +1424.4% to +1590.1%, and Mesh Small Sparse (min) from +90.5% to +142.9%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +72.3% / +79.1% | +42.1% / +48.7% | +72.5% / +67.1% | tie / tie | −2.3% / +16.3% |
| Double | +34.9% / +29.9% | +61.2% / +65.5% | +73.7% / +76.8% | +80.9% / +80.6% | +72.6% / +72.5% |
| Int64 | +20.2% / +23.3% | +67.1% / +68.9% | +43.1% / +44.6% | +66.5% / +48.5% | +204.8% / — |
| String | −5.1% / −7.4% | +6.9% / +9.7% | — / +10.0% | −1.2% / +9.4% | −12.3% / −16.3% |
| Uint64 | +90.3% / +86.6% | +57.3% / +64.2% | +60.0% / +63.8% | +30.3% / +37.6% | +50.7% / +13.6% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | tie / tie | +4.5% / +4.4% | −9.7% / — | +3.1% / +6.9% | +7.3% / +13.9% |
| Canada (pretty) | — / tie | +4.4% / +12.5% | −7.1% / — | −1.8% / +1.3% | — / +12.0% |
| CitmCatalog (min) | +31.9% / +37.1% | +3.9% / +3.9% | +9.7% / +10.3% | — / +1.6% | +10.7% / tie |
| CitmCatalog (pretty) | +30.1% / +34.8% | +2.1% / +4.0% | +5.0% / +7.2% | +36.8% / — | +4.5% / +9.7% |
| Discord (min) | +25.8% / +35.7% | +10.7% / +17.7% | +4.4% / +12.2% | +10.4% / tie | +4.2% / +4.4% |
| Discord (pretty) | +26.1% / +35.6% | +5.6% / +14.6% | +8.0% / +3.3% | +2.9% / +56.8% | −8.8% / −5.4% |
| Google Maps (min) | +9.8% / +9.4% | +13.3% / +16.3% | — / +14.3% | +14.0% / +15.8% | −1.6% / −1.9% |
| Google Maps (pretty) | tie / +1.2% | +10.8% / +13.7% | +14.3% / +15.0% | +16.0% / +16.6% | −4.4% / −4.4% |
| Instruments (min) | +31.9% / +34.2% | +6.6% / +7.5% | −3.9% / −3.1% | +7.0% / +4.7% | tie / +3.3% |
| Instruments (pretty) | +23.7% / +28.2% | +2.0% / +3.4% | −3.3% / −4.3% | +11.9% / +6.6% | −13.6% / −4.6% |
| Marine IK (min) | +11.2% / +9.5% | +3.0% / −5.0% | −4.8% / — | — / +3.3% | +7.4% / +5.8% |
| Marine IK (pretty) | +13.2% / +14.2% | — / −6.3% | −6.3% / −4.7% | — / tie | — / +8.0% |
| Mesh (min) | +32.8% / +37.8% | +3.1% / +3.0% | −3.4% / −4.4% | −1.7% / −3.4% | +4.5% / +10.0% |
| Mesh (pretty) | +26.9% / +34.0% | +2.1% / +6.4% | −7.2% / −8.5% | −7.8% / −5.2% | +7.7% / +7.3% |
| Random (min) | +9.0% / +11.2% | +25.7% / +30.0% | +14.5% / +20.1% | +20.9% / +22.3% | — / −20.3% |
| Random (pretty) | +7.9% / +13.0% | +20.6% / +33.5% | +11.5% / +19.9% | +18.4% / +20.8% | — / — |
| Twitter (min) | +20.8% / +26.5% | +4.1% / +16.5% | −9.6% / −0.9% | +14.7% / +9.2% | −4.9% / −3.0% |
| Twitter (pretty) | +18.6% / +25.5% | tie / +15.2% | −15.3% / −12.9% | +6.2% / +6.3% | −2.4% / tie |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +20.4% / +17.6% | +3.8% / +4.6% | −8.6% / −5.8% | +9.3% / +10.5% | +6.5% / +11.5% |
| Canada (pretty) | +19.1% / +19.3% | +2.2% / +3.4% | −5.7% / −7.4% | — / — | −9.8% / −4.8% |
| CitmCatalog (min) | +288.4% / +298.4% | +73.5% / +78.5% | +72.8% / +78.0% | +103.5% / — | +59.3% / +68.4% |
| CitmCatalog (pretty) | +278.6% / +291.1% | +70.1% / +74.6% | +64.4% / +66.2% | — / — | +71.2% / +55.3% |
| Discord (min) | +1424.4% / +1590.1% | +276.1% / +311.9% | +274.8% / +307.8% | — / +334.8% | +241.3% / +257.3% |
| Discord (pretty) | +1432.1% / +1599.3% | +274.7% / +296.4% | +319.1% / +327.2% | — / — | +237.2% / +249.3% |
| Google Maps (min) | +91.4% / +97.4% | tie / tie | +15.2% / +15.6% | +10.5% / +15.5% | +7.1% / +5.2% |
| Google Maps (pretty) | +80.2% / +87.1% | −5.0% / tie | +21.4% / +15.4% | — / −16.2% | +3.6% / tie |
| Instruments (min) | +618.6% / +653.6% | +133.2% / +139.5% | +142.1% / +147.5% | +135.7% / +130.9% | +124.6% / +130.4% |
| Instruments (pretty) | +610.5% / +635.7% | +123.6% / +127.8% | +124.0% / +128.2% | +139.0% / +150.1% | +120.6% / +111.3% |
| Marine IK (min) | — / — | +35.7% / — | +71.4% / +75.6% | +49.2% / — | — / +41.8% |
| Marine IK (pretty) | — / — | — / — | — / +51.7% | +52.1% / — | — / +16.9% |
| Mesh (min) | +303.8% / +336.0% | +40.1% / +49.0% | +46.4% / +52.4% | +55.2% / +60.4% | +49.5% / +43.4% |
| Mesh (pretty) | +290.7% / +315.2% | +29.4% / +35.1% | +39.9% / +49.6% | +53.0% / +55.5% | +57.7% / +45.7% |
| Random (min) | +196.5% / +220.7% | +27.0% / +23.8% | +22.4% / +24.7% | +21.3% / +23.1% | — / +23.6% |
| Random (pretty) | +190.2% / +216.8% | +24.8% / +28.1% | +19.5% / +22.2% | +21.7% / +22.5% | — / −9.0% |
| Twitter (min) | +649.2% / +719.1% | +217.9% / +220.9% | +128.1% / — | +187.5% / +167.9% | +165.4% / +164.4% |
| Twitter (pretty) | +645.0% / +706.3% | +180.7% / +215.2% | +103.1% / +111.3% | +108.6% / +158.1% | +138.1% / +153.4% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +60.9% / +57.9% | +6.0% / +8.0% | +6.7% / +4.2% | +99.8% / +58.7% | +30.0% / +29.4% |
| Canada (pretty) | +47.8% / +47.9% | +11.5% / +6.2% | +2.1% / −1.3% | — / −18.0% | +5.9% / +6.5% |
| CitmCatalog (min) | +221.5% / +227.8% | +44.9% / +45.8% | +23.8% / +25.6% | — / — | +8.4% / +6.4% |
| CitmCatalog (pretty) | +169.6% / +173.7% | +32.9% / +37.1% | +17.4% / +20.6% | — / — | +9.6% / +13.5% |
| Discord (min) | +8.9% / +12.0% | tie / +7.3% | +9.1% / +6.0% | +13.1% / +8.2% | −7.7% / −9.2% |
| Discord (pretty) | +9.5% / +13.5% | +4.7% / +2.6% | +6.1% / +8.0% | +10.3% / +4.8% | −10.7% / −14.1% |
| Google Maps (min) | +40.7% / +51.0% | tie / +0.9% | — / −3.9% | +10.9% / +12.9% | +1.4% / −7.5% |
| Google Maps (pretty) | +32.8% / +32.5% | +1.5% / tie | +2.0% / +4.7% | +11.7% / +11.6% | −4.2% / −6.7% |
| Instruments (min) | +29.5% / +34.9% | +8.3% / +4.4% | +17.8% / +13.3% | +33.2% / +27.6% | −14.0% / −14.1% |
| Instruments (pretty) | +33.2% / +36.9% | +7.2% / +8.4% | +3.9% / — | — / +41.1% | −14.2% / −18.0% |
| Marine IK (min) | +47.6% / +52.7% | +21.8% / +22.6% | +28.0% / +29.8% | +17.7% / +23.4% | +31.4% / +39.2% |
| Marine IK (pretty) | — / +58.0% | +12.4% / +12.9% | +8.1% / +7.0% | +15.7% / +5.4% | +19.8% / +12.5% |
| Mesh (min) | +148.9% / +155.6% | +9.3% / +11.7% | +11.0% / +16.9% | +10.8% / +12.3% | +20.8% / +6.2% |
| Mesh (pretty) | +134.3% / +141.4% | +6.7% / +9.1% | +8.1% / +7.6% | tie / −4.0% | +3.6% / +7.8% |
| Random (min) | +19.8% / +23.8% | +11.6% / +14.5% | +5.2% / +10.1% | +12.8% / +29.0% | −8.4% / −8.3% |
| Random (pretty) | +19.8% / +23.3% | +13.5% / +16.4% | +3.7% / +7.8% | +13.4% / +20.5% | −7.0% / +18.2% |
| Twitter (min) | +133.8% / +149.2% | +3.0% / +2.5% | +21.5% / +24.5% | +23.1% / +21.7% | −9.2% / −1.6% |
| Twitter (pretty) | +119.3% / +126.9% | +2.4% / −1.5% | +18.8% / +27.6% | +14.1% / +26.3% | −1.6% / −2.0% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +263.5% / +268.6% | +2.4% / +2.0% | +14.4% / +13.5% | — / +33.8% | +13.7% / +14.2% |
| Canada (pretty) | +179.9% / +179.3% | +8.1% / +7.0% | +2.6% / +2.1% | +21.9% / — | +2.5% / tie |
| CitmCatalog (min) | +261.5% / +267.7% | +17.3% / +18.1% | +40.8% / +41.8% | +56.2% / — | +1.7% / +13.5% |
| CitmCatalog (pretty) | +220.2% / +223.5% | +17.2% / +17.5% | +21.7% / +30.2% | — / — | +12.4% / +25.3% |
| Discord (min) | +108.3% / +129.1% | +4.0% / +1.7% | +16.5% / +17.1% | +24.8% / +19.6% | −2.6% / −2.4% |
| Discord (pretty) | +104.0% / +123.6% | +1.3% / −2.4% | +16.3% / +18.3% | — / +18.3% | −6.7% / −2.6% |
| Google Maps (min) | +73.1% / +69.4% | +3.6% / −1.8% | +12.2% / +12.5% | +8.0% / +7.0% | −5.3% / −10.9% |
| Google Maps (pretty) | +56.4% / +61.7% | tie / −1.6% | +13.1% / +15.9% | +13.0% / +27.8% | −9.8% / −7.1% |
| Instruments (min) | +186.2% / +196.9% | +8.7% / +5.2% | +22.0% / +27.3% | +9.9% / +19.0% | −1.1% / −1.2% |
| Instruments (pretty) | +161.4% / +178.4% | +3.2% / +4.8% | +21.8% / +25.0% | +59.5% / +31.8% | −2.3% / −3.0% |
| Marine IK (min) | +226.9% / +228.6% | +43.0% / +33.6% | +59.3% / +59.2% | +49.3% / +37.2% | — / +33.1% |
| Marine IK (pretty) | +179.8% / — | +8.0% / +15.3% | +22.2% / +22.9% | +23.8% / +39.8% | +26.5% / +24.6% |
| Mesh (min) | +150.1% / +163.5% | +8.8% / +8.3% | +11.6% / +13.5% | +11.1% / +6.2% | +12.3% / +21.1% |
| Mesh (pretty) | +134.6% / +142.5% | +6.1% / +8.8% | +12.1% / +10.7% | +4.4% / +9.3% | +52.3% / +39.0% |
| Random (min) | +48.7% / +51.3% | +6.3% / +3.9% | +7.9% / +11.3% | tie / +12.5% | −6.0% / −1.1% |
| Random (pretty) | +45.0% / +50.3% | +3.7% / tie | tie / +7.8% | +21.3% / +13.9% | −5.5% / −6.2% |
| Twitter (min) | +188.8% / +193.8% | +12.2% / +12.4% | +21.1% / +26.4% | +27.4% / +35.2% | −2.8% / +2.3% |
| Twitter (pretty) | +177.8% / +183.1% | +11.2% / +9.0% | +13.8% / +18.0% | +22.7% / +27.6% | −10.1% / tie |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +0.9% / −1.3% | +4.8% / +3.2% | −9.8% / −13.2% | +3.8% / +10.2% | tie / +9.0% |
| Canada (pretty) | +3.2% / tie | — / — | −8.2% / −11.6% | −3.1% / +2.6% | +6.4% / −1.6% |
| CitmCatalog (min) | +15.9% / +20.8% | tie / +5.0% | −8.3% / −4.0% | +9.9% / +15.0% | +2.8% / +5.3% |
| CitmCatalog (pretty) | +10.0% / +15.3% | −2.9% / −2.7% | −4.3% / −4.1% | +15.0% / +10.7% | +15.4% / +3.8% |
| Discord (min) | +6.7% / +24.3% | −7.1% / −1.3% | — / −10.4% | +8.1% / +22.5% | −4.7% / +5.1% |
| Discord (pretty) | +5.5% / +23.1% | −4.9% / −1.8% | +2.8% / +8.6% | +15.4% / +14.3% | +16.1% / +1.2% |
| Google Maps (min) | +10.1% / +12.2% | +15.0% / +17.0% | +11.7% / +19.8% | +16.9% / +17.3% | +1.7% / +1.1% |
| Google Maps (pretty) | tie / +5.0% | — / +13.7% | +11.2% / tie | +22.6% / +8.9% | tie / +11.8% |
| Instruments (min) | +35.4% / +35.2% | +0.9% / +2.2% | tie / −9.3% | +11.8% / +4.5% | +6.9% / +6.5% |
| Instruments (pretty) | +27.8% / +29.4% | −8.5% / −2.8% | −10.1% / −8.3% | +14.1% / +5.8% | tie / −1.7% |
| Marine IK (min) | +3.0% / +5.3% | −3.1% / −4.3% | −2.7% / −5.5% | +14.0% / tie | +9.3% / +5.6% |
| Marine IK (pretty) | +4.3% / +5.4% | −6.2% / −1.5% | −2.2% / −4.8% | +5.2% / +5.5% | +4.4% / +5.1% |
| Mesh (min) | +7.7% / +20.7% | −2.4% / −4.0% | −5.7% / −9.0% | tie / tie | +4.3% / +5.2% |
| Mesh (pretty) | +3.2% / +12.9% | −6.0% / −8.2% | — / −13.3% | +6.0% / −6.7% | −12.0% / +3.4% |
| Random (min) | +7.9% / +11.7% | +14.4% / +12.9% | +12.9% / +19.1% | +34.8% / +21.7% | +6.4% / −3.9% |
| Random (pretty) | +7.8% / +7.4% | +13.1% / +16.1% | +21.2% / +13.6% | +21.7% / +31.9% | +4.2% / −2.3% |
| Twitter (min) | +20.8% / +28.9% | −6.0% / −5.0% | −3.5% / +4.8% | +24.7% / +24.5% | −6.0% / +38.0% |
| Twitter (pretty) | +19.3% / +25.8% | −8.1% / −3.5% | tie / +3.7% | +4.4% / +7.8% | +10.3% / — |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +12.8% / +12.8% | +11.4% / — | −2.1% / −6.0% | +7.3% / +12.4% | +5.4% / +6.9% |
| Canada (pretty) | +14.2% / +14.3% | tie / +7.3% | −3.5% / −4.7% | +6.5% / +8.7% | +4.7% / +9.7% |
| CitmCatalog (min) | +202.5% / +218.1% | +66.4% / +61.5% | +71.7% / +77.0% | +58.2% / +63.1% | +61.2% / +62.0% |
| CitmCatalog (pretty) | +199.9% / +210.7% | +55.6% / +61.5% | +67.8% / +67.5% | +41.8% / +52.7% | +48.0% / +54.3% |
| Discord (min) | +1136.8% / +1345.2% | +261.4% / +302.3% | +280.3% / +311.1% | +290.7% / +351.9% | +249.0% / +288.4% |
| Discord (pretty) | +1101.9% / +1328.9% | +271.0% / +310.8% | +275.8% / +308.0% | +271.8% / +390.7% | +260.6% / +266.4% |
| Google Maps (min) | +88.1% / +104.4% | −1.3% / −2.5% | +15.4% / +13.9% | +13.5% / +13.0% | +1.0% / −5.6% |
| Google Maps (pretty) | +81.4% / +91.1% | −3.0% / −4.5% | +11.3% / +11.6% | +14.4% / +13.7% | −1.1% / tie |
| Instruments (min) | +642.3% / +663.4% | +147.5% / +147.8% | +137.5% / +141.3% | +156.9% / +161.1% | +122.5% / +136.8% |
| Instruments (pretty) | +633.0% / +644.8% | +133.6% / +138.6% | +130.9% / +122.1% | +137.9% / +139.2% | +103.5% / +125.3% |
| Marine IK (min) | +184.9% / +194.2% | +33.2% / +28.1% | +61.2% / +63.2% | +38.9% / +48.1% | +29.5% / +30.5% |
| Marine IK (pretty) | +185.0% / +189.4% | +28.4% / +28.2% | +55.4% / +59.6% | +35.6% / +44.2% | +44.7% / +19.6% |
| Mesh (min) | +120.7% / +186.9% | +22.0% / +18.5% | +24.9% / +35.3% | +34.7% / +40.9% | +30.0% / +33.0% |
| Mesh (pretty) | +110.5% / +182.1% | +15.3% / +12.6% | +23.5% / +26.9% | +28.3% / +19.9% | +23.7% / +29.5% |
| Random (min) | +217.0% / +243.3% | +42.8% / +45.9% | +38.6% / +51.4% | +44.2% / +52.4% | +23.9% / +67.0% |
| Random (pretty) | +215.5% / +230.3% | +42.2% / +51.1% | +41.5% / +49.4% | +32.8% / +53.5% | +17.1% / +37.4% |
| Twitter (min) | +670.9% / +702.6% | +173.6% / +177.1% | +151.3% / +151.2% | +154.0% / +212.9% | — / +114.7% |
| Twitter (pretty) | +656.7% / +717.1% | — / +166.7% | +133.0% / +144.5% | +144.4% / +193.6% | +186.6% / +198.4% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +34.8% / +29.6% | +17.4% / +22.0% | +24.9% / +23.4% | +27.9% / +29.1% | −4.1% / −7.8% |
| Canada (pretty) | +36.4% / +34.3% | +16.1% / +17.8% | +28.5% / +25.8% | +30.6% / +28.5% | +1.3% / +6.3% |
| CitmCatalog (min) | +136.2% / +168.5% | +12.9% / +19.0% | — / +6.5% | +30.4% / +20.6% | +14.1% / +31.4% |
| CitmCatalog (pretty) | +117.2% / +131.8% | +16.8% / +16.7% | +23.6% / +18.7% | +31.2% / +16.8% | +22.1% / +10.3% |
| Discord (min) | +15.5% / +30.3% | +4.1% / −0.8% | — / +8.5% | +13.9% / +17.8% | tie / +3.0% |
| Discord (pretty) | +14.9% / +23.3% | tie / tie | +4.4% / +2.2% | +12.2% / +17.6% | tie / +4.8% |
| Google Maps (min) | +40.3% / +51.7% | −4.4% / tie | −7.4% / −5.7% | +9.3% / +10.2% | −2.2% / +6.3% |
| Google Maps (pretty) | +35.3% / +37.0% | tie / −5.0% | — / −1.2% | +19.3% / +8.3% | −4.1% / −4.3% |
| Instruments (min) | +30.9% / +45.3% | tie / −2.9% | +6.6% / +4.7% | +21.2% / +13.6% | −3.2% / −7.6% |
| Instruments (pretty) | +25.4% / +26.5% | −2.3% / −5.0% | +6.6% / +4.8% | +22.1% / +17.4% | −9.3% / −12.6% |
| Marine IK (min) | +29.9% / +31.0% | +17.2% / +18.2% | +32.9% / +28.8% | +20.5% / +19.2% | +12.3% / +33.0% |
| Marine IK (pretty) | +32.6% / +36.5% | +12.5% / +15.6% | +25.6% / +27.8% | +20.3% / +18.5% | +27.3% / +13.7% |
| Mesh (min) | +90.5% / +142.9% | tie / +2.9% | +10.5% / +13.9% | +6.5% / +7.1% | +10.5% / +17.4% |
| Mesh (pretty) | +69.3% / +133.9% | −1.6% / tie | +5.3% / +2.8% | +7.8% / +6.8% | +4.8% / −4.0% |
| Random (min) | +22.2% / +26.1% | +3.3% / +3.1% | +4.7% / +3.1% | +23.8% / +43.5% | +8.3% / −10.2% |
| Random (pretty) | +21.8% / +26.8% | +9.0% / +7.8% | +4.2% / +6.9% | +39.7% / +16.3% | −11.1% / tie |
| Twitter (min) | +117.2% / +120.6% | −8.6% / −12.7% | +3.5% / +3.3% | +15.9% / +7.4% | +5.8% / tie |
| Twitter (pretty) | +90.0% / +97.9% | — / −8.6% | tie / −1.5% | +12.6% / tie | −20.8% / −1.2% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +148.6% / +153.8% | +26.9% / +19.4% | +34.2% / +35.8% | +30.5% / +24.9% | −12.7% / −12.7% |
| Canada (pretty) | +122.6% / +134.4% | — / +21.9% | +37.4% / +30.1% | +24.7% / +25.5% | +3.0% / tie |
| CitmCatalog (min) | +148.5% / +195.7% | +15.7% / +13.7% | +32.6% / +27.6% | +23.3% / +23.1% | +5.5% / +11.0% |
| CitmCatalog (pretty) | +147.4% / +163.4% | +9.4% / +14.0% | +26.2% / +25.4% | +20.7% / +28.3% | +13.3% / +11.8% |
| Discord (min) | +145.1% / +163.4% | +2.7% / tie | +12.2% / +12.8% | +30.5% / +28.6% | +1.0% / +3.3% |
| Discord (pretty) | +128.0% / +161.4% | tie / +5.0% | +7.8% / +14.3% | +31.3% / +25.9% | tie / +1.2% |
| Google Maps (min) | +66.9% / +72.7% | −5.4% / +3.2% | +8.5% / — | +2.3% / +15.3% | −7.8% / −5.1% |
| Google Maps (pretty) | +65.1% / +65.4% | −2.9% / −2.3% | +4.6% / +5.0% | −5.7% / tie | +9.7% / −7.7% |
| Instruments (min) | +189.2% / +196.5% | +1.3% / — | +23.5% / +29.3% | +46.7% / +37.4% | +8.5% / +6.1% |
| Instruments (pretty) | +166.3% / +194.2% | +1.6% / +2.6% | +18.5% / +12.0% | +30.5% / +33.6% | −5.9% / tie |
| Marine IK (min) | +124.5% / +120.6% | +29.3% / +29.2% | +45.7% / +53.0% | +32.4% / +39.0% | +24.1% / +20.9% |
| Marine IK (pretty) | +109.6% / +101.8% | +21.8% / +22.1% | +48.5% / +44.9% | +41.3% / — | +9.7% / +24.0% |
| Mesh (min) | +103.4% / +159.6% | −8.6% / −5.8% | +7.3% / +9.7% | tie / +5.7% | +9.3% / +7.2% |
| Mesh (pretty) | +88.4% / +131.0% | −7.6% / −6.4% | +3.9% / +6.7% | tie / +10.0% | +1.0% / +1.3% |
| Random (min) | +53.8% / +53.4% | tie / — | +5.8% / — | +6.9% / tie | tie / — |
| Random (pretty) | +53.1% / +49.2% | +5.5% / +1.5% | +8.0% / +7.8% | −7.7% / +34.3% | — / −3.4% |
| Twitter (min) | +154.6% / +162.9% | −4.9% / −4.1% | +9.9% / +8.3% | +19.0% / +49.8% | −62.8% / +6.6% |
| Twitter (pretty) | +135.6% / +147.0% | −4.6% / — | +7.6% / +12.2% | +9.4% / +13.3% | −17.2% / tie |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.0x to 17x faster at full size: +103.1% (Twitter pretty, Linux/GCC, fresh) up to +1599.3% (Discord pretty, MSVC, reused). The small documents show the same pattern, from +103.5% to +1345.2%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the margin runs from −16.2% to +21.4%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are a strength.** Jsonifier won 145 of 172 converged full-size sparse-reverse results across both allocation modes (6 ties, 21 losses), and 138 of 171 small ones (12 ties, 21 losses). MSVC won all 71 of its sparse-reverse results, and Linux/GCC won all of its own except one tie. All 21 full-size losses are on macOS/Clang (18) and Linux/Clang (3).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 32.4 MB/s on MSVC against 215.9 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 495.8 and 809.1 MB/s. Every converged MSVC reverse result is a win, by +80.2% or more outside Canada (+12.8% to +20.4%); every MSVC sparse result is a win, by +8.9% to +227.8%, and every sparse-reverse result by +45.0% to +268.6%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 144 of 171 converged full-size sparse results (4 ties, 23 losses) and 135 of 176 small ones (13 ties, 28 losses). MSVC won every one, at both sizes. On full-size documents, 18 of the 23 losses are on macOS/Clang.
- **In-order full reads are mostly wins at full size.** Across 165 converged full-size in-order results, Jsonifier won 119, tied 10 and lost 36. MSVC won 31 and tied 4. Linux/GCC split its in-order tests 15-0-17 across the two modes, and macOS/Clang 16-3-12.
- **Small documents read in order are the weakest category.** Across 174 converged results, Jsonifier won 112, tied 12 and lost 50. MSVC (33-2-1) and macOS/GCC (31-3-2) won almost all of theirs, but Linux/Clang went 12-1-20 and Linux/GCC 11-3-20. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The losses there are mostly modest (−1.3% to −13.3%), while the in-order small-document wins on the same builds, on Google Maps and Random, run from +11.2% to +21.2%.
- **In-order losses at full size cluster on number-heavy documents.** Mesh, Canada and Marine IK lose in order on Linux/GCC (−3.4% to −9.7%), and Mesh also on macOS/GCC. These documents are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. The other Linux/GCC losses are Instruments (−3.1% to −4.3%) and Twitter (−0.9% to −15.3%).
- **The number POD tests win on every build.** Double wins on all five builds in both modes (+29.9% to +80.9%), Int64 on every converged result (+20.2% to +204.8%), and Uint64 on all ten (+13.6% to +90.3%). The six POD losses are String on MSVC (both modes), macOS/GCC (fresh, −1.2%) and macOS/Clang (−12.3% and −16.3%), and Bool on macOS/Clang (fresh, −2.3%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 2.8% to 17.3% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 22 / 26 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 25 / 26 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 24 / 26 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 11 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 7 / 26 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 6-0-0 | 22-0-0 |
| Linux / Clang | 4-1-0 | 10-0-0 | 9-0-1 | 23-1-1 |
| Linux / GCC | 5-1-0 | 7-0-2 | 7-0-2 | 19-1-4 |
| macOS / GCC | 4-1-0 | 5-0-0 | 1-0-0 | 10-1-0 |
| macOS / Clang | 1-2-1 | 1-0-1 | 0-1-0 | 2-3-2 |
| **All** | **20-5-1** | **33-0-3** | **23-1-3** | **76-6-7** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +36.9% | +3.8% | +12.6% | +29.6% | tie |
| Large Amazon Cellphones (NDJSON) | +31.0% | +9.7% | tie | — | — |
| Stream Formats Small (NDJSON) | +23.8% | — | +16.5% | +4.4% | −18.0% |
| Stream Formats Small (comma) | +88.4% | +73.6% | +85.6% | +82.3% | +73.5% |
| Stream Formats Large (NDJSON) | +18.5% | +6.0% | +26.2% | tie | — |
| Stream Formats Large (comma) | +25.3% | tie | +36.3% | +14.8% | tie |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +25.9% | +3.6% | +11.4% | +7.7% | — |
| CitmCatalog (comma) | +22.3% | +12.1% | +20.0% | +13.5% | — |
| Google Maps (NDJSON) | +10.2% | +23.4% | +11.2% | +7.2% | — |
| Google Maps (comma) | +8.5% | +31.7% | +20.4% | — | −11.9% |
| Instruments (NDJSON) | +36.1% | +10.0% | −7.8% | — | — |
| Instruments (comma) | +41.8% | +24.0% | +14.0% | +33.3% | — |
| Random (NDJSON) | +7.3% | +26.8% | +12.1% | — | — |
| Random (comma) | +12.4% | +36.0% | +28.5% | — | — |
| Twitter (NDJSON) | +29.5% | +8.1% | −11.5% | — | +26.5% |
| Twitter (comma) | +22.4% | +13.0% | — | +26.5% | — |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | — | +46.3% | +39.5% | — | — |
| CitmCatalog (comma) | — | +62.5% | +60.3% | — | — |
| Google Maps (NDJSON) | +42.6% | +2.4% | +5.6% | +7.4% | — |
| Google Maps (comma) | +41.7% | +9.0% | +18.7% | — | — |
| Instruments (NDJSON) | +52.1% | −14.4% | −21.0% | — | — |
| Instruments (comma) | +65.4% | +5.3% | −5.5% | — | tie |
| Random (NDJSON) | +219.0% | +58.3% | +32.8% | — | — |
| Random (comma) | +230.2% | +69.0% | +51.1% | — | — |
| Twitter (NDJSON) | — | +235.8% | +135.7% | — | — |
| Twitter (comma) | — | +245.7% | — | — | — |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 20 of the 26 converged results in that group, tied 5 and lost 1. It won Amazon Cellphones on four builds (+3.8% to +36.9%) and tied on macOS/Clang, and it won Stream Formats Small (comma) on all five builds (+73.5% to +88.4%). The one loss is Stream Formats Small (NDJSON) on macOS/Clang (−18.0%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test that converged,** 22-0-0, and Linux/Clang lost one of 25.
- **In-order corpus streams are mostly wins.** Of the 36 converged in-order corpus results, Jsonifier won 33 and lost 3: Instruments (NDJSON, −7.8%) and Twitter (NDJSON, −11.5%) on Linux/GCC, and Google Maps (comma) on macOS/Clang (−11.9%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 23 of 27 converged reverse stream results and tied one. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +135.7% to +245.7%, while Google Maps rows, which have few, give +2.4% to +42.6%. All three losses are Instruments: NDJSON on Linux/Clang (−14.4%) and Linux/GCC (−21.0%), and comma on Linux/GCC (−5.5%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 121.8 MB/s under MSVC and 388.5 MB/s under Linux/Clang, while Jsonifier reads it at 402.3 and 656.6 MB/s.

## 9. Limitations

- **macOS/Clang is the weakest build.** It holds 86 of the 204 single-document losses, spread over every access pattern, and the largest loss in the suite, Twitter Small Sparse Reverse (min, fresh) at −62.8%, whose reused run is a +6.6% win. It also converged on only 7 of its 26 streaming tests. It ran on a virtualized M1, so part of these results likely reflects host noise, but its losses are not confined to one access pattern.
- **Small documents read in order lose on Linux.** Linux/Clang lost 20 of its 33 converged small in-order results and Linux/GCC 20 of 34, by up to −13.3%, while both builds win most full-size in-order tests. At 5 KiB per document, Jsonifier's fixed per-parse cost is the next target.
- **Linux/GCC** is the weakest x86 build for full-size in-order reads, with 17 losses across the two allocation modes on Canada, Instruments, Marine IK, Mesh and Twitter; the largest is Twitter (pretty, fresh) at −15.3%. It also loses reverse reads of Canada at both sizes (−2.1% to −8.6%), while it won or tied every sparse-reverse result at both sizes.
- **Number-heavy documents in order** remain a common loss. Mesh, Marine IK and Canada lose in order on Linux/GCC, and Mesh on macOS/GCC. The cause is float-array throughput rather than out-of-order access, so it is out of scope for the field index.
- **Instruments reverse streams** lose on both Linux builds in NDJSON form (−14.4% and −21.0%), although the single-document Instruments Reverse tests win on every build that converged.
- **Convergence is uneven.** macOS/GCC met the convergence limits on 127 of its 154 full-size single-document results but only 11 of its 26 streaming tests, and macOS/Clang on only 7 streaming tests, even with the looser NEON limits. Full-size Marine IK Reverse did not converge on MSVC in either mode.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. On the x86 in-order corpus streams this leaves comma mode 2.8% to 17.3% slower than NDJSON mode on the same records. The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commit.** The byte-for-byte comparison in §6 ran on `3e7bf6d`, not on `f356735`, and it does not cover the small-document or reused tests.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,410 converged single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,141, tied 65 and lost 204. On reverse reads it won 308 of 333 at both sizes, and on Windows/MSVC it lost 3 single-document results of 291. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (566-34-99 fresh, 575-31-105 reused). The main remaining targets are macOS/Clang, with 86 of the 204 losses, and small documents read in order on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. The one lesson it added repeats §5's: finding every document's end up front makes an in-order reader pay for two passes over the tape, and letting the reader's own cursor report where each document ended removes that pass for whitespace-delimited streams. Across 89 converged streaming results, Jsonifier won 76, tied 6 and lost 7, and it won every converged streaming test on Windows/MSVC. Instruments reverse NDJSON streams on Linux are the main remaining streaming target.

The main engineering lesson is in §5. An index over the whole document made the target workload 2.3x *slower*, because most objects are small. The version that works builds the index lazily, per object, and only once that object has shown both that it is large and that it is being read out of order: enough skipped keys to clear *T*, and two lookups that had to wrap around. In short: make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.