# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model — a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2) — has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization — so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary, skipping the tape wins on POD-type data on every build, but for real documents the faster path depends on the compiler and on whether the input is indented.

This paper describes both halves of that architecture: the single-pass primary path, and Jsonifier's stage-1 implementation — including its per-compiler-tuned step geometry, its fold-expression drain architecture for bitmask-to-index extraction, and its distributed UTF-8 validation strategy — with direct comparisons to simdjson's current design throughout.

---

## 1. Background: the canonical two-stage model

simdjson's pipeline is well documented. Stage 1 sweeps the input in 64-byte blocks, computing per-block bitmasks: backslashes, odd-length escape terminations, quotes, an in-string range mask derived via carry-less multiplication (prefix XOR), structural characters (`{ } [ ] : ,`), whitespace, and pseudo-structural scalar starts. The surviving bits are converted to byte offsets — the *structural tape* — via a tzcnt/blsr extraction loop. Stage 2 then walks the tape to build a DOM or, in the On Demand API, lazily materializes only the values the caller touches.

The model's strength is that stage 1 is branch-free and data-parallel; its cost is that it is unconditional. Every byte of every document is classified and indexed before a single value is parsed, regardless of whether the caller's access pattern needs the index at all.

## 2. The Jsonifier position: pay for the tape only when the tape pays for you

Jsonifier's primary API parses JSON directly into user-defined structs registered through compile-time reflection (`jsonifier::core<T>`). In that setting the parser already knows, at compile time, the expected shape of the document: which keys exist, their serialized order, their types. The parse loop is a schema-directed walk, not a blind tree construction.

Consider what the structural tape provides: the positions of every brace, bracket, colon, comma, quote, and scalar start. Now consider what a schema-directed single-pass parser does at each of those same positions: it is *already there*, with its iterator parked on that exact byte, consuming it as part of matching a known key literal or delimiting a value. The tape's information is a strict subset of what the fused pass discovers for free in program order. Building it first means that, for a full-document parse in which every value is materialized:

- every byte of the input is loaded twice (once in stage 1, once in stage 2),
- the tape itself is written and re-read (cache traffic proportional to structural density), and
- stage 2's control flow is driven by indirect loads through the index array rather than by a pointer walking linearly through memory the prefetcher already understands.

(We scope the double-load claim deliberately: a two-stage consumer that *skips* content — simdjson's On Demand API skipping unrequested fields — does not reload the skipped bytes in its second pass. That is exactly the workload where the tape earns its keep, and exactly why Jsonifier retains the two-stage machinery for partial reading. The accounting above describes the full-materialization case, where nothing is skipped and the tape's information is fully redundant with the walk.)

For full-document parsing into known types, the tape is overhead in principle, but whether skipping it is faster in practice turns out to depend on the compiler and on the shape of the input. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. In the freshly allocated run, the fused path wins every POD-type test, all 32 converged minified documents on Linux and macOS, and 10 of the 12 converged prettified documents on the M1 (one more is a tie). The two-stage path wins 25 of the 30 prettified documents on x86, and five of the ten minified documents under MSVC.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 6–7, 2026, with Jsonifier [6733e6d](https://github.com/nihilai-collective/jsonifier/commit/6733e6d) ([9d1ff54](https://github.com/nihilai-collective/jsonifier/commit/9d1ff54) on Windows / MSVC), simdjson [e9cdb87](https://github.com/simdjson/simdjson/commit/e9cdb87) ([ed60f06](https://github.com/simdjson/simdjson/commit/ed60f06) on macOS / GCC) and BenchmarkSuite [c4f600a](https://github.com/nihilai-collective/benchmarksuite/commit/c4f600a). The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep but is left out here, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3 and §2.5 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document, which §2.6 covers on its own, and "Small" cut-down copies of each document (at most 5 KiB minified) that measure per-call overhead; neither is counted in §2.2 to §2.5.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on x86 (10% and 5% on the virtualized M1), and a test is ranked only if every library in it converges, which is why some platforms have fewer than 25 tests. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to every library. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced one tie between the two Jsonifier paths across 109 freshly allocated tests and three across 110 reused ones.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 77 tests, 1 is a statistical tie, and the two-stage path wins 31. The split is almost entirely a matter of platform:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 11 | 0 | 14 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 17 | 0 | 8 | 25 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 16 | 0 | 8 | 24 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 16 | 1 | 0 | 17 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 17 | 0 | 1 | 18 of 25 |
| **Aggregate** | **77** | **1** | **31** | **109 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 5 / 0 / 5 | 19 / 0 / 0 | 13 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 9 | 4 / 0 / 16 | 10 / 1 / 1 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 23 / 1 / 1 | 20 / 0 / 5 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 23 / 1 / 0 | 22 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 17 / 0 / 0 | 17 / 0 / 0 |
| macOS / Clang 23.1 (Apple M1, NEON) | 18 / 0 / 0 | 17 / 1 / 0 |
| **Aggregate** | **106 / 2 / 1** | **100 / 1 / 8** |

**Reused objects.** With the target object held across iterations, the picture is the same:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 11 | 1 | 13 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 18 | 0 | 7 | 25 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 13 | 1 | 7 | 21 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 17 | 1 | 2 | 20 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 16 | 0 | 3 | 19 of 25 |
| **Aggregate** | **75** | **3** | **32** | **110 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 9 / 0 / 0 |
| Minified corpus documents | 5 / 1 / 4 | 18 / 0 / 0 | 13 / 0 / 1 |
| Prettified corpus documents | 1 / 0 / 9 | 3 / 1 / 14 | 11 / 1 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 23 / 2 / 0 | 20 / 0 / 5 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 19 / 0 / 2 | 17 / 2 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 20 / 0 / 0 | 20 / 0 / 0 |
| macOS / Clang 23.1 (Apple M1, NEON) | 19 / 0 / 0 | 18 / 0 / 1 |
| **Aggregate** | **106 / 2 / 2** | **99 / 2 / 9** |

Across 110 converged tests the fused path wins 75, ties 3 and loses 32 to the two-stage path, and the platform split barely moves: MSVC is 11 / 1 / 13, while macOS/Clang, with fewer tests converging, goes 16 / 0 / 3. Against simdjson, the faster Jsonifier path wins 106, ties 2 and loses 2, and the fused path alone wins 99 and loses 9. Taking allocation out of the timed region helps every library, so it moves individual verdicts, but it does not change which path is faster on a given platform and input shape.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput, and "n/c" means the test did not converge for at least one library.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 498 | 138 | 127 | 3.60× | Fused | Win |
| Double (POD) | 581 | 301 | 170 | 1.93× | Fused | Win |
| Int64 (POD) | 1,808 | 674 | 385 | 2.68× | Fused | Win |
| String (POD) | 1,003 | 708 | 657 | 1.42× | Fused | Win |
| Uint64 (POD) | 2,168 | 743 | 389 | 2.92× | Fused | Win |
| Canada (minified) | 577 | 536 | 351 | 1.08× | Fused | Win |
| Canada (prettified) | 1,557 | 1,660 | 1,076 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 944 | 991 | 506 | 0.95× | Two-stage | Win |
| CitmCatalog (prettified) | 1,934 | 2,519 | 1,378 | 0.77× | Two-stage | Win |
| Discord (minified) | 1,001 | 922 | 584 | 1.09× | Fused | Win |
| Discord (prettified) | 1,325 | 1,505 | 970 | 0.88× | Two-stage | Win |
| Google Maps Response (minified) | 1,054 | 965 | 520 | 1.09× | Fused | Win |
| Google Maps Response (prettified) | 2,227 | 2,170 | 1,282 | 1.03× | Fused | Win |
| Instruments (minified) | 871 | 912 | 684 | 0.95× | Two-stage | Win |
| Instruments (prettified) | 1,407 | 1,692 | 1,266 | 0.83× | Two-stage | Win |
| Marine IK Reverse (minified) | 375 | 387 | 36 | 0.97× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,581 | 1,932 | 194 | 0.82× | Two-stage | Win |
| Marine IK (minified) | 381 | 392 | 257 | 0.97× | Two-stage | Win |
| Marine IK (prettified) | 1,582 | 1,954 | 1,249 | 0.81× | Two-stage | Win |
| Mesh (minified) | 497 | 559 | 422 | 0.89× | Two-stage | Win |
| Mesh (prettified) | 726 | 1,031 | 792 | 0.70× | Two-stage | Win |
| Random (minified) | 808 | 793 | 532 | 1.02× | Fused | Win |
| Random (prettified) | 1,197 | 1,403 | 976 | 0.85× | Two-stage | Win |
| Twitter (minified) | 1,183 | 1,079 | 776 | 1.10× | Fused | Win |
| Twitter (prettified) | 1,362 | 1,576 | 1,168 | 0.86× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,093 | 370 | 216 | 5.65× | Fused | Win |
| Double (POD) | 1,287 | 455 | 277 | 2.83× | Fused | Win |
| Int64 (POD) | 3,186 | 1,046 | 565 | 3.05× | Fused | Win |
| String (POD) | 1,662 | 1,140 | 1,120 | 1.46× | Fused | Win |
| Uint64 (POD) | 3,546 | 1,039 | 589 | 3.41× | Fused | Win |
| Canada (minified) | 1,026 | 883 | 643 | 1.16× | Fused | Win |
| Canada (prettified) | 2,621 | 2,498 | 1,925 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,340 | 1,774 | 1,203 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 3,598 | 4,225 | 3,050 | 0.85× | Two-stage | Win |
| Discord (minified) | 2,004 | 1,732 | 1,456 | 1.16× | Fused | Win |
| Discord (prettified) | 1,928 | 2,666 | 2,281 | 0.72× | Two-stage | Win |
| Google Maps Response (minified) | 2,299 | 922 | 583 | 2.49× | Fused | Win |
| Google Maps Response (prettified) | 1,813 | 2,187 | 1,410 | 0.83× | Two-stage | Win |
| Instruments (minified) | 1,362 | 859 | 806 | 1.59× | Fused | Win |
| Instruments (prettified) | 1,257 | 1,411 | 1,438 | 0.89× | Two-stage | **Loss** |
| Marine IK Reverse (minified) | 856 | 734 | 572 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 3,420 | 3,302 | 2,748 | 1.04× | Fused | Win |
| Marine IK (minified) | 859 | 739 | 593 | 1.16× | Fused | Win |
| Marine IK (prettified) | 3,218 | 3,338 | 2,787 | 0.96× | Two-stage | Win |
| Mesh (minified) | 1,390 | 1,174 | 1,096 | 1.18× | Fused | Win |
| Mesh (prettified) | 1,975 | 2,151 | 2,010 | 0.92× | Two-stage | Win |
| Random (minified) | 1,435 | 1,366 | 1,077 | 1.05× | Fused | Win |
| Random (prettified) | 1,792 | 1,964 | 1,941 | 0.91× | Two-stage | Tie |
| Twitter (minified) | 1,796 | 1,747 | 1,656 | 1.03× | Fused | Win |
| Twitter (prettified) | 1,871 | 2,477 | 2,391 | 0.76× | Two-stage | Win |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,217 | 261 | 178 | 8.51× | Fused | Win |
| Double (POD) | 1,194 | 365 | 226 | 3.27× | Fused | Win |
| Int64 (POD) | 3,376 | 813 | 508 | 4.15× | Fused | Win |
| String (POD) | 1,720 | 1,009 | 970 | 1.70× | Fused | Win |
| Uint64 (POD) | 3,278 | 838 | 505 | 3.91× | Fused | Win |
| Canada (minified) | 901 | 888 | 751 | 1.01× | Fused | Win |
| Canada (prettified) | 2,393 | 2,567 | 2,205 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,231 | 1,660 | 1,329 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 3,727 | 4,059 | 3,339 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,062 | 1,626 | 1,373 | 1.27× | Fused | Win |
| Discord (prettified) | 2,235 | 2,551 | 2,197 | 0.88× | Two-stage | Win |
| Google Maps Response (minified) | 2,374 | 1,727 | 1,092 | 1.37× | Fused | Win |
| Google Maps Response (prettified) | 3,649 | 3,759 | 2,441 | 0.97× | Two-stage | Win |
| Instruments (minified) | 3,092 | 2,146 | 1,352 | 1.44× | Fused | Win |
| Instruments (prettified) | 3,045 | 3,443 | 2,505 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 782 | 714 | 584 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 3,174 | 3,031 | 2,571 | 1.05× | Fused | Win |
| Marine IK (minified) | 780 | 723 | 573 | 1.08× | Fused | Win |
| Marine IK (prettified) | 3,087 | 3,279 | 2,689 | 0.94× | Two-stage | Win |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | 1,664 | 1,999 | 2,022 | 0.83× | Two-stage | Tie |
| Random (minified) | 1,346 | 1,280 | 844 | 1.05× | Fused | Win |
| Random (prettified) | 1,834 | 1,744 | 1,487 | 1.05× | Fused | Win |
| Twitter (minified) | 1,866 | 1,575 | 1,280 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,755 | 2,127 | 1,862 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,524 | 238 | 189 | 6.41× | Fused | Win |
| Double (POD) | 815 | 288 | 142 | 2.83× | Fused | Win |
| Int64 (POD) | 2,290 | 610 | 468 | 3.75× | Fused | Win |
| String (POD) | 1,082 | 692 | 781 | 1.56× | Fused | Win |
| Uint64 (POD) | 2,274 | 618 | 453 | 3.68× | Fused | Win |
| Canada (minified) | 690 | 655 | 474 | 1.05× | Fused | Win |
| Canada (prettified) | 1,781 | 1,742 | 1,355 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 1,916 | 1,282 | 1,069 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | 3,261 | 2,874 | 2,414 | 1.13× | Fused | Win |
| Discord (minified) | 1,331 | 996 | 819 | 1.34× | Fused | Win |
| Discord (prettified) | 1,925 | 1,480 | 1,275 | 1.30× | Fused | Win |
| Google Maps Response (minified) | 1,662 | 1,167 | 607 | 1.42× | Fused | Win |
| Google Maps Response (prettified) | 2,458 | 1,957 | 1,488 | 1.26× | Fused | Win |
| Instruments (minified) | n/c | n/c | n/c | — | — | — |
| Instruments (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 931 | 837 | 682 | 1.11× | Fused | Win |
| Mesh (prettified) | n/c | n/c | n/c | — | — | — |
| Random (minified) | 813 | 628 | 417 | 1.30× | Fused | Win |
| Random (prettified) | n/c | n/c | n/c | — | — | — |
| Twitter (minified) | 1,676 | 1,446 | 1,148 | 1.16× | Fused | Win |
| Twitter (prettified) | 1,841 | 1,885 | 1,493 | 0.98× | Tie | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,100 | 207 | 170 | 5.30× | Fused | Win |
| Double (POD) | 816 | 243 | 166 | 3.36× | Fused | Win |
| Int64 (POD) | 1,930 | 540 | 413 | 3.57× | Fused | Win |
| String (POD) | 840 | 537 | 725 | 1.56× | Fused | Win |
| Uint64 (POD) | 1,993 | 554 | 425 | 3.60× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 1,629 | 1,153 | 656 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 2,961 | 2,580 | 1,827 | 1.15× | Fused | Win |
| Discord (minified) | n/c | n/c | n/c | — | — | — |
| Discord (prettified) | 2,384 | 1,712 | 1,742 | 1.39× | Fused | Win |
| Google Maps Response (minified) | 1,730 | 1,029 | 824 | 1.68× | Fused | Win |
| Google Maps Response (prettified) | 2,763 | 2,126 | 2,122 | 1.30× | Fused | Win |
| Instruments (minified) | n/c | n/c | n/c | — | — | — |
| Instruments (prettified) | 3,021 | 2,651 | 2,234 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 646 | 534 | 397 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 1,094 | 898 | 785 | 1.22× | Fused | Win |
| Mesh (prettified) | 1,444 | 1,567 | 1,441 | 0.92× | Two-stage | Win |
| Random (minified) | 1,480 | 941 | 893 | 1.57× | Fused | Win |
| Random (prettified) | 2,051 | 1,341 | 1,563 | 1.53× | Fused | Win |
| Twitter (minified) | 2,046 | 1,544 | 1,451 | 1.32× | Fused | Win |
| Twitter (prettified) | 2,207 | 2,153 | 2,146 | 1.03× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 537 | 140 | 131 | 3.85× | Fused | Win |
| Double (POD) | 692 | 325 | 175 | 2.13× | Fused | Win |
| Int64 (POD) | 2,246 | 732 | 399 | 3.07× | Fused | Win |
| String (POD) | 1,771 | 1,032 | 875 | 1.72× | Fused | Win |
| Uint64 (POD) | 2,954 | 800 | 410 | 3.69× | Fused | Win |
| Canada (minified) | 700 | 649 | 389 | 1.08× | Fused | Win |
| Canada (prettified) | 1,797 | 1,956 | 1,208 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,039 | 1,115 | 552 | 0.93× | Two-stage | Win |
| CitmCatalog (prettified) | 2,070 | 2,755 | 1,465 | 0.75× | Two-stage | Win |
| Discord (minified) | 1,369 | 1,215 | 760 | 1.13× | Fused | Win |
| Discord (prettified) | 1,686 | 1,946 | 1,205 | 0.87× | Two-stage | Win |
| Google Maps Response (minified) | 1,187 | 1,108 | 548 | 1.07× | Fused | Win |
| Google Maps Response (prettified) | 2,398 | 2,322 | 1,323 | 1.03× | Fused | Win |
| Instruments (minified) | 1,026 | 1,026 | 756 | 1.00× | Tie | Win |
| Instruments (prettified) | 1,516 | 1,827 | 1,354 | 0.83× | Two-stage | Win |
| Marine IK Reverse (minified) | 403 | 418 | 37 | 0.96× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,659 | 2,017 | 195 | 0.82× | Two-stage | Win |
| Marine IK (minified) | 404 | 421 | 271 | 0.96× | Two-stage | Win |
| Marine IK (prettified) | 1,659 | 2,036 | 1,339 | 0.81× | Two-stage | Win |
| Mesh (minified) | 515 | 577 | 450 | 0.89× | Two-stage | Win |
| Mesh (prettified) | 742 | 1,057 | 847 | 0.70× | Two-stage | Win |
| Random (minified) | 1,089 | 1,046 | 640 | 1.04× | Fused | Win |
| Random (prettified) | 1,538 | 1,846 | 1,155 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,644 | 1,466 | 966 | 1.12× | Fused | Win |
| Twitter (prettified) | 1,715 | 2,105 | 1,448 | 0.81× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,276 | 370 | 236 | 6.16× | Fused | Win |
| Double (POD) | 1,399 | 470 | 269 | 2.98× | Fused | Win |
| Int64 (POD) | 3,607 | 1,062 | 586 | 3.40× | Fused | Win |
| String (POD) | 2,512 | 1,430 | 1,453 | 1.76× | Fused | Win |
| Uint64 (POD) | 3,929 | 1,068 | 575 | 3.68× | Fused | Win |
| Canada (minified) | 1,269 | 1,063 | 749 | 1.19× | Fused | Win |
| Canada (prettified) | 3,026 | 2,939 | 2,230 | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,895 | 2,111 | 1,421 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 4,222 | 4,849 | 3,484 | 0.87× | Two-stage | Win |
| Discord (minified) | 2,803 | 2,378 | 1,858 | 1.18× | Fused | Win |
| Discord (prettified) | 2,354 | 3,564 | 2,892 | 0.66× | Two-stage | Win |
| Google Maps Response (minified) | 2,548 | 2,171 | 1,501 | 1.17× | Fused | Win |
| Google Maps Response (prettified) | 2,062 | 2,313 | 1,582 | 0.89× | Two-stage | Win |
| Instruments (minified) | 1,704 | 921 | 1,057 | 1.85× | Fused | Win |
| Instruments (prettified) | 1,336 | 1,576 | 1,585 | 0.85× | Two-stage | Tie |
| Marine IK Reverse (minified) | 953 | 815 | 647 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 3,726 | 3,545 | 3,043 | 1.05× | Fused | Win |
| Marine IK (minified) | 978 | 814 | 654 | 1.20× | Fused | Win |
| Marine IK (prettified) | 3,670 | 3,478 | 2,938 | 1.06× | Fused | Win |
| Mesh (minified) | 1,480 | 1,248 | 1,229 | 1.19× | Fused | Win |
| Mesh (prettified) | 2,042 | 2,267 | 2,259 | 0.90× | Two-stage | Tie |
| Random (minified) | 2,123 | 1,952 | 1,417 | 1.09× | Fused | Win |
| Random (prettified) | 2,287 | 3,251 | 2,431 | 0.70× | Two-stage | Win |
| Twitter (minified) | 2,245 | 2,188 | 2,054 | 1.03× | Fused | Win |
| Twitter (prettified) | 2,156 | 3,287 | 2,911 | 0.66× | Two-stage | Win |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,363 | 262 | 181 | 9.02× | Fused | Win |
| Double (POD) | 1,288 | 371 | 228 | 3.47× | Fused | Win |
| Int64 (POD) | 3,679 | 826 | 510 | 4.45× | Fused | Win |
| String (POD) | 2,536 | 1,236 | 1,218 | 2.05× | Fused | Win |
| Uint64 (POD) | 3,590 | 866 | 476 | 4.14× | Fused | Win |
| Canada (minified) | 1,085 | 1,072 | 895 | 1.01× | Fused | Win |
| Canada (prettified) | 2,797 | 3,028 | 2,602 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,801 | 2,074 | 1,580 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 4,170 | 4,636 | 3,951 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,807 | 2,139 | 1,774 | 1.31× | Fused | Win |
| Discord (prettified) | 2,724 | 3,094 | 2,692 | 0.88× | Two-stage | Win |
| Google Maps Response (minified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (prettified) | 3,972 | 4,004 | 2,724 | 0.99× | Tie | Win |
| Instruments (minified) | 3,694 | 2,297 | 1,456 | 1.61× | Fused | Win |
| Instruments (prettified) | 3,303 | 3,813 | 2,670 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 864 | 779 | 644 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | 877 | 789 | 637 | 1.11× | Fused | Win |
| Marine IK (prettified) | 3,401 | 3,563 | 2,935 | 0.95× | Two-stage | Win |
| Mesh (minified) | 1,136 | 1,104 | 1,268 | 1.03× | Fused | **Loss** |
| Mesh (prettified) | 1,732 | 2,036 | 2,344 | 0.85× | Two-stage | **Loss** |
| Random (minified) | n/c | n/c | n/c | — | — | — |
| Random (prettified) | n/c | n/c | n/c | — | — | — |
| Twitter (minified) | 2,362 | 1,929 | 1,508 | 1.22× | Fused | Win |
| Twitter (prettified) | 2,205 | 2,707 | 2,185 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,631 | 236 | 190 | 6.90× | Fused | Win |
| Double (POD) | 1,139 | 353 | 141 | 3.23× | Fused | Win |
| Int64 (POD) | 2,876 | 679 | 484 | 4.24× | Fused | Win |
| String (POD) | 1,690 | 914 | 1,072 | 1.85× | Fused | Win |
| Uint64 (POD) | 2,851 | 656 | 454 | 4.35× | Fused | Win |
| Canada (minified) | 860 | 809 | 552 | 1.06× | Fused | Win |
| Canada (prettified) | 2,141 | 2,092 | 1,553 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,406 | 1,489 | 1,197 | 1.62× | Fused | Win |
| CitmCatalog (prettified) | 3,742 | 3,232 | 2,651 | 1.16× | Fused | Win |
| Discord (minified) | 2,001 | 1,389 | 1,057 | 1.44× | Fused | Win |
| Discord (prettified) | 2,240 | 1,839 | 1,389 | 1.22× | Fused | Win |
| Google Maps Response (minified) | 1,633 | 1,301 | 687 | 1.26× | Fused | Win |
| Google Maps Response (prettified) | 2,961 | 2,493 | 1,480 | 1.19× | Fused | Win |
| Instruments (minified) | n/c | n/c | n/c | — | — | — |
| Instruments (prettified) | 2,844 | 2,502 | 1,834 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | 2,301 | 2,347 | 1,439 | 0.98× | Tie | Win |
| Marine IK (minified) | 691 | 539 | 425 | 1.28× | Fused | Win |
| Marine IK (prettified) | 2,454 | 2,257 | 1,810 | 1.09× | Fused | Win |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | n/c | n/c | n/c | — | — | — |
| Random (minified) | 1,060 | 1,136 | 630 | 0.93× | Two-stage | Win |
| Random (prettified) | n/c | n/c | n/c | — | — | — |
| Twitter (minified) | 1,933 | 1,670 | 1,234 | 1.16× | Fused | Win |
| Twitter (prettified) | 2,133 | 2,408 | 1,686 | 0.89× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,189 | 221 | 169 | 5.39× | Fused | Win |
| Double (POD) | 980 | 309 | 160 | 3.17× | Fused | Win |
| Int64 (POD) | 2,333 | 604 | 418 | 3.86× | Fused | Win |
| String (POD) | n/c | n/c | n/c | — | — | — |
| Uint64 (POD) | 2,347 | 477 | 410 | 4.92× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 1,936 | 1,246 | 759 | 1.55× | Fused | Win |
| CitmCatalog (prettified) | 3,391 | 3,029 | 2,194 | 1.12× | Fused | Win |
| Discord (minified) | 2,117 | 1,289 | 1,437 | 1.64× | Fused | Win |
| Discord (prettified) | 2,709 | 1,940 | 2,291 | 1.40× | Fused | Win |
| Google Maps Response (minified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (prettified) | 3,168 | 2,202 | 2,045 | 1.44× | Fused | Win |
| Instruments (minified) | 2,660 | 1,619 | 1,249 | 1.64× | Fused | Win |
| Instruments (prettified) | 2,979 | 2,512 | 2,175 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | 605 | 564 | 391 | 1.07× | Fused | Win |
| Marine IK (prettified) | 2,210 | 2,479 | 1,939 | 0.89× | Two-stage | Win |
| Mesh (minified) | 1,166 | 945 | 857 | 1.23× | Fused | Win |
| Mesh (prettified) | 1,511 | 1,642 | 1,556 | 0.92× | Two-stage | Win |
| Random (minified) | 1,793 | 1,265 | 994 | 1.42× | Fused | Win |
| Random (prettified) | 2,356 | 1,894 | 1,720 | 1.24× | Fused | Win |
| Twitter (minified) | 2,641 | 1,999 | 1,776 | 1.32× | Fused | Win |
| Twitter (prettified) | 2,631 | 2,650 | 2,471 | 0.99× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: the platform, and whether the document is indented. On the M1 the fused path wins all POD and minified input, and most prettified input. On Linux it wins POD and all minified input, and sixteen of the twenty prettified documents go to the tape. Under MSVC it loses half of the minified documents and nine of the ten prettified ones.

**POD-type tests: the fused path wins all 25 freshly allocated and all 24 reused, on every build.** It runs them 1.4× (String, Windows/MSVC) to 8.5× (Bool, Linux/GCC) faster than the two-stage path, and 1.7× to 9.0× faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the compiler.

**Minified documents: the fused path's on Linux and the M1, split under MSVC.** Outside MSVC the fused path wins all 32 converged minified documents, 19 on Linux and 13 on macOS, with no ties and no losses. Its margins run from 1% (Canada on Linux/GCC) to 149% (Google Maps on Linux/Clang, where the two-stage path is unusually slow at 922 MB/s against the fused path's 2,299; the next widest is 68%, Google Maps on macOS/Clang); on Linux/GCC they are widest on CitmCatalog (34%) and Instruments (44%). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order. Under MSVC the fused path wins five documents, Random by 2%, Canada by 8%, Discord by 9%, Google Maps by 9% and Twitter by 10%. The two-stage path wins the other five, by 3% (Marine IK and Marine IK Reverse) to 12% (Mesh).

**Prettified documents on x86: the two-stage path wins 25 of 30.** It wins 9 of the 10 under MSVC, by 7% (Canada) to 42% (Mesh); 8 of the 10 on Linux/Clang, by 4% (Marine IK) to 38% (Discord); and 8 of the 10 on Linux/GCC, by 3% (Google Maps) to 21% (Twitter). The fused path wins the other five: Google Maps under MSVC (by 3%), Canada (5%) and Marine IK Reverse (4%) on Linux/Clang, and Marine IK Reverse (5%) and Random (5%) on Linux/GCC. The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That narrows the gap, and on five documents it closes it, but on most x86 documents it does not. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis.

**Prettified documents on the M1: fused, with Mesh the exception.** The fused path wins 10 of the 12 converged documents, by 2% (Canada on macOS/GCC) to 53% (Random on macOS/Clang). Twitter on macOS/GCC is a tie, and the two-stage path wins Mesh on macOS/Clang by 8%; prettified Mesh did not converge on macOS/GCC. NEON largely escapes the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it.

**Mesh is the document the tape wins most often.** The two-stage path wins prettified Mesh on every build where it converged (all but macOS/GCC), by 8% (macOS/Clang) to 42% (Windows/MSVC). It also wins minified Mesh under MSVC by 12%, while the fused path wins minified Mesh on Linux/Clang (18%), macOS/GCC (11%) and macOS/Clang (22%); minified Mesh did not converge on Linux/GCC. We have not profiled what is particular about this document.

**MSVC slows the fused path more than the tape.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the ten minified documents that converged on both builds, the fused path reaches a median 48% of its Linux/Clang throughput, the two-stage path 57% and simdjson 45%. The shortfall is uneven. On minified CitmCatalog the fused path reaches 40% of its Linux/Clang throughput while the two-stage path reaches 56% and simdjson 42%; on minified Marine IK the figures are 44%, 53% and 43%, and on minified Mesh 36%, 48% and 38%. Those are documents on which the tape wins under MSVC (by 5%, 3% and 12%). On minified Canada, Discord and Twitter the fused path keeps nearly as much as the tape (56% against 61%, 50% against 53%, 66% against 62%), and under MSVC it wins all three. The exceptions run the other way: on Google Maps and Instruments the two-stage path under MSVC reaches 105% and 106% of its Linux/Clang throughput, because its Linux/Clang runs on those two documents are slow relative to the fused path (922 against 2,299 MB/s on Google Maps). The two-stage path's stage 1 is branch-free intrinsic code whose shape is fixed by the source, while the fused path and simdjson's On Demand walk are both branch-heavy code whose performance rests on the optimizer. We take that difference in exposure to the compiler to be why MSVC moves the balance toward the tape, but we have not isolated it.

**Against simdjson, Jsonifier wins 106 of 109 tests with whichever path is faster.** It ties two, prettified Random on Linux/Clang and prettified Mesh on Linux/GCC, and loses one, prettified Instruments on Linux/Clang (by 2%). The fused path alone wins 100, ties 1 and loses 8: prettified Mesh on MSVC, Linux/Clang and Linux/GCC, prettified Discord, Instruments, Random and Twitter on Linux/Clang, and prettified Twitter on Linux/GCC. The tie is prettified Mesh on macOS/Clang.

**Reverse key order: MSVC is where it bites.** Requesting every key in reverse order forces simdjson's On Demand API into the rescanning behavior described in §3. Under MSVC simdjson manages 36 MB/s on minified Marine IK Reverse against 387 MB/s for the two-stage path, 11× slower, and 194 MB/s on the prettified version against 1,932 MB/s, 10× slower. On the other builds where Marine IK Reverse converged, the gap is much smaller: the faster Jsonifier path is 1.2× to 1.6× faster than simdjson in both runs.

The routing implication is sharper than §2's rule of thumb. On the M1, the fused path is the right default for POD and minified input and for prettified input other than Mesh. On Linux it is right for POD and minified input, and prettified input mostly belongs to the two-stage path (16 of 20 documents). Under MSVC it is right for POD-type data and five of the ten minified documents, and prettified input should go to the two-stage path. Routing each test to its faster path would raise the record against simdjson from 100 wins, 1 tie and 8 losses to 106, 2 and 1. Eight tests change hands, all of them prettified documents and four of them on Linux/Clang. The router does not yet take the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents, not the POD-type tests. The tables list every test where both converged.

#### Freshly allocated objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 888 | 733 | 1.21× | Win |
| Canada (prettified) | 2,567 | 2,190 | 1.17× | Win |
| CitmCatalog (minified) | 1,660 | 1,154 | 1.44× | Win |
| CitmCatalog (prettified) | 4,059 | 2,900 | 1.40× | Win |
| Discord (minified) | 1,626 | 1,516 | 1.07× | Win |
| Discord (prettified) | 2,551 | 2,393 | 1.07× | Win |
| Google Maps Response (minified) | 1,727 | 1,261 | 1.37× | Win |
| Google Maps Response (prettified) | 3,759 | 2,967 | 1.27× | Win |
| Instruments (minified) | 2,146 | 1,745 | 1.23× | Win |
| Instruments (prettified) | 3,443 | 3,152 | 1.09× | Win |
| Marine IK Reverse (minified) | 714 | 660 | 1.08× | Win |
| Marine IK Reverse (prettified) | 3,031 | 3,025 | 1.00× | Tie |
| Marine IK (minified) | 723 | 652 | 1.11× | Win |
| Marine IK (prettified) | 3,279 | 2,959 | 1.11× | Win |
| Mesh (prettified) | 1,999 | 2,090 | 0.96× | **Loss** |
| Random (minified) | 1,280 | 1,150 | 1.11× | Win |
| Random (prettified) | 1,744 | 2,025 | 0.86× | **Loss** |
| Twitter (minified) | 1,575 | 1,711 | 0.92× | **Loss** |
| Twitter (prettified) | 2,127 | 2,503 | 0.85× | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 655 | 525 | 1.25× | Win |
| Canada (prettified) | 1,742 | 1,483 | 1.18× | Win |
| CitmCatalog (minified) | 1,282 | 1,040 | 1.23× | Win |
| CitmCatalog (prettified) | 2,874 | 2,375 | 1.21× | Win |
| Discord (minified) | 996 | 933 | 1.07× | Win |
| Discord (prettified) | 1,480 | 1,295 | 1.14× | Win |
| Google Maps Response (minified) | 1,167 | 793 | 1.47× | Win |
| Google Maps Response (prettified) | 1,957 | 1,892 | 1.03× | Win |
| Mesh (minified) | 837 | 544 | 1.54× | Win |
| Random (minified) | 628 | 577 | 1.09× | Tie |
| Twitter (minified) | 1,446 | 1,223 | 1.18× | Win |
| Twitter (prettified) | 1,885 | 1,587 | 1.19× | Win |

#### Reused objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,072 | 838 | 1.28× | Win |
| Canada (prettified) | 3,028 | 2,492 | 1.22× | Win |
| CitmCatalog (minified) | 2,074 | 1,310 | 1.58× | Win |
| CitmCatalog (prettified) | 4,636 | 3,256 | 1.42× | Win |
| Discord (minified) | 2,139 | 1,730 | 1.24× | Win |
| Discord (prettified) | 3,094 | 2,718 | 1.14× | Win |
| Google Maps Response (prettified) | 4,004 | 3,150 | 1.27× | Win |
| Instruments (minified) | 2,297 | 1,837 | 1.25× | Win |
| Instruments (prettified) | 3,813 | 3,257 | 1.17× | Win |
| Marine IK Reverse (minified) | 779 | 736 | 1.06× | Win |
| Marine IK (minified) | 789 | 728 | 1.08× | Win |
| Marine IK (prettified) | 3,563 | 3,323 | 1.07× | Win |
| Mesh (minified) | 1,104 | 1,172 | 0.94× | **Loss** |
| Mesh (prettified) | 2,036 | 2,197 | 0.93× | **Loss** |
| Twitter (minified) | 1,929 | 1,998 | 0.97× | **Loss** |
| Twitter (prettified) | 2,707 | 2,795 | 0.97× | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 809 | 620 | 1.30× | Win |
| Canada (prettified) | 2,092 | 1,720 | 1.22× | Win |
| CitmCatalog (minified) | 1,489 | 1,146 | 1.30× | Win |
| CitmCatalog (prettified) | 3,232 | 2,580 | 1.25× | Win |
| Discord (minified) | 1,389 | 1,056 | 1.31× | Win |
| Discord (prettified) | 1,839 | 1,583 | 1.16× | Win |
| Google Maps Response (minified) | 1,301 | 1,014 | 1.28× | Win |
| Google Maps Response (prettified) | 2,493 | 1,958 | 1.27× | Win |
| Instruments (prettified) | 2,502 | 1,898 | 1.32× | Win |
| Marine IK Reverse (prettified) | 2,347 | 1,868 | 1.26× | Win |
| Marine IK (minified) | 539 | 460 | 1.17× | Win |
| Marine IK (prettified) | 2,257 | 1,931 | 1.17× | Win |
| Random (minified) | 1,136 | 638 | 1.78× | Win |
| Twitter (minified) | 1,670 | 1,291 | 1.29× | Win |
| Twitter (prettified) | 2,408 | 1,736 | 1.39× | Win |

In the freshly allocated run, two-stage Jsonifier wins 25 of these 31 tests, ties 2 and loses 4. On macOS/GCC it wins 11 of 12, by 3% (Google Maps, prettified) to 54% (Mesh, minified), and ties minified Random. On Linux/GCC it wins 14 of 19, by 7% (Discord, both forms) to 44% (CitmCatalog, minified), ties Marine IK Reverse (prettified), and is slower on prettified Mesh (by 4%), prettified Random (by 14%) and Twitter in both forms (by 8% minified and 15% prettified). Mesh is also where Jsonifier is weakest against simdjson On Demand on that build (§2.4), so its losses on Mesh look like a property of the document under GCC on x86 rather than of the tape; we have not profiled them. With the fused path instead of the two-stage one, Jsonifier beats the reflection reader on 14 of 19 tests on Linux/GCC and on all 12 on macOS/GCC. With objects reused, two-stage Jsonifier wins 27 of 31: on Linux/GCC it wins 12 of 16 and is slower on Mesh in both forms (by 6% minified and 7% prettified) and on Twitter in both forms (by 3% each), and on macOS/GCC it wins all 15.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row, and the two rows differ by at most 10% across all 19 converged results.

#### Freshly allocated objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 5,746 | 1,850 | 3.11× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 6,954 | 2,703 | 2.57× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 6,653 | 5,373 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | 8,488 | 7,164 | 1.18× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | minified | 6,993 | 5,275 | 1.33× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | prettified | 8,594 | 6,876 | 1.25× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 3,933 | 2,850 | 1.38× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | 4,017 | 3,808 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | 3,976 | 3,364 | 1.18× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,770 | 4,289 | 1.11× | Win |

#### Reused objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 5,729 | 1,918 | 2.99× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 7,284 | 2,775 | 2.62× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 6,732 | 5,544 | 1.21× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | 8,836 | 7,312 | 1.21× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | minified | 7,318 | 5,398 | 1.36× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | prettified | 8,783 | 7,058 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 3,878 | 3,185 | 1.22× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | n/c | n/c | — | — |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | 4,118 | 3,456 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,888 | 4,385 | 1.11× | Win |

Jsonifier wins all 19 converged results, by 1.05× (prettified on macOS/GCC, freshly allocated) to 3.11× (minified under MSVC, freshly allocated). Throughput here is counted over the whole document, so these figures run well above the full-parse ones: on minified Twitter the fused path parses at 1,183 MB/s under MSVC and 1,796 MB/s on Linux/Clang when it materializes every value, against 5,746 and 6,653 MB/s here, because the bytes the caller does not ask for are skipped through the tape instead of being walked. The margin over simdjson is 2.6× to 3.1× under MSVC and 1.05× to 1.4× elsewhere. On minified Twitter in the freshly allocated run, simdjson On Demand under MSVC reaches 34% of its Linux/Clang throughput and Jsonifier's stage 1 + 2 path reaches 86%. This is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

## 3. The single-pass path: `json_cursor` over raw text

The fused path is implemented as `json_cursor<parseOpts, read_buffer_ptr>` — a set of static navigation primitives over a raw `read_buffer_ptr` cursor, with the end pointer, the detected indentation and the string buffer held in a `parse_context` and the nesting depth passed down the call chain, walking the document exactly once. Several design decisions distinguish it:

**Compile-time key fusion.** For each reflected member, the expected token is baked into the binary at compile time as a fused literal. In minified known-order mode, an entire member header — leading comma, quoted key, and colon — collapses into a single constant:

```cpp
template<uint64_t index, typename literal_type> JSONIFIER_INLINE static constexpr auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
	if constexpr (index > 0) {
		return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	} else {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}
}
```

When the document arrives in the expected order — the overwhelmingly common case for machine-generated JSON — matching `,"name":` is one constant-length comparison, and the cursor jumps the entire header in a single add. No tokenizer, no tape, no per-character state machine.

**Adaptive out-of-order recovery.** When the known-order guess misses, the parser falls back to a compile-time hash map over the type's keys (`hash_map<value_type>::findIndex`), dispatches through a generated table, and — crucially — *learns*: a thread-local `antiHashStatesNew` array records which index actually matched at each slot, so a document stream with a consistently permuted key order pays the hash cost once and then rides the corrected fast path.

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans — and skips — every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, turning an O(members) parse into an O(members²) crawl over structural positions — per object, per document, forever. The model has no memory; the millionth permuted document costs exactly what the first one did.

Jsonifier's failure mode for the same situation is: one hash lookup, one dispatch-table indirection, and a learned correction. The mismatch cost is paid once per key slot per thread, not once per object instance. A feed of a billion documents with keys in reversed order parses at effectively the same throughput as a feed in declared order, because after the first document the "expected order" *is* the observed order. The schema-directed model converts key order from a per-document runtime tax into a per-stream calibration.

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `context.indentSize * depth`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (string unescaping, discussed in §6) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra with several architectural departures.

### 4.1 Step geometry as a per-compiler compile-time constant

simdjson classifies input per 64-byte block, and its generic indexer steps through the buffer 128 bytes per iteration — two 64-byte blocks, software-pipelined one block deep, a geometry it retains even on its widest kernel (the Icelake implementation invokes `index<128>`). Jsonifier generalizes the unit of work to a *step* of `simdBlocksPerStep` 64-byte blocks, with the constant chosen per ISA **and per compiler**:

```cpp
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
using jsonifier_simd_int_t			= __m256i;
	#if JSONIFIER_COMPILER_CLANG
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 4;
	#elif JSONIFIER_COMPILER_GCC
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 8;
	#else
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 8;
	#endif
#endif
```

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA reflects real differences in how each compiler schedules the unrolled block bodies: on AVX2 Clang takes four blocks (256 bytes) per step where GCC and MSVC take eight (512 bytes), and on NEON both compilers take four blocks per step with a drain burst of 8. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations — a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(const simd_array_t in_01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNext() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit — a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional — `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNextInString`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4` — the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm — `followsNonquoteScalar` carries the cross-block bit — so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
	const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
	const uint64_t op		   = simd::op_collector::impl(in_01, opTable, spaceMask);
	const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
	const uint64_t scalarStart = scalar & ~follows;
	return op | simd::rope_detector<rope_block>::quotes | scalarStart;
}
```

Note the overload pair: the minified variant omits the whitespace collector entirely — an entire classification lane deleted at compile time when the caller declares the input minified. This is the same philosophy as the compiler-specific step constants: every fact known before runtime is burned into the instruction stream.

### 4.4 The drain architecture: bits → indices

Extraction — converting each 64-bit structural mask into byte offsets on the tape — is where naive implementations serialize hard, because the classic loop (`tzcnt`, store, `blsr`, repeat) is a loop-carried dependency chain of length popcount.

Jsonifier's answer differs by ISA.

**AVX-512: fully vectorized drain.** With `VBMI2` available, the bitmask never enters a scalar loop at all. `_mm512_maskz_compress_epi8` compresses a constant 0..63 byte ramp under the structural mask, producing the set-bit positions as packed bytes; four `cvtepu8_epi32` widenings plus a broadcast base-add stream up to 64 indices to the tape in a handful of instructions:

```cpp
const __m512i indexes			= _mm512_maskz_compress_epi8(bits,
			  _mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, 0x37363534, 0x33323130, 0x2f2e2d2c, 0x2b2a2928, 0x27262524, 0x23222120, 0x1f1e1d1c, 0x1b1a1918, 0x17161514, 0x13121110,
				  0x0f0e0d0c, 0x0b0a0908, 0x07060504, 0x03020100));
const __m512i startIndexLocal = _mm512_set1_epi32(base);
__m512i t0					  = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes));
_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
```

The widening cascade is guarded by the lane's precomputed popcount (`count > 16`, `> 32`, `> 48`), so sparse blocks pay for one store, not four. simdjson employs the same family of trick: its generic indexer exposes a `CUSTOM_BIT_INDEXER` hook, and the Icelake kernel fills it with its own `VBMI2` compress-based extractor. On this ISA the two libraries again share the inner mechanism, and the performance separation comes from the surrounding step architecture (§4.1, §4.4 drain scheduling below) rather than the extraction primitive.

**AVX2/AVX/NEON: the folded stepped drain.** Without byte-compress, extraction must use the tzcnt chain — but the chain's *structure* is still a compile-time decision. `write_indices_functor` emits one extract-advance pair per index; `write_indices_stepped_functor` groups them into unconditional bursts of `simdTapeStep` writes:

```cpp
template<auto...> struct write_indices_functor {
	using size_type = uint64_t;

	template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& bits, structural_index_ptr tape) noexcept {
		tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
		bits									  = simd::tape_writer_op::advance(bits);
	}
};

template<uint64_t step> struct write_indices_stepped_functor {
	using size_type = uint64_t;
	template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& bits, structural_index_ptr tape, uint64_t cnt) noexcept {
		if constexpr (index > 0) {
			if ((index < cnt)) [[unlikely]] {
				functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
				return true;
			} else {
				return false;
			}
		} else {
			functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
			return true;
		}
	}
};
```

The `functor_runner`'s `implAnd` expands a stepped range sequence `<0, 64, simdTapeStep>` through an `&&`-fold: each group writes `simdTapeStep` indices *unconditionally* (over-writing garbage past the true count is harmless — the tape cursor only advances by the real popcount), and the fold short-circuits the moment a group's start index reaches the count. One predictable branch per `simdTapeStep` extractions instead of one per extraction.

Credit where due: simdjson's current generic `bit_indexer` implements the same stepped-burst pattern — `write_indexes_stepped<START, END, STEP>` via recursive template expansion, unconditional bursts, `simdjson_unlikely` short-circuit checks at each group boundary — with the burst size exposed as a build macro (`SIMDJSON_STRUCTURAL_INDEXER_STEP`, default 4). The two libraries have converged on the extraction inner loop itself. The divergences are in everything around it:

- **Coverage.** simdjson's stepped expansion runs to a fixed `STEP_UNTIL` of 24 set bits and falls back to a plain scalar loop for denser blocks; Jsonifier's fold covers the full 0..64 range at the same stride.
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep`, because the two constants interact. AVX2 uses a burst of 4 on all three compilers, with 4 blocks per step on Clang and 8 on GCC and MSVC; NEON uses a burst of 8 on both Clang and GCC, at 4 blocks per step (§4.1).
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step — up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains — each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser — it is a *specialization* of the same `json_cursor` interface over `structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the cursor interface: `cursor_t<options, context_type>` picks the specialization from the context's `iterator_type`, and the `json_cursor<parseOpts, structural_index_ptr>` specialization supplies the token-navigation primitives:

```cpp
template<typename context_type> JSONIFIER_INLINE static bool skipValue(structural_index_ptr& iter, context_type& context) noexcept {
	if (iter >= context.endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>(iter, context);
	}
	const char first = static_cast<char>(*valuePtr(iter, context));
	if (first == '{' || first == '[') {
		int64_t depth{};
		while (iter < context.endIter) {
			depth += nestingDeltaTable[static_cast<uint8_t>(context.stringRoot[*iter])];
			++iter;
			if (depth == 0) {
				return true;
			}
		}
		return reject<parse_statuses::unexpected_string_end>(iter, context);
	}
	++iter;
	return true;
}
```

`nestingDeltaTable` maps `{` and `[` to +1, `}` and `]` to −1 and every other byte to 0, so the loop needs one table load and one add per tape entry and a single branch on the running depth.

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per structural entry — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer cursor, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

The asymmetry defines the routing rule. When the caller wants *every* value (full-document parse into a reflected type), skips are rare and the tape is overhead. When the caller wants a *few* values from a large document (partial reading), or wants only the structure itself (prettify/minify, where the transformation is literally "copy bytes, adjusting whitespace at structural positions"), skips dominate and the tape converts O(bytes) navigation into O(structurals).

The same routing logic appears in stage 1's own entry point: `reset<minified>` selects between the whitespace-aware and whitespace-free classification pipelines, and the tail-block handling pads with `0x20`, indexes the pad, then retroactively pops any tape entries pointing past the true document length — branchless main loop, exact tape.

## 6. Distributed UTF-8 validation

Let's be precise about what simdjson does, because it is also an in-register scheme: `json_structural_indexer::next` feeds every 64-byte block into `utf8_checker::check_next_input` using the very registers the classifier just loaded. Validation is fused into stage 1, costs no extra pass over memory, and covers the entire input. (The standalone `generic_validate_utf8` exists too, for the buffer-validation API.) On Demand then additionally defers some string-level checks to traversal.

So the distinction is not "in-register versus dedicated pass" — both libraries validate in registers already in flight. The distinction is **scope and location**:

- **simdjson** validates *all input bytes*, in *stage 1*. Elegant and total, but it means the range checker runs over structural characters, whitespace, numbers, and literals — bytes that a JSON parser's own grammar already constrains to ASCII. For a document that is 70% non-string content, 70% of the validation work confirms what token matching would prove for free.
- **Jsonifier** validates *string bytes only*, in the *string parse loop*. Everything outside strings is implicitly ASCII-constrained: structural characters are matched literally, numbers pass through a digit-table parser, `true`/`false`/`null` are compared as packed integer constants — any non-ASCII byte in those positions is a parse error by construction, no range checker required. The only place arbitrary bytes can legally appear is inside strings, and that is exactly — and only — where `utf8_register_validator` runs.

One accounting qualifier belongs in the main text rather than a footnote, because it bounds the size of the claim: simdjson's checker leads with an ASCII fast path — a block whose registers OR to an ASCII-only result skips the multibyte carry chain entirely and pays roughly one reduction and one test. For ASCII-heavy input, then, "validation work proportional to input bytes" is proportional with a small constant, not with the full cost of the multibyte machinery. The scope distinction survives the qualifier — Jsonifier runs *no* validation instructions of any kind over non-string bytes, fast-path or otherwise, and the two approaches still separate cleanly on documents dense in non-ASCII string content — but the honest comparison states the fast path up front rather than burying it.

Jsonifier ships the same range-based algorithm simdjson uses (the `byte1High`/`byte1Low`/`byte2High` nibble-lookup classifier with the `carry`/`tooShort`/`tooLong`/`surrogate` error-bit algebra) in two deployments. The first, the standalone `validateUtf8` built on `utf8_checker`, is the conventional block validator, with the same ASCII fast path (`orAll` the block's registers, one test, skip the multibyte machinery). The second is the distributed one:

```cpp
if (delimiters == static_cast<integer_type>(0)) {
	validator.checkRegister(simdValue);
	string1Start += bytesProcessed;
	string2 += bytesProcessed;
	continue;
}
```

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte) — so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator — the position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The stage-1 companion paper covers this mechanism in full [§6.1 there].

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes* — every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on minified documents outside MSVC and on most documents on the M1; slower on most prettified x86 input (25 of 30 documents) and on five of ten minified documents under MSVC (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared cursor interface |
| Out-of-order keys | Forward scan + wrap-around rescan per lookup, per object — no memory across documents | One hash fallback, then learned per-slot order correction (thread-local, per stream) |
| Schema knowledge (known-type workloads) | Exists in caller's traversal code — invisible to the library | Declared once via reflection — consumed by the architecture |
| Step size | 128 bytes (two 64-byte blocks), pipelined one block deep | 256–512 bytes, per-(ISA × compiler) constant |
| Bit extraction | Stepped tzcnt bursts (global STEP macro, scalar tail past 24 bits); VBMI2 compress on Icelake | Same primitives, but full-range folded bursts with per-toolchain-swept stride |
| Drain scheduling | Previous block drained during current block's classification | Whole-step deferred batch drain, lane offsets precomputed from popcounts |
| Escape short-circuit | Yes (zero-backslash fast exit) | Yes (same) |
| In-string prefix-XOR | Computed every block | Skipped for quoteless blocks |
| UTF-8 validation | In-register, fused into stage 1, over all input bytes (ASCII fast path per block); verdict at EOF | In-register, fused into string parsing, over string bytes only, carried across the width cascade; verdict per string |
| Specialization axis | Runtime CPU dispatch | Compile-time everything (Cathedral Architecture) |

The last row is the root of every other difference. simdjson must ship one binary that runs well everywhere, so its kernels are shaped for runtime selection among a fixed set. Jsonifier's founding constraint — only the *data* is a runtime variable; the schema, the ISA, the compiler, the parse options are all known at build time — lets every routing decision in this paper (tape or no tape, whitespace lane or not, validation or not, 4 blocks or 8, burst of 1 or 4) be resolved before the first byte is read.

## 8. Conclusion

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head, on both freshly allocated and reused target objects. Across 109 freshly allocated tests on five platforms the fused path wins 77, ties 1 and loses 31, and where it loses is a matter of platform more than of principle. It wins all 25 POD-type tests, by up to 8.5× (Bool on Linux/GCC), all 32 minified documents outside MSVC, and 10 of the 12 prettified documents on the M1. The two-stage path wins 25 of the 30 prettified documents on x86, and under MSVC it also takes five of the ten minified documents. The tape, in other words, is not only an index for partial reads: on some compilers and some layouts it is also the faster way to read the whole book, and the router should learn which. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 106 of those 109 tests and ties two, its own stage-1 + reflection path beats simdjson's stage-1 + reflection reader on 25 of 31 (ties 2), and on the partial-reading test, where the index is the right tool, it beats simdjson On Demand on all 19 converged results. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*