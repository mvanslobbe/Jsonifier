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

For full-document parsing into known types, the tape is overhead in principle, but whether skipping it is faster in practice turns out to depend on the compiler and on the shape of the input. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. In the freshly allocated run, the fused path wins every POD-type test, 27 of the 31 converged minified documents on Linux and macOS (three more are ties), and 8 of the 11 converged prettified documents on the M1. The two-stage path wins all 27 converged prettified documents on x86, and five of the ten minified documents under MSVC.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 5, 2026, with Jsonifier [b87a5d0](https://github.com/nihilai-collective/jsonifier/commit/b87a5d0), simdjson [1a37712](https://github.com/simdjson/simdjson/commit/1a37712) and BenchmarkSuite [8d787b1](https://github.com/nihilai-collective/benchmarksuite/commit/8d787b1). The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep but is left out here, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3 and §2.5 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document, which §2.6 covers on its own, and "Small" cut-down copies of each document (at most 5 KiB minified) that measure per-call overhead; neither is counted in §2.2 to §2.5.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on x86 (10% and 5% on the virtualized M1), and a test is ranked only if every library in it converges, which is why some platforms have fewer than 25 tests. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to every library. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced five ties between the two Jsonifier paths across 103 freshly allocated tests, and seven across 109 reused ones.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 63 tests, five are statistical ties, and the two-stage path wins 35. The split is almost entirely a matter of platform:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 9 | 1 | 15 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 12 | 0 | 11 | 23 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 12 | 1 | 7 | 20 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 15 | 2 | 1 | 18 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 15 | 1 | 1 | 17 of 25 |
| **Aggregate** | **63** | **5** | **35** | **103 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 9 / 0 / 0 |
| Minified corpus documents | 4 / 1 / 5 | 14 / 1 / 1 | 13 / 2 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 0 / 0 / 17 | 8 / 1 / 2 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 22 / 1 / 0 | 18 / 1 / 4 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 18 / 0 / 2 | 18 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 17 / 0 / 1 | 16 / 1 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 17 / 0 / 0 | 16 / 0 / 1 |
| **Aggregate** | **99 / 1 / 3** | **92 / 2 / 9** |

**Reused objects.** With the target object held across iterations, the picture is the same:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 9 | 2 | 14 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 12 | 1 | 10 | 23 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 13 | 3 | 8 | 24 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 14 | 1 | 1 | 16 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 20 | 0 | 1 | 21 of 25 |
| **Aggregate** | **68** | **7** | **34** | **109 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 9 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 4 / 1 / 5 | 16 / 2 / 0 | 14 / 0 / 0 |
| Prettified corpus documents | 0 / 1 / 9 | 0 / 2 / 18 | 10 / 1 / 2 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 22 / 1 / 0 | 19 / 0 / 4 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 23 / 0 / 1 | 22 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 15 / 1 / 0 | 15 / 0 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 21 / 0 / 0 | 20 / 0 / 1 |
| **Aggregate** | **106 / 2 / 1** | **100 / 0 / 9** |

Across 109 converged tests the fused path wins 68, ties 7 and loses 34 to the two-stage path, and the platform split barely moves: MSVC is 9 / 2 / 14, while macOS/Clang, with more tests converging, goes 20 / 0 / 1. Against simdjson, the faster Jsonifier path wins 106, ties 2 and loses 1, and the fused path alone wins 100 and loses 9. Taking allocation out of the timed region helps every library, so it moves individual verdicts, but it does not change which path is faster on a given platform and input shape.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput, and "n/c" means the test did not converge for at least one library.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 354 | 112 | 85 | 3.15× | Fused | Win |
| Double (POD) | 416 | 233 | 124 | 1.78× | Fused | Win |
| Int64 (POD) | 1,170 | 512 | 293 | 2.29× | Fused | Win |
| String (POD) | 1,022 | 716 | 640 | 1.43× | Fused | Win |
| Uint64 (POD) | 2,143 | 728 | 390 | 2.95× | Fused | Win |
| Canada (minified) | 585 | 574 | 372 | 1.02× | Tie | Win |
| Canada (prettified) | 1,567 | 1,692 | 1,129 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 900 | 1,031 | 467 | 0.87× | Two-stage | Win |
| CitmCatalog (prettified) | 2,042 | 2,537 | 1,229 | 0.80× | Two-stage | Win |
| Discord (minified) | 1,018 | 945 | 612 | 1.08× | Fused | Win |
| Discord (prettified) | 1,337 | 1,494 | 978 | 0.89× | Two-stage | Win |
| Google Maps Response (minified) | 1,032 | 1,004 | 513 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,130 | 2,154 | 1,252 | 0.99× | Two-stage | Win |
| Instruments (minified) | 887 | 963 | 662 | 0.92× | Two-stage | Win |
| Instruments (prettified) | 1,439 | 1,760 | 1,255 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 374 | 394 | 36 | 0.95× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,567 | 1,936 | 191 | 0.81× | Two-stage | Win |
| Marine IK (minified) | 377 | 409 | 253 | 0.92× | Two-stage | Win |
| Marine IK (prettified) | 1,590 | 2,008 | 1,272 | 0.79× | Two-stage | Win |
| Mesh (minified) | 502 | 610 | 421 | 0.82× | Two-stage | Win |
| Mesh (prettified) | 745 | 1,094 | 786 | 0.68× | Two-stage | Win |
| Random (minified) | 775 | 751 | 516 | 1.03× | Fused | Win |
| Random (prettified) | 1,199 | 1,384 | 930 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,207 | 1,087 | 806 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,329 | 1,576 | 1,162 | 0.84× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,303 | 383 | 199 | 6.02× | Fused | Win |
| Double (POD) | 1,412 | 487 | 294 | 2.90× | Fused | Win |
| Int64 (POD) | 3,556 | 1,115 | 618 | 3.19× | Fused | Win |
| String (POD) | 1,643 | 1,205 | 1,031 | 1.36× | Fused | Win |
| Uint64 (POD) | 3,781 | 1,077 | 637 | 3.51× | Fused | Win |
| Canada (minified) | 1,033 | 998 | 698 | 1.03× | Fused | Win |
| Canada (prettified) | 2,508 | 2,882 | 2,110 | 0.87× | Two-stage | Win |
| CitmCatalog (minified) | 2,046 | 1,772 | 1,319 | 1.15× | Fused | Win |
| CitmCatalog (prettified) | 4,016 | 4,419 | 3,364 | 0.91× | Two-stage | Win |
| Discord (minified) | 1,801 | 1,705 | 1,504 | 1.06× | Fused | Win |
| Discord (prettified) | 2,036 | 2,835 | 2,375 | 0.72× | Two-stage | Win |
| Google Maps Response (minified) | 2,292 | 2,103 | 1,335 | 1.09× | Fused | Win |
| Google Maps Response (prettified) | 3,790 | 4,556 | 3,540 | 0.83× | Two-stage | Win |
| Instruments (minified) | n/c | n/c | n/c | — | — | — |
| Instruments (prettified) | 2,966 | 3,654 | 3,380 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | 3,509 | 3,690 | 2,934 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 891 | 814 | 660 | 1.09× | Fused | Win |
| Marine IK (prettified) | 3,527 | 3,720 | 3,038 | 0.95× | Two-stage | Win |
| Mesh (minified) | 1,417 | 1,267 | 1,219 | 1.12× | Fused | Win |
| Mesh (prettified) | 1,854 | 2,364 | 2,219 | 0.78× | Two-stage | Win |
| Random (minified) | 1,466 | 1,515 | 1,183 | 0.97× | Two-stage | Win |
| Random (prettified) | 2,098 | 2,664 | 2,095 | 0.79× | Two-stage | Win |
| Twitter (minified) | 1,960 | 1,914 | 1,838 | 1.02× | Fused | Win |
| Twitter (prettified) | 2,074 | 2,621 | 2,597 | 0.79× | Two-stage | Tie |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,198 | 274 | 182 | 8.02× | Fused | Win |
| Double (POD) | 1,249 | 361 | 231 | 3.46× | Fused | Win |
| Int64 (POD) | 3,349 | 836 | 514 | 4.01× | Fused | Win |
| String (POD) | 1,746 | 1,006 | 948 | 1.74× | Fused | Win |
| Uint64 (POD) | 3,314 | 834 | 499 | 3.98× | Fused | Win |
| Canada (minified) | 886 | 887 | 746 | 1.00× | Tie | Win |
| Canada (prettified) | 2,257 | 2,606 | 2,183 | 0.87× | Two-stage | Win |
| CitmCatalog (minified) | 2,261 | 1,714 | 1,356 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 3,768 | 4,124 | 3,429 | 0.91× | Two-stage | Win |
| Discord (minified) | n/c | n/c | n/c | — | — | — |
| Discord (prettified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (minified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (prettified) | 3,643 | 3,809 | 2,569 | 0.96× | Two-stage | Win |
| Instruments (minified) | 3,070 | 2,016 | 1,347 | 1.52× | Fused | Win |
| Instruments (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (minified) | 761 | 705 | 573 | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 3,040 | 3,075 | 2,663 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 775 | 717 | 545 | 1.08× | Fused | Win |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 1,158 | 1,077 | 1,211 | 1.07× | Fused | **Loss** |
| Mesh (prettified) | 1,871 | 2,095 | 2,268 | 0.89× | Two-stage | **Loss** |
| Random (minified) | 1,420 | 1,314 | 886 | 1.08× | Fused | Win |
| Random (prettified) | 2,112 | 2,408 | 1,649 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,003 | 1,712 | 1,343 | 1.17× | Fused | Win |
| Twitter (prettified) | 1,991 | 2,575 | 1,946 | 0.77× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,418 | 198 | 171 | 7.15× | Fused | Win |
| Double (POD) | 797 | 263 | 128 | 3.03× | Fused | Win |
| Int64 (POD) | 1,921 | 523 | 380 | 3.67× | Fused | Win |
| String (POD) | n/c | n/c | n/c | — | — | — |
| Uint64 (POD) | 2,071 | 597 | 421 | 3.47× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 1,918 | 1,275 | 974 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | 2,889 | 2,567 | 2,426 | 1.13× | Fused | Win |
| Discord (minified) | 1,450 | 1,076 | 942 | 1.35× | Fused | Win |
| Discord (prettified) | 1,746 | 1,560 | 1,368 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 1,606 | 1,017 | 684 | 1.58× | Fused | Win |
| Google Maps Response (prettified) | 2,572 | 1,911 | 1,546 | 1.35× | Fused | Win |
| Instruments (minified) | 1,994 | 1,410 | 1,110 | 1.41× | Fused | Win |
| Instruments (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | 2,036 | 1,934 | 1,710 | 1.05× | Tie | Win |
| Mesh (minified) | 692 | 744 | 767 | 0.93× | Tie | **Loss** |
| Mesh (prettified) | 1,095 | 1,402 | 1,329 | 0.78× | Two-stage | Win |
| Random (minified) | 858 | 688 | 556 | 1.25× | Fused | Win |
| Random (prettified) | 1,338 | 978 | 1,016 | 1.37× | Fused | Win |
| Twitter (minified) | 1,528 | 1,332 | 917 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,974 | 1,835 | 1,488 | 1.08× | Fused | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,094 | 174 | 157 | 6.27× | Fused | Win |
| Double (POD) | 706 | 270 | 183 | 2.61× | Fused | Win |
| Int64 (POD) | 1,991 | 602 | 452 | 3.31× | Fused | Win |
| String (POD) | 1,089 | 586 | 721 | 1.86× | Fused | Win |
| Uint64 (POD) | 1,872 | 603 | 425 | 3.11× | Fused | Win |
| Canada (minified) | 672 | 604 | 406 | 1.11× | Fused | Win |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 1,575 | 1,042 | 637 | 1.51× | Fused | Win |
| CitmCatalog (prettified) | n/c | n/c | n/c | — | — | — |
| Discord (minified) | 1,813 | 961 | 1,185 | 1.89× | Fused | Win |
| Discord (prettified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (minified) | 1,917 | 1,022 | 857 | 1.88× | Fused | Win |
| Google Maps Response (prettified) | 2,736 | 1,916 | 2,090 | 1.43× | Fused | Win |
| Instruments (minified) | n/c | n/c | n/c | — | — | — |
| Instruments (prettified) | 2,789 | 2,289 | 1,699 | 1.22× | Fused | Win |
| Marine IK Reverse (minified) | 445 | 468 | 350 | 0.95× | Tie | Win |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 1,094 | 800 | 784 | 1.37× | Fused | Win |
| Mesh (prettified) | 1,241 | 1,576 | 1,439 | 0.79× | Two-stage | Win |
| Random (minified) | 1,461 | 821 | 893 | 1.78× | Fused | Win |
| Random (prettified) | 2,036 | 1,060 | 1,291 | 1.92× | Fused | Win |
| Twitter (minified) | 1,849 | 1,312 | 1,427 | 1.41× | Fused | Win |
| Twitter (prettified) | n/c | n/c | n/c | — | — | — |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 391 | 127 | 90 | 3.09× | Fused | Win |
| Double (POD) | 538 | 256 | 132 | 2.10× | Fused | Win |
| Int64 (POD) | 1,528 | 584 | 328 | 2.61× | Fused | Win |
| String (POD) | 1,756 | 1,009 | 883 | 1.74× | Fused | Win |
| Uint64 (POD) | 2,925 | 801 | 397 | 3.65× | Fused | Win |
| Canada (minified) | 705 | 668 | 409 | 1.06× | Fused | Win |
| Canada (prettified) | 1,816 | 1,993 | 1,251 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,012 | 1,168 | 553 | 0.87× | Two-stage | Win |
| CitmCatalog (prettified) | 2,195 | 2,835 | 1,441 | 0.77× | Two-stage | Win |
| Discord (minified) | 1,395 | 1,252 | 760 | 1.11× | Fused | Win |
| Discord (prettified) | 1,682 | 1,960 | 1,215 | 0.86× | Two-stage | Win |
| Google Maps Response (minified) | 1,165 | 1,095 | 555 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,308 | 2,313 | 1,356 | 1.00× | Tie | Win |
| Instruments (minified) | 979 | 1,092 | 744 | 0.90× | Two-stage | Win |
| Instruments (prettified) | 1,575 | 1,957 | 1,386 | 0.80× | Two-stage | Win |
| Marine IK Reverse (minified) | 403 | 431 | 36 | 0.93× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,644 | 2,073 | 192 | 0.79× | Two-stage | Win |
| Marine IK (minified) | 401 | 432 | 269 | 0.93× | Two-stage | Win |
| Marine IK (prettified) | 1,679 | 2,133 | 1,328 | 0.79× | Two-stage | Win |
| Mesh (minified) | 520 | 629 | 452 | 0.83× | Two-stage | Win |
| Mesh (prettified) | 755 | 1,144 | 843 | 0.66× | Two-stage | Win |
| Random (minified) | 1,065 | 1,063 | 636 | 1.00× | Tie | Win |
| Random (prettified) | 1,555 | 1,869 | 1,142 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,703 | 1,508 | 1,015 | 1.13× | Fused | Win |
| Twitter (prettified) | 1,707 | 2,138 | 1,450 | 0.80× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,460 | 385 | 260 | 6.39× | Fused | Win |
| Double (POD) | 1,504 | 498 | 284 | 3.02× | Fused | Win |
| Int64 (POD) | 3,713 | 1,149 | 632 | 3.23× | Fused | Win |
| String (POD) | 2,612 | 1,570 | 1,373 | 1.66× | Fused | Win |
| Uint64 (POD) | n/c | n/c | n/c | — | — | — |
| Canada (minified) | 1,253 | 1,136 | 778 | 1.10× | Fused | Win |
| Canada (prettified) | 3,072 | 3,364 | 2,439 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,431 | 2,141 | 1,509 | 1.14× | Fused | Win |
| CitmCatalog (prettified) | 4,524 | 5,078 | 3,561 | 0.89× | Two-stage | Win |
| Discord (minified) | 2,673 | 2,336 | 2,063 | 1.14× | Fused | Win |
| Discord (prettified) | 2,496 | 3,663 | 2,991 | 0.68× | Two-stage | Win |
| Google Maps Response (minified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (prettified) | 3,977 | 5,033 | 3,835 | 0.79× | Two-stage | Win |
| Instruments (minified) | 3,094 | 2,325 | 2,029 | 1.33× | Fused | Win |
| Instruments (prettified) | 3,303 | 4,029 | 3,659 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 964 | 890 | 718 | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 3,781 | 3,970 | 3,297 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 996 | 903 | 682 | 1.10× | Fused | Win |
| Marine IK (prettified) | 3,812 | 4,064 | 3,350 | 0.94× | Two-stage | Win |
| Mesh (minified) | 1,513 | 1,393 | 1,352 | 1.09× | Fused | Win |
| Mesh (prettified) | 2,100 | 2,492 | 2,487 | 0.84× | Two-stage | Tie |
| Random (minified) | 2,147 | 2,145 | 1,541 | 1.00× | Tie | Win |
| Random (prettified) | 2,718 | 3,498 | 2,675 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,434 | 2,327 | 2,253 | 1.05× | Fused | Win |
| Twitter (prettified) | 2,434 | 3,288 | 3,182 | 0.74× | Two-stage | Win |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,408 | 275 | 179 | 8.75× | Fused | Win |
| Double (POD) | 1,441 | 368 | 225 | 3.92× | Fused | Win |
| Int64 (POD) | 3,666 | 848 | 519 | 4.32× | Fused | Win |
| String (POD) | 2,628 | 1,243 | 1,202 | 2.11× | Fused | Win |
| Uint64 (POD) | 3,596 | 858 | 507 | 4.19× | Fused | Win |
| Canada (minified) | 1,066 | 1,065 | 893 | 1.00× | Tie | Win |
| Canada (prettified) | 2,519 | 3,092 | 2,622 | 0.81× | Two-stage | Win |
| CitmCatalog (minified) | 2,888 | 2,036 | 1,633 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 4,264 | 4,751 | 3,994 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,901 | 2,145 | 1,679 | 1.35× | Fused | Win |
| Discord (prettified) | 2,792 | 3,353 | 2,672 | 0.83× | Two-stage | Win |
| Google Maps Response (minified) | 2,805 | 1,909 | 1,149 | 1.47× | Fused | Win |
| Google Maps Response (prettified) | 3,824 | 4,058 | 2,770 | 0.94× | Two-stage | Win |
| Instruments (minified) | 3,689 | 2,231 | 1,449 | 1.65× | Fused | Win |
| Instruments (prettified) | 3,580 | 3,772 | 2,653 | 0.95× | Two-stage | Win |
| Marine IK Reverse (minified) | 847 | 769 | 611 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 3,329 | 3,313 | 2,946 | 1.00× | Tie | Win |
| Marine IK (minified) | 870 | 758 | 636 | 1.15× | Fused | Win |
| Marine IK (prettified) | 3,701 | 3,701 | 3,158 | 1.00× | Tie | Win |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | 1,826 | 2,080 | 2,529 | 0.88× | Two-stage | **Loss** |
| Random (minified) | 1,994 | 1,867 | 1,140 | 1.07× | Fused | Win |
| Random (prettified) | 2,549 | 3,259 | 2,053 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,472 | 2,254 | 1,618 | 1.10× | Fused | Win |
| Twitter (prettified) | 2,343 | 2,909 | 2,270 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,525 | 187 | 189 | 8.15× | Fused | Win |
| Double (POD) | 991 | 230 | 117 | 4.30× | Fused | Win |
| Int64 (POD) | 2,648 | 636 | 442 | 4.16× | Fused | Win |
| String (POD) | 1,662 | 748 | 1,006 | 2.22× | Fused | Win |
| Uint64 (POD) | 2,543 | 606 | 443 | 4.19× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 2,378 | 1,474 | 1,002 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | n/c | n/c | n/c | — | — | — |
| Discord (minified) | 1,841 | 1,333 | 1,130 | 1.38× | Fused | Win |
| Discord (prettified) | 2,181 | 1,892 | 1,598 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,870 | 1,154 | 734 | 1.62× | Fused | Win |
| Google Maps Response (prettified) | 2,794 | 2,284 | 1,645 | 1.22× | Fused | Win |
| Instruments (minified) | 2,786 | 1,550 | 1,207 | 1.80× | Fused | Win |
| Instruments (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | 2,206 | 2,181 | 1,819 | 1.01× | Tie | Win |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | 1,273 | 1,485 | 1,492 | 0.86× | Two-stage | Tie |
| Random (minified) | n/c | n/c | n/c | — | — | — |
| Random (prettified) | 1,591 | 1,432 | 1,229 | 1.11× | Fused | Win |
| Twitter (minified) | 1,980 | 1,560 | 1,226 | 1.27× | Fused | Win |
| Twitter (prettified) | 2,432 | 2,233 | 1,707 | 1.09× | Fused | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,289 | 174 | 184 | 7.41× | Fused | Win |
| Double (POD) | 965 | 308 | 177 | 3.14× | Fused | Win |
| Int64 (POD) | 2,459 | 670 | 471 | 3.67× | Fused | Win |
| String (POD) | 1,616 | 720 | 1,128 | 2.24× | Fused | Win |
| Uint64 (POD) | 2,266 | 644 | 397 | 3.52× | Fused | Win |
| Canada (minified) | 822 | 719 | 437 | 1.14× | Fused | Win |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 2,171 | 1,144 | 719 | 1.90× | Fused | Win |
| CitmCatalog (prettified) | 3,142 | 2,679 | 1,911 | 1.17× | Fused | Win |
| Discord (minified) | 2,153 | 1,218 | 1,384 | 1.77× | Fused | Win |
| Discord (prettified) | 2,655 | 1,833 | 1,905 | 1.45× | Fused | Win |
| Google Maps Response (minified) | 2,047 | 1,171 | 874 | 1.75× | Fused | Win |
| Google Maps Response (prettified) | 3,012 | 2,254 | 1,966 | 1.34× | Fused | Win |
| Instruments (minified) | 2,643 | 1,362 | 1,192 | 1.94× | Fused | Win |
| Instruments (prettified) | 2,969 | 2,140 | 2,269 | 1.39× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | 763 | 641 | 434 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,726 | 2,605 | 1,922 | 1.05× | Fused | Win |
| Mesh (minified) | 1,168 | 909 | 803 | 1.28× | Fused | Win |
| Mesh (prettified) | 1,425 | 1,661 | 1,556 | 0.86× | Two-stage | Win |
| Random (minified) | 1,774 | 1,060 | 993 | 1.67× | Fused | Win |
| Random (prettified) | n/c | n/c | n/c | — | — | — |
| Twitter (minified) | 2,345 | 1,697 | 1,670 | 1.38× | Fused | Win |
| Twitter (prettified) | 2,530 | 1,754 | 2,118 | 1.44× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: the platform, and whether the document is indented. On the M1 the fused path wins all POD and nearly all minified input, and most prettified input. On Linux it wins POD and almost all minified input, and every prettified document goes to the tape. Under MSVC it loses half of the minified documents and every prettified one.

**POD-type tests: the fused path wins all 24, on every build.** It runs them 1.4× (String, Linux/Clang) to 8.0× (Bool, Linux/GCC) faster than the two-stage path, and 1.7× to 8.8× faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the compiler.

**Minified documents: the fused path's on Linux and the M1, split under MSVC.** Outside MSVC the fused path wins 27 of the 31 converged minified documents. Three are ties (Mesh on macOS/GCC, Marine IK Reverse on macOS/Clang and Canada on Linux/GCC) and one is a loss, Random on Linux/Clang, by 3%. Its margins run from 2% (Twitter on Linux/Clang) to 89% (Discord on macOS/Clang); on Linux/GCC they are widest on CitmCatalog (32%) and Instruments (52%). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order. Under MSVC the fused path wins four documents, Google Maps and Random by 3% each, Discord by 8% and Twitter by 11%, and ties Canada. The two-stage path wins the other five, by 5% (Marine IK Reverse) to 22% (Mesh).

**Prettified documents on x86: the two-stage path wins all 27.** It wins all ten under MSVC, by 1% (Google Maps) to 47% (Mesh); all ten under Linux/Clang, by 5% (Marine IK and Marine IK Reverse) to 39% (Discord); and all seven that converged under Linux/GCC, by 1% (Marine IK Reverse) to 29% (Twitter). The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That narrows the gap on some documents, but on none of the x86 builds does it close. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis.

**Prettified documents on the M1: mostly fused, with Mesh the exception.** The fused path wins 8 of the 11 converged documents, by 8% (Twitter on macOS/GCC) to 92% (Random on macOS/Clang), and Marine IK on macOS/GCC is a tie. The two-stage path wins Mesh on both compilers, by 28% on macOS/GCC and 27% on macOS/Clang. NEON largely escapes the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it.

**Mesh is the document the tape wins most often.** The two-stage path wins prettified Mesh on all five builds, by 12% (Linux/GCC) to 47% (Windows/MSVC). It also wins minified Mesh under MSVC by 22% and ties it on macOS/GCC, while the fused path wins minified Mesh on Linux/Clang (12%), Linux/GCC (7%) and macOS/Clang (37%). We have not profiled what is particular about this document.

**MSVC slows the fused path more than the tape.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the minified documents that converged on both builds, the fused path reaches a median 49% of its Linux/Clang throughput, the two-stage path 53% and simdjson 40%. The shortfall is uneven. On minified CitmCatalog the fused path reaches 44% of its Linux/Clang throughput while the two-stage path reaches 58% and simdjson 35%; on minified Marine IK the figures are 42%, 50% and 38%, and on minified Mesh 35%, 48% and 35%. Those are the documents on which the tape wins under MSVC by the widest margins (15%, 9% and 22%). On minified Canada, Discord and Twitter the fused path keeps as much as the tape (57% against 57%, 56% against 55%, 62% against 57%), and under MSVC it wins or ties all three. The two-stage path's stage 1 is branch-free intrinsic code whose shape is fixed by the source, while the fused path and simdjson's On Demand walk are both branch-heavy code whose performance rests on the optimizer. We take that difference in exposure to the compiler to be why MSVC moves the balance toward the tape, but we have not isolated it.

**Against simdjson, Jsonifier wins 99 of 103 tests with whichever path is faster.** It ties one, prettified Twitter on Linux/Clang, and loses three, all Mesh on GCC: both forms on Linux/GCC and the minified form on macOS/GCC. The fused path alone wins 92, ties 2 and loses 9: prettified Mesh on every build, minified Mesh on Linux/GCC, and prettified Discord, Instruments and Twitter on Linux/Clang.

**Reverse key order: MSVC is where it bites.** Requesting every key in reverse order forces simdjson's On Demand API into the rescanning behavior described in §3. Under MSVC simdjson manages 36 MB/s on minified Marine IK Reverse against 394 MB/s for the two-stage path, 11× slower, and 191 MB/s on the prettified version against 1,936 MB/s, 10× slower. On the other builds where Marine IK Reverse converged, the gap is much smaller: the faster Jsonifier path is 1.1× to 1.4× faster than simdjson in both runs.

The routing implication is sharper than §2's rule of thumb. On the M1, the fused path is the right default for POD and minified input and for prettified input other than Mesh. On Linux it is right for POD and minified input, and prettified input should go to the two-stage path. Under MSVC it is right for POD-type data and four of the minified documents, and prettified input should go to the two-stage path. Routing each test to its faster path would raise the record against simdjson from 92 wins, 2 ties and 9 losses to 99, 1 and 3. Nine tests change hands, eight of them prettified documents and five of them on Linux/Clang; the ninth, minified Mesh on macOS/GCC, goes from a tie to a loss. The router does not yet take the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents, not the POD-type tests. The tables list every test where both converged.

#### Freshly allocated objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 887 | 724 | 1.23× | Win |
| Canada (prettified) | 2,606 | 2,138 | 1.22× | Win |
| CitmCatalog (minified) | 1,714 | 1,157 | 1.48× | Win |
| CitmCatalog (prettified) | 4,124 | 2,922 | 1.41× | Win |
| Google Maps Response (prettified) | 3,809 | 2,994 | 1.27× | Win |
| Instruments (minified) | 2,016 | 1,731 | 1.16× | Win |
| Marine IK Reverse (minified) | 705 | 660 | 1.07× | Win |
| Marine IK Reverse (prettified) | 3,075 | 2,943 | 1.04× | Win |
| Marine IK (minified) | 717 | 649 | 1.10× | Win |
| Mesh (minified) | 1,077 | 1,194 | 0.90× | **Loss** |
| Mesh (prettified) | 2,095 | 2,153 | 0.97× | **Loss** |
| Random (minified) | 1,314 | 1,222 | 1.07× | Win |
| Random (prettified) | 2,408 | 2,170 | 1.11× | Win |
| Twitter (minified) | 1,712 | 1,888 | 0.91× | **Loss** |
| Twitter (prettified) | 2,575 | 2,539 | 1.01× | Tie |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| CitmCatalog (minified) | 1,275 | 924 | 1.38× | Win |
| CitmCatalog (prettified) | 2,567 | 1,951 | 1.32× | Win |
| Discord (minified) | 1,076 | 1,009 | 1.07× | Win |
| Discord (prettified) | 1,560 | 1,450 | 1.08× | Win |
| Google Maps Response (minified) | 1,017 | 879 | 1.16× | Win |
| Google Maps Response (prettified) | 1,911 | 1,928 | 0.99× | **Loss** |
| Instruments (minified) | 1,410 | 1,141 | 1.24× | Win |
| Marine IK (prettified) | 1,934 | 1,794 | 1.08× | Win |
| Mesh (minified) | 744 | 564 | 1.32× | Win |
| Mesh (prettified) | 1,402 | 911 | 1.54× | Win |
| Random (minified) | 688 | 661 | 1.04× | Win |
| Random (prettified) | 978 | 1,100 | 0.89× | **Loss** |
| Twitter (minified) | 1,332 | 1,113 | 1.20× | Win |
| Twitter (prettified) | 1,835 | 1,577 | 1.16× | Win |

#### Reused objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,065 | 829 | 1.29× | Win |
| Canada (prettified) | 3,092 | 2,474 | 1.25× | Win |
| CitmCatalog (minified) | 2,036 | 1,330 | 1.53× | Win |
| CitmCatalog (prettified) | 4,751 | 3,290 | 1.44× | Win |
| Discord (minified) | 2,145 | 1,720 | 1.25× | Win |
| Discord (prettified) | 3,353 | 2,717 | 1.23× | Win |
| Google Maps Response (minified) | 1,909 | 1,342 | 1.42× | Win |
| Google Maps Response (prettified) | 4,058 | 3,095 | 1.31× | Win |
| Instruments (minified) | 2,231 | 1,777 | 1.26× | Win |
| Instruments (prettified) | 3,772 | 3,224 | 1.17× | Win |
| Marine IK Reverse (minified) | 769 | 728 | 1.06× | Win |
| Marine IK Reverse (prettified) | 3,313 | 3,184 | 1.04× | Win |
| Marine IK (minified) | 758 | 718 | 1.06× | Win |
| Marine IK (prettified) | 3,701 | 3,433 | 1.08× | Win |
| Mesh (prettified) | 2,080 | 2,381 | 0.87× | **Loss** |
| Random (minified) | 1,867 | 1,506 | 1.24× | Win |
| Random (prettified) | 3,259 | 2,671 | 1.22× | Win |
| Twitter (minified) | 2,254 | 2,155 | 1.05× | Win |
| Twitter (prettified) | 2,909 | 3,123 | 0.93× | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| CitmCatalog (minified) | 1,474 | 1,081 | 1.36× | Win |
| Discord (minified) | 1,333 | 1,129 | 1.18× | Win |
| Discord (prettified) | 1,892 | 1,625 | 1.16× | Win |
| Google Maps Response (minified) | 1,154 | 938 | 1.23× | Win |
| Google Maps Response (prettified) | 2,284 | 2,043 | 1.12× | Win |
| Instruments (minified) | 1,550 | 1,183 | 1.31× | Win |
| Marine IK (prettified) | 2,181 | 1,955 | 1.12× | Win |
| Mesh (prettified) | 1,485 | 1,101 | 1.35× | Win |
| Random (prettified) | 1,432 | 1,204 | 1.19× | Win |
| Twitter (minified) | 1,560 | 1,252 | 1.25× | Win |
| Twitter (prettified) | 2,233 | 1,754 | 1.27× | Win |

In the freshly allocated run, two-stage Jsonifier wins 23 of these 29 tests, ties 1 and loses 5. On macOS/GCC it wins 12 of 14, by 4% (Random, minified) to 54% (Mesh, prettified), and is slower on prettified Google Maps (by 1%) and prettified Random (by 11%). On Linux/GCC it wins 11 of 15, by 4% (Marine IK Reverse, prettified) to 48% (CitmCatalog, minified), ties Twitter (prettified), and is slower on Mesh in both forms (by 10% minified and 3% prettified) and on Twitter (minified) by 9%. Mesh is also where Jsonifier loses to simdjson On Demand on that build (§2.4), so its losses to the reflection reader look like a property of the document under GCC on x86 rather than of the tape; we have not profiled them. With the fused path instead of the two-stage one, Jsonifier beats the reflection reader on 11 of 15 tests on Linux/GCC and on all 14 on macOS/GCC. With objects reused, two-stage Jsonifier wins 28 of 30: on Linux/GCC it wins 17 of 19 and is slower on prettified Mesh (by 13%) and prettified Twitter (by 7%), and on macOS/GCC it wins all 11.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row, and the two rows differ by at most 7% across all 18 converged results.

#### Freshly allocated objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 5,034 | 1,925 | 2.62× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 6,757 | 2,675 | 2.53× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 7,038 | 5,895 | 1.19× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | n/c | n/c | — | — |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | minified | 7,775 | 5,590 | 1.39× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | prettified | 9,112 | 7,473 | 1.22× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 3,886 | 2,819 | 1.38× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | 4,059 | 3,630 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | n/c | n/c | — | — |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,400 | 3,875 | 1.14× | Win |

#### Reused objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 5,294 | 1,999 | 2.65× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 7,024 | 2,748 | 2.56× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 7,070 | 6,198 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | 9,431 | 7,556 | 1.25× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | minified | 8,179 | 6,027 | 1.36× | Win |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | prettified | 9,471 | 7,465 | 1.27× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 4,011 | 2,599 | 1.54× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | 4,634 | 3,558 | 1.30× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | 3,700 | 2,808 | 1.32× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,567 | 4,053 | 1.13× | Win |

Jsonifier wins all 18 converged results, by 1.12× (prettified on macOS/GCC, freshly allocated) to 2.65× (minified under MSVC, reused). Throughput here is counted over the whole document, so these figures run well above the full-parse ones: on minified Twitter the fused path parses at 1,207 MB/s under MSVC and 1,960 MB/s on Linux/Clang when it materializes every value, against 5,034 and 7,038 MB/s here, because the bytes the caller does not ask for are skipped through the tape instead of being walked. The margin over simdjson is 2.5× to 2.7× under MSVC and 1.1× to 1.5× elsewhere. On minified Twitter in the freshly allocated run, simdjson On Demand under MSVC reaches 33% of its Linux/Clang throughput and Jsonifier's stage 1 + 2 path reaches 72%. This is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

## 3. The single-pass path: `json_iterator` over raw text

The fused path is implemented as `json_iterator<parseOpts, read_buffer_ptr, string_buffer_type>` — an iterator holding a raw `read_buffer_ptr` cursor plus depth counters, walking the document exactly once. Several design decisions distinguish it:

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

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `indentSize * currentDepth()`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

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

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA reflects real differences in how each compiler schedules the unrolled block bodies: on AVX2 Clang takes four blocks (256 bytes) per step where GCC and MSVC take eight (512 bytes), and on NEON Clang uses a drain burst of 4 where GCC uses 8. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations — a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(simd_array_t in_01, jsonifier_simd_int_t bsRegister, jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNextInString() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit — a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional — `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString`. Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4` — the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm — `followsNonquoteScalar` carries the cross-block bit — so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(simd_array_t in_01, jsonifier_simd_int_t opTable, jsonifier_simd_int_t spaceMask, jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace	  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
	const uint64_t op			  = simd::op_collector::impl(in_01, opTable, spaceMask);
	const uint64_t scalar		  = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t nonquoteScalar = scalar & ~simd::rope_detector<rope_block>::quotes;
	const uint64_t follows		  = simd::rope_detector<rope_block>::followsNonquoteScalar(nonquoteScalar);
	const uint64_t scalarStart	  = scalar & ~follows;
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
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep`, because the two constants interact. AVX2 uses a burst of 4 on all three compilers, with 4 blocks per step on Clang and 8 on GCC and MSVC; NEON uses a burst of 4 on Clang and 8 on GCC, at 4 blocks per step on both (§4.1).
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step — up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(array<uint64_t, simdBlocksPerStep> bitsArr, array<uint64_t, simdBlocksPerStep> cnts, structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains — each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser — it is a *specialization* of the same `json_iterator` interface over `structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the iterator concept; a `structural_context` trait switches the token-navigation primitives:

```cpp
JSONIFIER_INLINE bool skipValue() noexcept {
	if (iter >= endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>();
	}
	const char first = static_cast<char>(*currentPtr());
	if (first == '{' || first == '[') {
		int64_t depth{};
		while (iter < endIter) {
			depth += nestingDeltaTable[static_cast<uint8_t>(stringRootIter[*iter])];
			++iter;
			if (depth == 0) {
				return true;
			}
		}
		return reject<parse_statuses::unexpected_string_end>();
	}
	++iter;
	return true;
}
```

`nestingDeltaTable` maps `{` and `[` to +1, `}` and `]` to −1 and every other byte to 0, so the loop needs one table load and one add per tape entry and a single branch on the running depth.

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per structural entry — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer iterator, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on minified documents outside MSVC and on nearly all documents on the M1; slower on prettified x86 input and on five of ten minified documents under MSVC (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared iterator concept |
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

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head, on both freshly allocated and reused target objects. Across 103 freshly allocated tests on five platforms the fused path wins 63, ties 5 and loses 35, and where it loses is a matter of platform more than of principle. It wins all 24 POD-type tests, by up to 8.0× (Bool on Linux/GCC), 27 of the 31 minified documents outside MSVC, and 8 of the 11 prettified documents on the M1. The two-stage path wins all 27 prettified documents on x86, and under MSVC it also takes five of the ten minified documents. The tape, in other words, is not only an index for partial reads: on some compilers and some layouts it is also the faster way to read the whole book, and the router should learn which. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 99 of those 103 tests and ties one, its own stage-1 + reflection path beats simdjson's stage-1 + reflection reader on 23 of 29, and on the partial-reading test, where the index is the right tool, it beats simdjson On Demand on all 18 converged results. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*