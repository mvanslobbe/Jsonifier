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

For full-document parsing into known types, the tape is overhead in principle, but whether skipping it is faster in practice turns out to depend on the compiler and on the shape of the input. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. The fused path wins every POD-type test and every minified document on Linux and macOS, and most prettified documents on the M1. The two-stage path wins most prettified documents on x86, and seven of the ten minified documents under MSVC.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 3, 2026, with Jsonifier [4724a1a](https://github.com/nihilai-collective/jsonifier/commit/4724a1a), simdjson [2a690bc](https://github.com/simdjson/simdjson/commit/2a690bc) and BenchmarkSuite [4e7c701](https://github.com/nihilai-collective/benchmarksuite/commit/4e7c701). The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep but is left out here, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3 and §2.5 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, all libraries receive keys in document order. The sweep also runs "Small" cut-down copies of each document (at most 5 KiB minified) that measure per-call overhead; they are not counted here.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on x86 (10% and 5% on the virtualized M1), and a test is ranked only if every library in it converges, which is why some platforms have fewer than 25 tests. Ties are declared by Welch's t-test on the kept epoch. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to every library. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced three ties between the two Jsonifier paths across 107 tests.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 67 tests, three are statistical ties, and the two-stage path wins 37. The split is almost entirely a matter of platform:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 8 | 0 | 17 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 13 | 0 | 9 | 22 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 17 | 2 | 6 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 18 | 1 | 2 | 21 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 11 | 0 | 3 | 14 of 25 |
| **Aggregate** | **67** | **3** | **37** | **107 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 7 / 0 / 0 |
| Minified corpus documents | 3 / 0 / 7 | 17 / 0 / 0 | 14 / 0 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 3 / 2 / 15 | 8 / 1 / 5 |

Against simdjson, counting a test for Jsonifier when either of its paths ranks above simdjson:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 | 22 / 0 / 0 | 17 / 0 / 5 |
| Linux / GCC 16.1 | 22 / 1 / 2 | 22 / 0 / 3 |
| macOS / GCC 16.2 | 20 / 0 / 1 | 20 / 0 / 1 |
| macOS / Clang 23.1 | 13 / 1 / 0 | 13 / 1 / 0 |
| **Aggregate** | **102 / 2 / 3** | **96 / 1 / 10** |

**Reused objects.** With the target object held across iterations, the picture is the same:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 8 | 0 | 17 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 13 | 1 | 9 | 23 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 16 | 5 | 4 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 19 | 0 | 2 | 21 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 16 | 0 | 1 | 17 of 25 |
| **Aggregate** | **72** | **6** | **33** | **111 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 9 / 0 / 0 |
| Minified corpus documents | 3 / 0 / 7 | 17 / 1 / 0 | 15 / 0 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 2 / 5 / 13 | 11 / 0 / 3 |

Against simdjson, counting a test for Jsonifier when either of its paths ranks above simdjson:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 | 25 / 0 / 0 | 24 / 0 / 1 |
| Linux / Clang 24.0 | 20 / 0 / 3 | 17 / 1 / 5 |
| Linux / GCC 16.1 | 21 / 0 / 4 | 21 / 0 / 4 |
| macOS / GCC 16.2 | 20 / 0 / 1 | 20 / 0 / 1 |
| macOS / Clang 23.1 | 17 / 0 / 0 | 17 / 0 / 0 |
| **Aggregate** | **103 / 0 / 8** | **99 / 1 / 11** |

Across 111 converged tests the fused path wins 72, ties 6 and loses 33 to the two-stage path, and the platform split barely moves: MSVC is again 8 / 0 / 17, while macOS/Clang, with more tests converging, goes 16 / 0 / 1. Against simdjson, the faster Jsonifier path wins 103 and loses 8, and the fused path alone wins 99, ties 1 and loses 11. Taking allocation out of the timed region helps every library, so it narrows Jsonifier's lead slightly but does not change which path is faster.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two ranked higher, and "n/c" means the test did not converge for at least one library.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 506 | 154 | 129 | 3.28× | Fused | Win |
| Double (POD) | 576 | 299 | 170 | 1.93× | Fused | Win |
| Int64 (POD) | 1,802 | 665 | 391 | 2.71× | Fused | Win |
| String (POD) | 1,021 | 726 | 632 | 1.41× | Fused | Win |
| Uint64 (POD) | 2,159 | 740 | 398 | 2.92× | Fused | Win |
| Canada (minified) | 583 | 567 | 370 | 1.03× | Fused | Win |
| Canada (prettified) | 1,588 | 1,704 | 1,112 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 901 | 1,062 | 517 | 0.85× | Two-stage | Win |
| CitmCatalog (prettified) | 2,032 | 2,616 | 1,386 | 0.78× | Two-stage | Win |
| Discord (minified) | 1,181 | 1,115 | 615 | 1.06× | Fused | Win |
| Discord (prettified) | 1,534 | 1,773 | 1,002 | 0.87× | Two-stage | Win |
| Google Maps Response (minified) | 1,053 | 1,076 | 522 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,130 | 2,338 | 1,255 | 0.91× | Two-stage | Win |
| Instruments (minified) | 966 | 1,091 | 683 | 0.89× | Two-stage | Win |
| Instruments (prettified) | 1,498 | 1,897 | 1,283 | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 384 | 414 | 36 | 0.93× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,606 | 2,014 | 191 | 0.80× | Two-stage | Win |
| Marine IK (minified) | 388 | 425 | 254 | 0.91× | Two-stage | Win |
| Marine IK (prettified) | 1,600 | 1,994 | 1,257 | 0.80× | Two-stage | Win |
| Mesh (minified) | 501 | 611 | 409 | 0.82× | Two-stage | Win |
| Mesh (prettified) | 743 | 1,093 | 764 | 0.68× | Two-stage | Win |
| Random (minified) | 922 | 940 | 524 | 0.98× | Two-stage | Win |
| Random (prettified) | 1,397 | 1,683 | 963 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,456 | 1,275 | 774 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,537 | 1,857 | 1,144 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,110 | 353 | 222 | 5.97× | Fused | Win |
| Double (POD) | 1,354 | 445 | 276 | 3.05× | Fused | Win |
| Int64 (POD) | 3,283 | 1,026 | 590 | 3.20× | Fused | Win |
| String (POD) | 1,721 | 1,123 | 1,142 | 1.53× | Fused | Win |
| Uint64 (POD) | 3,549 | 972 | 600 | 3.65× | Fused | Win |
| Canada (minified) | 1,020 | 924 | 647 | 1.10× | Fused | Win |
| Canada (prettified) | 2,438 | 2,500 | 1,929 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 1,757 | 1,613 | 1,190 | 1.09× | Fused | Win |
| CitmCatalog (prettified) | 3,642 | 3,767 | 2,968 | 0.97× | Two-stage | Win |
| Discord (minified) | 1,897 | 1,615 | 1,428 | 1.17× | Fused | Win |
| Discord (prettified) | 2,112 | 2,614 | 2,327 | 0.81× | Two-stage | Win |
| Google Maps Response (minified) | n/c | n/c | n/c | — | — | — |
| Google Maps Response (prettified) | 3,252 | 3,541 | 3,329 | 0.92× | Two-stage | Win |
| Instruments (minified) | 2,665 | 2,017 | 1,768 | 1.32× | Fused | Win |
| Instruments (prettified) | 2,830 | 3,355 | 3,012 | 0.84× | Two-stage | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | 3,224 | 3,333 | 2,715 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 810 | 707 | 608 | 1.15× | Fused | Win |
| Marine IK (prettified) | 3,203 | 3,111 | 2,627 | 1.03× | Fused | Win |
| Mesh (minified) | 1,326 | 1,129 | 1,152 | 1.17× | Fused | Win |
| Mesh (prettified) | 1,755 | 2,100 | 1,973 | 0.84× | Two-stage | Win |
| Random (minified) | 1,400 | 1,178 | 1,062 | 1.19× | Fused | Win |
| Random (prettified) | 1,879 | 2,195 | 1,811 | 0.86× | Two-stage | Win |
| Twitter (minified) | n/c | n/c | n/c | — | — | — |
| Twitter (prettified) | 1,852 | 2,410 | 2,364 | 0.77× | Two-stage | Win |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,240 | 347 | 186 | 6.46× | Fused | Win |
| Double (POD) | 1,323 | 418 | 213 | 3.16× | Fused | Win |
| Int64 (POD) | 3,390 | 964 | 576 | 3.52× | Fused | Win |
| String (POD) | 1,787 | 920 | 979 | 1.94× | Fused | Win |
| Uint64 (POD) | 3,429 | 1,010 | 569 | 3.39× | Fused | Win |
| Canada (minified) | 897 | 859 | 747 | 1.04× | Fused | Win |
| Canada (prettified) | 2,339 | 2,385 | 2,160 | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,762 | 1,539 | 1,325 | 1.14× | Fused | Win |
| CitmCatalog (prettified) | 3,578 | 3,548 | 3,319 | 1.01× | Tie | Win |
| Discord (minified) | 1,891 | 1,577 | 1,423 | 1.20× | Fused | Win |
| Discord (prettified) | 2,339 | 2,466 | 2,215 | 0.95× | Two-stage | Win |
| Google Maps Response (minified) | 1,625 | 1,502 | 1,108 | 1.08× | Fused | Win |
| Google Maps Response (prettified) | 3,462 | 3,590 | 2,678 | 0.96× | Two-stage | Win |
| Instruments (minified) | 3,012 | 1,996 | 1,304 | 1.51× | Fused | Win |
| Instruments (prettified) | 3,198 | 3,451 | 2,420 | 0.93× | Two-stage | Win |
| Marine IK Reverse (minified) | 765 | 682 | 598 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 3,086 | 3,006 | 2,745 | 1.03× | Fused | Win |
| Marine IK (minified) | 775 | 687 | 597 | 1.13× | Fused | Win |
| Marine IK (prettified) | 3,145 | 3,060 | 2,710 | 1.03× | Fused | Win |
| Mesh (minified) | 1,054 | 1,001 | 1,181 | 1.05× | Fused | **Loss** |
| Mesh (prettified) | 1,691 | 1,874 | 2,186 | 0.90× | Two-stage | **Loss** |
| Random (minified) | 1,340 | 1,114 | 1,073 | 1.20× | Fused | Win |
| Random (prettified) | 1,858 | 1,901 | 1,896 | 0.98× | Two-stage | Tie |
| Twitter (minified) | 1,911 | 1,365 | 1,174 | 1.40× | Fused | Win |
| Twitter (prettified) | 1,856 | 1,907 | 1,640 | 0.97× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,357 | 196 | 180 | 6.93× | Fused | Win |
| Double (POD) | 858 | 272 | 134 | 3.16× | Fused | Win |
| Int64 (POD) | 2,008 | 569 | 335 | 3.53× | Fused | Win |
| String (POD) | n/c | n/c | n/c | — | — | — |
| Uint64 (POD) | 2,099 | 599 | 411 | 3.50× | Fused | Win |
| Canada (minified) | 641 | 601 | 442 | 1.07× | Fused | Win |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 1,792 | 1,149 | 961 | 1.56× | Fused | Win |
| CitmCatalog (prettified) | 2,706 | 2,266 | 2,171 | 1.19× | Fused | Win |
| Discord (minified) | 1,534 | 1,082 | 890 | 1.42× | Fused | Win |
| Discord (prettified) | 1,827 | 1,721 | 1,338 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 1,405 | 1,030 | 709 | 1.36× | Fused | Win |
| Google Maps Response (prettified) | 2,243 | 2,286 | 1,551 | 0.98× | Two-stage | Win |
| Instruments (minified) | 2,484 | 1,386 | 1,044 | 1.79× | Fused | Win |
| Instruments (prettified) | 2,759 | 2,534 | 1,762 | 1.09× | Fused | Win |
| Marine IK Reverse (minified) | 566 | 479 | 405 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,192 | 2,115 | 1,760 | 1.04× | Fused | Win |
| Marine IK (minified) | 599 | 506 | 421 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,322 | 2,180 | 1,775 | 1.07× | Fused | Win |
| Mesh (minified) | 906 | 694 | 825 | 1.30× | Fused | Win |
| Mesh (prettified) | 1,234 | 1,269 | 1,427 | 0.97× | Two-stage | **Loss** |
| Random (minified) | 898 | 723 | 607 | 1.24× | Fused | Win |
| Random (prettified) | 1,280 | 1,275 | 1,060 | 1.00× | Tie | Win |
| Twitter (minified) | n/c | n/c | n/c | — | — | — |
| Twitter (prettified) | n/c | n/c | n/c | — | — | — |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | n/c | n/c | n/c | — | — | — |
| Double (POD) | 741 | 203 | 135 | 3.66× | Fused | Win |
| Int64 (POD) | 1,910 | 601 | 402 | 3.18× | Fused | Win |
| String (POD) | n/c | n/c | n/c | — | — | — |
| Uint64 (POD) | 2,068 | 538 | 385 | 3.85× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (prettified) | n/c | n/c | n/c | — | — | — |
| Discord (minified) | 2,341 | 1,445 | 1,347 | 1.62× | Fused | Win |
| Discord (prettified) | 2,110 | 1,814 | 2,099 | 1.16× | Fused | Tie |
| Google Maps Response (minified) | 1,921 | 1,330 | 1,025 | 1.44× | Fused | Win |
| Google Maps Response (prettified) | 2,705 | 2,856 | 2,351 | 0.95× | Two-stage | Win |
| Instruments (minified) | 2,583 | 1,548 | 1,150 | 1.67× | Fused | Win |
| Instruments (prettified) | 3,104 | 2,610 | 2,243 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | 1,395 | 1,481 | 1,284 | 0.94× | Two-stage | Win |
| Random (minified) | 1,294 | 1,006 | 801 | 1.29× | Fused | Win |
| Random (prettified) | 1,830 | 1,917 | 1,416 | 0.95× | Two-stage | Win |
| Twitter (minified) | 2,109 | 1,549 | 1,465 | 1.36× | Fused | Win |
| Twitter (prettified) | 2,375 | 2,108 | 2,140 | 1.13× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 539 | 158 | 129 | 3.41× | Fused | Win |
| Double (POD) | 675 | 331 | 181 | 2.04× | Fused | Win |
| Int64 (POD) | 2,245 | 726 | 416 | 3.09× | Fused | Win |
| String (POD) | 1,767 | 1,026 | 877 | 1.72× | Fused | Win |
| Uint64 (POD) | 2,923 | 814 | 420 | 3.59× | Fused | Win |
| Canada (minified) | 703 | 665 | 408 | 1.06× | Fused | Win |
| Canada (prettified) | 1,775 | 1,926 | 1,238 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 978 | 1,165 | 555 | 0.84× | Two-stage | Win |
| CitmCatalog (prettified) | 2,173 | 2,818 | 1,463 | 0.77× | Two-stage | Win |
| Discord (minified) | 1,341 | 1,245 | 757 | 1.08× | Fused | Win |
| Discord (prettified) | 1,692 | 2,022 | 1,203 | 0.84× | Two-stage | Win |
| Google Maps Response (minified) | 1,160 | 1,214 | 562 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 2,322 | 2,576 | 1,394 | 0.90× | Two-stage | Win |
| Instruments (minified) | 983 | 1,118 | 771 | 0.88× | Two-stage | Win |
| Instruments (prettified) | 1,547 | 1,951 | 1,421 | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 412 | 441 | 36 | 0.93× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,701 | 2,141 | 192 | 0.79× | Two-stage | Win |
| Marine IK (minified) | 412 | 439 | 266 | 0.94× | Two-stage | Win |
| Marine IK (prettified) | 1,700 | 2,157 | 1,273 | 0.79× | Two-stage | Win |
| Mesh (minified) | 515 | 629 | 434 | 0.82× | Two-stage | Win |
| Mesh (prettified) | 766 | 1,147 | 820 | 0.67× | Two-stage | Win |
| Random (minified) | 1,075 | 1,099 | 636 | 0.98× | Two-stage | Win |
| Random (prettified) | 1,566 | 1,950 | 1,147 | 0.80× | Two-stage | Win |
| Twitter (minified) | 1,667 | 1,445 | 958 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,688 | 2,091 | 1,401 | 0.81× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,224 | 353 | 234 | 6.29× | Fused | Win |
| Double (POD) | 1,415 | 456 | 237 | 3.11× | Fused | Win |
| Int64 (POD) | 3,596 | 1,032 | 616 | 3.48× | Fused | Win |
| String (POD) | 2,502 | 1,445 | 1,456 | 1.73× | Fused | Win |
| Uint64 (POD) | 3,856 | 1,019 | 625 | 3.78× | Fused | Win |
| Canada (minified) | 1,258 | 1,115 | 761 | 1.13× | Fused | Win |
| Canada (prettified) | 2,803 | 2,930 | 2,219 | 0.96× | Two-stage | Win |
| CitmCatalog (minified) | 2,028 | 1,828 | 1,358 | 1.11× | Fused | Win |
| CitmCatalog (prettified) | 3,988 | 4,256 | 3,485 | 0.94× | Two-stage | Win |
| Discord (minified) | 2,389 | 1,816 | 1,817 | 1.32× | Fused | Win |
| Discord (prettified) | 2,303 | 3,025 | 2,954 | 0.76× | Two-stage | Win |
| Google Maps Response (minified) | 1,966 | 1,742 | 1,528 | 1.13× | Fused | Win |
| Google Maps Response (prettified) | 3,427 | 3,772 | 3,464 | 0.91× | Two-stage | Win |
| Instruments (minified) | 2,769 | 2,033 | 2,038 | 1.36× | Fused | Win |
| Instruments (prettified) | 2,726 | 3,411 | 3,523 | 0.80× | Two-stage | **Loss** |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | 3,401 | 3,566 | 2,953 | 0.95× | Two-stage | Win |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | 3,503 | 3,452 | 2,948 | 1.01× | Tie | Win |
| Mesh (minified) | 1,417 | 1,294 | 1,292 | 1.09× | Fused | Win |
| Mesh (prettified) | 1,852 | 2,102 | 2,208 | 0.88× | Two-stage | **Loss** |
| Random (minified) | 1,668 | 1,478 | 1,420 | 1.13× | Fused | Win |
| Random (prettified) | 2,163 | 2,406 | 2,362 | 0.90× | Two-stage | Win |
| Twitter (minified) | 2,214 | 1,841 | 2,037 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,056 | 2,624 | 2,916 | 0.78× | Two-stage | **Loss** |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,472 | 350 | 186 | 7.06× | Fused | Win |
| Double (POD) | 1,419 | 422 | 235 | 3.36× | Fused | Win |
| Int64 (POD) | 3,669 | 975 | 582 | 3.76× | Fused | Win |
| String (POD) | 2,665 | 1,226 | 1,239 | 2.17× | Fused | Win |
| Uint64 (POD) | 3,625 | 1,074 | 608 | 3.37× | Fused | Win |
| Canada (minified) | 1,089 | 1,043 | 895 | 1.04× | Fused | Win |
| Canada (prettified) | 2,704 | 2,897 | 2,613 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,035 | 1,758 | 1,589 | 1.16× | Fused | Win |
| CitmCatalog (prettified) | 3,966 | 3,985 | 3,924 | 1.00× | Tie | Win |
| Discord (minified) | 2,139 | 1,836 | 1,853 | 1.17× | Fused | Win |
| Discord (prettified) | 2,558 | 2,757 | 2,839 | 0.93× | Two-stage | **Loss** |
| Google Maps Response (minified) | 1,738 | 1,626 | 1,204 | 1.07× | Fused | Win |
| Google Maps Response (prettified) | 3,676 | 3,684 | 2,791 | 1.00× | Tie | Win |
| Instruments (minified) | 3,145 | 2,084 | 1,402 | 1.51× | Fused | Win |
| Instruments (prettified) | 3,304 | 3,599 | 2,564 | 0.92× | Two-stage | Win |
| Marine IK Reverse (minified) | 841 | 734 | 650 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 3,355 | 3,265 | 3,015 | 1.03× | Fused | Win |
| Marine IK (minified) | 859 | 752 | 657 | 1.14× | Fused | Win |
| Marine IK (prettified) | 3,456 | 3,317 | 3,017 | 1.04× | Fused | Win |
| Mesh (minified) | 1,093 | 1,099 | 1,320 | 0.99× | Tie | **Loss** |
| Mesh (prettified) | 1,773 | 1,971 | 2,430 | 0.90× | Two-stage | **Loss** |
| Random (minified) | 1,616 | 1,306 | 1,377 | 1.24× | Fused | Win |
| Random (prettified) | 2,169 | 2,140 | 2,422 | 1.01× | Tie | **Loss** |
| Twitter (minified) | 2,241 | 1,513 | 1,368 | 1.48× | Fused | Win |
| Twitter (prettified) | 2,059 | 2,041 | 1,873 | 1.01× | Tie | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,511 | 152 | 187 | 9.96× | Fused | Win |
| Double (POD) | 878 | 300 | 107 | 2.93× | Fused | Win |
| Int64 (POD) | 2,595 | 470 | 357 | 5.52× | Fused | Win |
| String (POD) | 1,579 | 675 | 948 | 2.34× | Fused | Win |
| Uint64 (POD) | 2,497 | 654 | 417 | 3.82× | Fused | Win |
| Canada (minified) | 789 | 748 | 490 | 1.06× | Fused | Win |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 2,159 | 1,297 | 1,066 | 1.66× | Fused | Win |
| CitmCatalog (prettified) | 3,370 | 2,928 | 2,379 | 1.15× | Fused | Win |
| Discord (minified) | 1,859 | 1,250 | 1,066 | 1.49× | Fused | Win |
| Discord (prettified) | 2,014 | 1,841 | 1,610 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 1,594 | 1,105 | 724 | 1.44× | Fused | Win |
| Google Maps Response (prettified) | 2,425 | 2,444 | 1,631 | 0.99× | Two-stage | Win |
| Instruments (minified) | 2,714 | 1,617 | 1,181 | 1.68× | Fused | Win |
| Instruments (prettified) | 2,885 | 2,482 | 1,958 | 1.16× | Fused | Win |
| Marine IK Reverse (minified) | 630 | 479 | 446 | 1.32× | Fused | Win |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | 2,525 | 2,475 | 1,902 | 1.02× | Fused | Win |
| Mesh (minified) | 970 | 875 | 873 | 1.11× | Fused | Win |
| Mesh (prettified) | 1,255 | 1,501 | 1,556 | 0.84× | Two-stage | **Loss** |
| Random (minified) | 1,090 | 844 | 718 | 1.29× | Fused | Win |
| Random (prettified) | 1,572 | 1,449 | 1,251 | 1.08× | Fused | Win |
| Twitter (minified) | 1,830 | 1,261 | 951 | 1.45× | Fused | Win |
| Twitter (prettified) | n/c | n/c | n/c | — | — | — |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,188 | 181 | 177 | 6.55× | Fused | Win |
| Double (POD) | 826 | 318 | 175 | 2.60× | Fused | Win |
| Int64 (POD) | 2,356 | 647 | 433 | 3.64× | Fused | Win |
| String (POD) | 1,721 | 726 | 923 | 2.37× | Fused | Win |
| Uint64 (POD) | n/c | n/c | n/c | — | — | — |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | n/c | n/c | n/c | — | — | — |
| CitmCatalog (minified) | 2,424 | 1,174 | 973 | 2.06× | Fused | Win |
| CitmCatalog (prettified) | 2,812 | 3,357 | 2,474 | 0.84× | Two-stage | Win |
| Discord (minified) | 2,298 | 1,427 | 1,533 | 1.61× | Fused | Win |
| Discord (prettified) | 2,591 | 2,310 | 2,357 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 2,045 | 1,390 | 1,081 | 1.47× | Fused | Win |
| Google Maps Response (prettified) | 3,220 | 2,926 | 2,353 | 1.10× | Fused | Win |
| Instruments (minified) | 2,793 | 1,630 | 1,198 | 1.71× | Fused | Win |
| Instruments (prettified) | 3,256 | 2,711 | 2,420 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK (prettified) | 2,660 | 2,284 | 1,754 | 1.16× | Fused | Win |
| Mesh (minified) | n/c | n/c | n/c | — | — | — |
| Mesh (prettified) | n/c | n/c | n/c | — | — | — |
| Random (minified) | 1,523 | 1,126 | 894 | 1.35× | Fused | Win |
| Random (prettified) | 1,931 | 1,829 | 1,510 | 1.06× | Fused | Win |
| Twitter (minified) | 2,425 | 1,697 | 1,602 | 1.43× | Fused | Win |
| Twitter (prettified) | 2,522 | 2,071 | 2,322 | 1.22× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: the compiler, and whether the document is indented. On the M1 the fused path wins all POD and minified input and most prettified input. On Linux it wins POD and minified input, and prettified input mostly goes to the tape. Under MSVC it loses most minified documents and every prettified one.

**POD-type tests: the fused path wins all 22, on every build.** It runs them 1.4× (String, Windows/MSVC) to 6.9× (Bool, macOS/GCC) faster than the two-stage path. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the compiler.

**Minified documents: the fused path's on Linux and the M1, mostly the tape's under MSVC.** Outside MSVC the fused path wins all 31 converged minified documents. Its margins run from 4% (Canada on Linux/GCC) to 79% (Instruments on macOS/GCC), and Instruments and Twitter, the documents with the most keys to match, are 32% to 51% faster on both Linux compilers where they converged. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order. Under MSVC the fused path wins Canada, Discord and Twitter, by 3% to 14%; the two-stage path wins the other seven, by 2% (Google Maps and Random) to 22% (Mesh).

**Prettified documents on x86: the two-stage path wins 25 of 30.** It wins all ten under MSVC, by 7% (Canada) to 47% (Mesh); nine of ten under Linux/Clang, by 3% (Canada, CitmCatalog and Marine IK Reverse) to 30% (Twitter); and six of ten under Linux/GCC, by 2% (Random) to 11% (Mesh). The fused path wins Marine IK on Linux/Clang (3%) and Marine IK and Marine IK Reverse on Linux/GCC (3% each), and Canada and CitmCatalog on Linux/GCC are ties. The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That keeps it close under GCC, but not under Clang or MSVC. Stage 1 classifies every byte, whitespace included, in 64-byte vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis.

**Prettified documents on the M1: close, leaning fused.** The fused path wins 8 of 14, by 4% (Marine IK Reverse on macOS/GCC) to 19% (CitmCatalog on macOS/GCC and Instruments on macOS/Clang), and Random on macOS/GCC is a tie. The two-stage path takes the other five by 2% to 6%: Google Maps and Mesh on both compilers, and Random on macOS/Clang. NEON largely escapes the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it.

**MSVC slows the fused path more than the tape.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. On minified Instruments the fused path reaches 36% of its Linux/Clang throughput, while the two-stage path reaches 54% and simdjson 39%; on minified CitmCatalog the figures are 51%, 66% and 43%, and on minified Mesh 38%, 54% and 36%. The two-stage path's stage 1 is branch-free intrinsic code whose shape is fixed by the source, while the fused path and simdjson's On Demand walk are both branch-heavy code whose performance rests on the optimizer. We take that difference in exposure to the compiler to be why MSVC moves the balance toward the tape, but we have not isolated it.

**Against simdjson, Jsonifier wins 102 of 107 tests with whichever path is faster.** It ties two, prettified Random on Linux/GCC and prettified Discord on macOS/Clang, and loses three: Mesh in both forms on Linux/GCC, and prettified Mesh on macOS/GCC. The fused path alone wins 96, ties 1 and loses 10. Neither MSVC, Linux/Clang nor macOS/Clang loses a test with the faster path.

**Reverse key order: MSVC is where it bites.** Requesting every key in reverse order forces simdjson's On Demand API into the rescanning behavior described in §3. Under MSVC simdjson manages 36 MB/s on minified Marine IK Reverse against 414 MB/s for the two-stage path, 11.5× slower, and 191 MB/s on the prettified version against 2,014 MB/s, 10.5× slower. On the other builds the gap is much smaller: the faster Jsonifier path is 1.1× to 1.4× faster than simdjson on every converged version.

The routing implication is sharper than §2's rule of thumb. On the M1, the fused path is the right default for POD and minified input and slightly ahead on prettified input. On Linux it is right for POD and minified input; prettified input should go to the two-stage path under Clang and mostly under GCC. Under MSVC it is right for POD-type data and a few minified documents, and prettified input should go to the two-stage path. Routing each test to its faster path would raise the record against simdjson from 96 wins, 1 tie and 10 losses to 102 wins, 2 ties and 3 losses; all seven tests that change hands are prettified documents on x86, five of them on Linux/Clang. The router does not yet take the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents, not the POD-type tests. The tables list every test where both converged.

#### Freshly allocated objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 859 | 628 | 1.37× | Win |
| Canada (prettified) | 2,385 | 1,878 | 1.27× | Win |
| CitmCatalog (minified) | 1,539 | 1,301 | 1.18× | Win |
| CitmCatalog (prettified) | 3,548 | 3,254 | 1.09× | Win |
| Discord (minified) | 1,577 | 1,545 | 1.02× | Win |
| Discord (prettified) | 2,466 | 2,462 | 1.00× | Tie |
| Google Maps Response (minified) | 1,502 | 1,288 | 1.17× | Win |
| Google Maps Response (prettified) | 3,590 | 3,058 | 1.17× | Win |
| Instruments (minified) | 1,996 | 1,799 | 1.11× | Win |
| Instruments (prettified) | 3,451 | 3,184 | 1.08× | Win |
| Marine IK Reverse (minified) | 682 | 601 | 1.13× | Win |
| Marine IK Reverse (prettified) | 3,006 | 2,792 | 1.08× | Win |
| Marine IK (minified) | 687 | 590 | 1.16× | Win |
| Marine IK (prettified) | 3,060 | 2,784 | 1.10× | Win |
| Mesh (minified) | 1,001 | 1,236 | 0.81× | **Loss** |
| Mesh (prettified) | 1,874 | 2,255 | 0.83× | **Loss** |
| Random (minified) | 1,114 | 1,130 | 0.99× | **Loss** |
| Random (prettified) | 1,901 | 1,999 | 0.95× | **Loss** |
| Twitter (minified) | 1,365 | 1,695 | 0.81× | **Loss** |
| Twitter (prettified) | 1,907 | 2,375 | 0.80× | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 601 | 428 | 1.40× | Win |
| CitmCatalog (minified) | 1,149 | 949 | 1.21× | Win |
| CitmCatalog (prettified) | 2,266 | 2,185 | 1.04× | Win |
| Discord (minified) | 1,082 | 974 | 1.11× | Win |
| Discord (prettified) | 1,721 | 1,386 | 1.24× | Win |
| Google Maps Response (minified) | 1,030 | 782 | 1.32× | Win |
| Google Maps Response (prettified) | 2,286 | 1,733 | 1.32× | Win |
| Instruments (minified) | 1,386 | 1,145 | 1.21× | Win |
| Instruments (prettified) | 2,534 | 1,931 | 1.31× | Win |
| Marine IK Reverse (minified) | 479 | 391 | 1.23× | Win |
| Marine IK Reverse (prettified) | 2,115 | 1,763 | 1.20× | Win |
| Marine IK (minified) | 506 | 215 | 2.35× | Win |
| Marine IK (prettified) | 2,180 | 1,037 | 2.10× | Win |
| Mesh (minified) | 694 | 836 | 0.83× | **Loss** |
| Mesh (prettified) | 1,269 | 1,449 | 0.88× | **Loss** |
| Random (minified) | 723 | 599 | 1.21× | Win |
| Random (prettified) | 1,275 | 1,127 | 1.13× | Win |

#### Reused objects

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,043 | 731 | 1.43× | Win |
| Canada (prettified) | 2,897 | 2,156 | 1.34× | Win |
| CitmCatalog (minified) | 1,758 | 1,585 | 1.11× | Win |
| CitmCatalog (prettified) | 3,985 | 3,854 | 1.03× | Win |
| Discord (minified) | 1,836 | 2,007 | 0.91× | **Loss** |
| Discord (prettified) | 2,757 | 3,059 | 0.90× | **Loss** |
| Google Maps Response (minified) | 1,626 | 1,374 | 1.18× | Win |
| Google Maps Response (prettified) | 3,684 | 3,222 | 1.14× | Win |
| Instruments (minified) | 2,084 | 1,942 | 1.07× | Win |
| Instruments (prettified) | 3,599 | 3,487 | 1.03× | Win |
| Marine IK Reverse (minified) | 734 | 655 | 1.12× | Win |
| Marine IK Reverse (prettified) | 3,265 | 3,036 | 1.08× | Win |
| Marine IK (minified) | 752 | 658 | 1.14× | Win |
| Marine IK (prettified) | 3,317 | 3,044 | 1.09× | Win |
| Mesh (minified) | 1,099 | 1,401 | 0.78× | **Loss** |
| Mesh (prettified) | 1,971 | 2,525 | 0.78× | **Loss** |
| Random (minified) | 1,306 | 1,501 | 0.87× | **Loss** |
| Random (prettified) | 2,140 | 2,629 | 0.81× | **Loss** |
| Twitter (minified) | 1,513 | 2,198 | 0.69× | **Loss** |
| Twitter (prettified) | 2,041 | 3,020 | 0.68× | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 748 | 468 | 1.60× | Win |
| CitmCatalog (minified) | 1,297 | 1,034 | 1.25× | Win |
| CitmCatalog (prettified) | 2,928 | 2,036 | 1.44× | Win |
| Discord (minified) | 1,250 | 1,110 | 1.13× | Win |
| Discord (prettified) | 1,841 | 1,532 | 1.20× | Win |
| Google Maps Response (minified) | 1,105 | 867 | 1.27× | Win |
| Google Maps Response (prettified) | 2,444 | 1,683 | 1.45× | Win |
| Instruments (minified) | 1,617 | 1,267 | 1.28× | Win |
| Instruments (prettified) | 2,482 | 2,087 | 1.19× | Win |
| Marine IK Reverse (minified) | 479 | 431 | 1.11× | Win |
| Marine IK (prettified) | 2,475 | 1,867 | 1.33× | Win |
| Mesh (minified) | 875 | 893 | 0.98× | **Loss** |
| Mesh (prettified) | 1,501 | 1,558 | 0.96× | **Loss** |
| Random (minified) | 844 | 777 | 1.09× | Win |
| Random (prettified) | 1,449 | 1,361 | 1.06× | Win |
| Twitter (minified) | 1,261 | 1,275 | 0.99× | Tie |

In the freshly allocated run, two-stage Jsonifier wins 28 of these 37 tests, ties 1 and loses 8. On the M1 it wins 15 of 17, by 4% (CitmCatalog, prettified) to 2.35× (Marine IK, minified), and loses only Mesh in both forms, by 17% (minified) and 12% (prettified). On Linux/GCC it wins 13 of 20, by 2% (Discord, minified) to 37% (Canada, minified), ties Discord (prettified), and loses Mesh, Random and Twitter in both forms, by 1% (Random, minified) to 20% (Twitter, prettified). Mesh and Twitter are also where Jsonifier struggles against simdjson On Demand on that build, so those losses look like properties of the documents under GCC on x86 rather than of the tape; we have not profiled them. With the fused path instead of the two-stage one, Jsonifier beats the reflection reader on 14 of 20 tests on Linux/GCC, with one tie, and 16 of 17 on macOS/GCC.

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

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSizeRoot` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `indentSize * currentDepth()`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

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
static constexpr uint64_t simdTapeStep	   = 1;
static constexpr uint64_t simdBlocksPerStep = 8;
	#else
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 8;
	#endif
#endif
```

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA — Clang preferring narrower steps with wider tape strides, GCC the reverse — reflects real differences in how each compiler schedules the unrolled block bodies, and is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

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
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep` — the Clang-vs-GCC AVX2 split in §4.1 (stride 4 vs stride 1) exists precisely because the two constants interact.
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
		uint64_t depth{};
		while (iter < endIter) {
			const char c = static_cast<char>(stringRootIter[*iter]);
			++iter;
			if (c == '{' || c == '[') {
				++depth;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					return true;
				}
			}
		}
		return reject<parse_statuses::unexpected_string_end>();
	}
	++iter;
	return true;
}
```

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per brace/bracket — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer iterator, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on minified documents outside MSVC and on most documents on the M1; slower on prettified x86 input and on half of minified documents under MSVC (see §2.2) |
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

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head, on both freshly allocated and reused target objects. Across 107 freshly allocated tests on five platforms the fused path wins 67, ties 3 and loses 37, and where it loses is a matter of platform more than of principle. It wins all 22 POD-type tests, by up to 6.9× (Bool on macOS/GCC), all 31 minified documents outside MSVC, and 8 of 14 prettified documents on the M1. The two-stage path wins 25 of 30 prettified documents on x86, and under MSVC it also takes seven of the ten minified documents. The tape, in other words, is not only an index for partial reads: on some compilers and some layouts it is also the faster way to read the whole book, and the router should learn which. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 102 of those 107 tests and ties two, and its own stage-1 + reflection path beats simdjson's stage-1 + reflection reader on 28 of 37. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*