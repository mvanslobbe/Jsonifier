# Jsonifier Serialization: Design and Performance

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Jsonifier serializes a reflection-registered C++ object in two passes: an exact-or-over size estimate, one buffer resize, then a single branch-light write pass that never checks capacity. Every byte the schema already knows (key names, quotes, colons, commas, indentation) is folded at compile time into packed integer constants and emitted with fixed-width stores, so the runtime work left is the data itself: strings, numbers and container lengths.

This paper describes that design as it stands in `include/jsonifier-incl/serializing/` (`serializer.hpp`, `serialize_impl.hpp`) and its helpers for strings (`string_utils.hpp`), integers (`i_to_str.hpp`) and floats (`d_to_str.hpp`).

## Architecture

The whole pipeline is resolved at compile time from `jsonifier::core<T>` and `serialize_options`: one sizing walk, one resize, one write walk.

```mermaid
flowchart LR
    A["serializeJson<br/>entry point"] --> B["Size pass<br/>staticSize shortcuts"]
    B --> C["One resize<br/>bound + 64 bytes slack"]
    C --> D["Write pass<br/>fixed-width overstores"]
    D -- "POD values inlined, containers outlined" --> E["Member headers<br/>one packed store each"]
    D --> F["Strings<br/>SIMD, SWAR, scalar"]
    D --> G["Integers<br/>4 digits per store"]
    D --> H["Floats<br/>zmij shortest round-trip"]
```

The `serialize<options>` dispatcher forces inlining (`JSONIFIER_INLINE`) only for POD values and optionals of them (`inline_contained_v`). Objects, maps and vectors go through a plain `inline` function, so the compiler can keep large container bodies out of line instead of expanding every nested type into its caller. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`).

## Key techniques

### Size first, then write without bounds checks

`get_size_impl` computes an upper bound for the output before a byte is written. Fixed-width types report a `staticSize` (numbers and enums 32, `bool` 5, `char` 8, null 4), so a `vector<double>` is sized as `n × 32` plus separators with no loop. Strings are sized at `6n + 2`, the worst case where every byte becomes a `\u00XX` escape. Object keys contribute `key.size() + 3` (or `+ 4` when prettified) as a compile-time constant.

`serializeJson` adds 64 bytes of slack and calls `resize_and_overwrite` when the buffer has it, so no zero-fill happens. The write pass then trusts the bound completely: no growth checks, no reallocation, and every writer may overstore (write 8 bytes, advance 4) into the slack.

### Writing through `resize_and_overwrite`

When the output buffer has C++23's `resize_and_overwrite` (detected by the `has_resize_and_overwrite` concept), the whole write pass runs inside it. `std::string::resize_and_overwrite(n, op)` grows the buffer to at least `n` without initializing it, calls `op(data, n)`, and sets the final size to whatever `op` returns. One call replaces the usual three steps: zero-filling resize, write, shrinking resize.

The `op` Jsonifier passes is `serialize_writer_ro`, a small functor that holds a reference to the object:

```cpp
template<serialize_options options, typename value_type> struct serialize_writer_ro {
	JSONIFIER_INLINE uint64_t operator()(write_buffer_ptr ptrNew, uint64_t) noexcept {
		const write_buffer_ptr bufferPtr = serialize<options>::implInline(object, ptrNew, 0);
		return static_cast<uint64_t>(bufferPtr - ptrNew);
	}
	value_type& object;
};
```

- **`operator()` is force-inlined.** `JSONIFIER_INLINE` makes the compiler expand the serializer into the standard library's call site, instead of leaving an out-of-line function that the library calls. The write pass then runs as part of `resize_and_overwrite`'s own body, with the buffer pointer in a register and no extra call frame. Whether `resize_and_overwrite` itself inlines into `serializeJson` is still the compiler's choice.
- **The functor is a reference and a function.** It holds only `value_type& object`, and its copy and move operations are deleted, so passing it costs nothing and the object is never copied.
- **The length comes straight from the pointer.** `operator()` returns the end pointer minus the start, so the final size is set from the write itself, with no second pass and no separate size variable.

Buffers without `resize_and_overwrite` take a fallback: `resize` to the bound if the buffer is smaller, write, then `resize` down to the written length. That path zero-fills on growth.

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD step, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpr Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht). Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

1. **One base per member.** `get_serialize_base` expands an index sequence over the `core<T>::parseValue` tuple and wraps each entry in `json_entity_serialize<options, entity>`. Each base carries its member's `name`, `memberPtr` and `isItLast` as compile-time constants.
2. **Aggregate the bases.** `serialize_map<bases...>` inherits from every one of them, so a type with 12 members gets one type with 12 bases.
3. **Fold over them.** `iterateValues` is a single fold, `((bufferPtr = bases::processIndex(value, bufferPtr, indent)), ...)`. Each `processIndex` is a static function of its own base, so every call is direct and resolved at compile time.

Inside each `processIndex`, nothing about the member is decided at runtime:

- the header `"name":` is that base's own `packed_blitter` constant;
- the value type is known, so `serialize<options>::impl` picks its writer with no dispatch;
- the trailing comma comes from `if constexpr (!isItLast)`, not an index check.

The sizing pass uses the same pattern: `size_getter_map` aggregates `json_entity_size` bases and folds `processIndex` over them, so both passes walk members the same way.

The result is no virtual calls, no `std::tuple` `get<I>` recursion, no runtime member index and no loop over members. A whole object becomes straight-line code: a run of fixed stores for headers and commas, with the value writers in between. The CAFBERIHT paper measures this same fold-over-bases dispatch as zero overhead against a hand-written baseline by assembly diff.

That is the distinction from CAFBERIHT. There, the bases are also filtered at compile time by tag, so only the components that match join the aggregate. Jsonifier's serializer keeps every registered member, so it has no filter step: the aggregate is exactly the members in `core<T>`. Runtime key exclusion (`jsonifierExcludedKeys`) is a separate mechanism, a set lookup inside each `processIndex` (see Limitations).

### Copies have a compile-time power-of-2 size

Nearly every copy on the write path has a power-of-2 size fixed at compile time: 2, 4 or 8 bytes. `pow2MemcpyWrapper<size>` takes the size as a template argument and `static_assert`s `std::has_single_bit(size)`, so a copy of any other size will not compile. A `memcpy` of 2, 4 or 8 bytes known at compile time lowers to a single unaligned load and store, with no call, no length branch and no tail loop.

What this does to the generated code:

| Copy | Typical x86-64 | Typical ARM64 |
| --- | --- | --- |
| Runtime length | `call memcpy` (or an inline length dispatch with branches and a tail loop) | `bl memcpy` |
| Compile-time 7 bytes | two overlapping 4-byte moves, or 4 + 2 + 1 | several loads and stores |
| Compile-time 2 / 4 / 8 bytes | one `mov` load, one `mov` store | one `ldrh`/`ldr` and one `strh`/`str` |
| Compile-time 2 / 4 / 8 bytes of constant data | one immediate store, no load (`mov qword ptr [rdi], imm`) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers for the whole member loop.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

That is why the writers round up and overstore instead of copying exact lengths. A 5-byte `false` is one 8-byte store. A 3-digit group is a 4-byte load followed by a 2-byte and a 1-byte store. A 7-byte key literal is one 8-byte store. The pointer then advances by the true length, and the size pass's slack absorbs the extra bytes.

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through the plain wrapper. That size is still a compile-time constant. The only copies whose length is known only at runtime are off the common path: escape sequences inside strings (only when an escapable byte is found), raw JSON passthrough, and the memset for indents deeper than 8 levels.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test, which serializes individual bools one at a time in a loop, this writer runs at 951–2,671 MB/s depending on the build, 10–17× Glaze's 54–246 MB/s. On Linux / GCC simdjson's reflection writer runs 252 MB/s, so the lead there is 8.6×.

### Strings: SIMD, then SWAR, then scalar

`string_serializer` copies first and checks second. For strings of 16 bytes or more it walks the widest available SIMD width down to the narrowest, storing each block unconditionally and testing it for `"`, `\` and bytes below 0x20. A clean block advances the full width; a dirty one advances to the first escapable byte, writes its escape from a 256-entry table and resumes.

The tail of 8 to 15 bytes uses SWAR on 64-bit words (`flagMask`), with a final overlapping load of the last 8 bytes. Strings under 8 bytes use two overlapping 4-byte or 2-byte loads, so a short clean key or value costs two loads, one test and two stores.

### Integers: exact length first, then 4 digits per store

Integers go through `to_chars` in `i_to_str.hpp` in three steps.

1. **Width.** In `serialize_impl`, every integer narrower than 64 bits is widened to `uint64_t` or `int64_t`, so one writer per signedness covers all eight integer types.
2. **Sign.** A negative value writes `'-'` and then takes its absolute value without a branch: `(v ^ (v >> 63)) - (v >> 63)` on the unsigned type. Because the result is unsigned, `INT64_MIN` works too.
3. **Length.** A balanced tree of comparisons against powers of ten picks the exact digit count, 1 to 20, in at most 5 compares. Each count has its own fixed writer (`to_chars_internal<v_type, N>`), so the writer knows at compile time where every digit goes and never reverses or shifts its output.

The writers use three lookup tables built at compile time: 100 two-digit entries, 1,000 three-digit entries and 10,000 four-digit entries, each digit group pre-packed as characters. Writing four digits is one 4-byte copy from the 10,000-entry table.

Divisions are replaced by multiplies:

- **Divide by 10^4** inside an 8-digit block: multiply by 3518437209 and shift right 45.
- **Divide by 10^8** for longer values: take the high bits of a 128-bit product with 12379400392853802749 and shift right 90 in total. GCC and Clang use `__uint128_t`, MSVC uses `_umul128`, and other compilers fall back to a portable `mulhi`.

So a 20-digit `uint64_t` is two 10^8 splits, four 10^4 splits and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

On the Int64 and Uint64 tests, which serialize individual values one at a time in a loop, Jsonifier is ahead of Glaze on every build where they converged except Int64 on Linux / Clang, a tie at 0.99×: 1.06–1.16× on Linux, 1.45× on MSVC (Uint64 only; Int64 did not converge), 1.25–1.46× on M1 GCC and 7.2–20× on M1 Clang. simdjson's reflection writer is level with it or ahead on GCC: Jsonifier is at 0.98× on Int64 and 1.02× on Uint64 on Linux, and at 0.88× and 0.97× on M1. With the string reused, Jsonifier is ahead of the fastest other library by 1.2–4.9× on both tests on every build.

### Floats

Doubles go through `zmij::detail::write`, a shortest-round-trip formatter vendored in `zmij.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3) and `indentChar` (default space) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in *Two Stages, On Demand*.

## Performance results

Jsonifier was fastest in 100 of the 115 converged serialization tests on five builds, tied 3 and lost 12. With the output string reused across iterations it was fastest in 108 of 121, tied 1 and lost 12. Against Glaze alone it wins 113 of the 115 freshly allocated tests and 120 of the 121 reused ones. Eleven of the twelve losses in each run are to simdjson's reflection writer, which exists only on the two GCC builds; the twelfth, in both runs, is the String test on macOS / Clang against Glaze. On the three builds with no reflection writer it is fastest in 68 of 70 freshly allocated tests and 72 of 73 reused. Prettified documents are where the lead is widest: 1.4–8.2× over Glaze, the only other writer there, across the five builds with freshly allocated output.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 5, 2026: Jsonifier [b87a5d0](https://github.com/nihilai-collective/jsonifier/commit/b87a5d0), Glaze [52971fe](https://github.com/stephenberry/glaze/commit/52971fe), simdjson [1a37712](https://github.com/simdjson/simdjson/commit/1a37712), BenchmarkSuite [8d787b1](https://github.com/nihilai-collective/benchmarksuite/commit/8d787b1). Every test runs twice: once with a freshly allocated output string per iteration, and once with a reused one (see Method). Sampling and tie rules are listed under Method below and match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection, and only for minified output, because its writer has no single-pass pretty mode. The Minify and Prettify tests reformat existing text instead of serializing objects, so they are left out of the counts.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 6.18.40.1 (WSL2) | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.1 | Intel Core i9-14900KF | Linux 6.18.40.1 (WSL2) | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.44 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The sweep's "Small" cut-down documents (at most 5 KiB minified) are not counted here. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the i9 builds, or under 10% and 5% on the virtualized M1. A test is ranked only if every library in it converged, and tests that do not converge are left out of every ranking, which is why builds report different test counts.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at the top, and as a loss when another library ranks above it. Win, tie and loss counts use only converged results.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The i9 numbers were taken under WSL2 and the M1 numbers in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 24 | 24 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 24 | 1 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 24 | 17 | 0 | 7 |
| macOS / GCC 16.2 (Apple M1, NEON) | 21 | 15 | 2 | 4 |
| macOS / Clang 23.1 (Apple M1, NEON) | 21 | 20 | 0 | 1 |
| **Total** | **115** | **100** | **3** | **12** |

Every converged write test per build follows, in MB/s, freshly allocated run first and reused run after it. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 951 | 54 | 17.47× |
| Double | 129 | 86 | 1.49× |
| String | 1,203 | 864 | 1.39× |
| Uint64 | 533 | 368 | 1.45× |
| Canada (minified) | 1,203 | 796 | 1.51× |
| Canada (prettified) | 2,686 | 1,406 | 1.91× |
| CitmCatalog (minified) | 8,861 | 4,471 | 1.98× |
| CitmCatalog (prettified) | 4,489 | 1,944 | 2.31× |
| Discord (minified) | 7,767 | 4,396 | 1.77× |
| Discord (prettified) | 11,178 | 4,345 | 2.57× |
| Google Maps Response (minified) | 7,816 | 3,405 | 2.30× |
| Google Maps Response (prettified) | 13,617 | 5,114 | 2.66× |
| Instruments (minified) | 11,271 | 4,012 | 2.81× |
| Instruments (prettified) | 16,608 | 4,597 | 3.61× |
| Marine IK Reverse (minified) | 793 | 664 | 1.19× |
| Marine IK Reverse (prettified) | 2,640 | 1,776 | 1.49× |
| Marine IK (minified) | 801 | 592 | 1.35× |
| Marine IK (prettified) | 2,641 | 1,214 | 2.17× |
| Mesh (minified) | 1,255 | 906 | 1.39× |
| Mesh (prettified) | 2,045 | 1,485 | 1.38× |
| Random (minified) | 2,967 | 2,904 | 1.02× |
| Random (prettified) | 3,721 | 2,224 | 1.67× |
| Twitter (minified) | 9,458 | 4,734 | 2.00× |
| Twitter (prettified) | 11,989 | 3,485 | 3.44× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,671 | 246 | 10.84× |
| Double | 345 | 326 | 1.06× |
| Int64 | 978 | 992 | 0.99× |
| String | 4,095 | 2,244 | 1.83× |
| Uint64 | 1,040 | 981 | 1.06× |
| Canada (minified) | 1,613 | 1,027 | 1.57× |
| Canada (prettified) | 5,689 | 3,457 | 1.65× |
| CitmCatalog (minified) | 9,042 | 6,091 | 1.48× |
| CitmCatalog (prettified) | 17,696 | 6,220 | 2.85× |
| Discord (minified) | 10,874 | 6,630 | 1.64× |
| Discord (prettified) | 16,955 | 5,347 | 3.17× |
| Google Maps Response (minified) | 9,138 | 4,420 | 2.07× |
| Google Maps Response (prettified) | 17,941 | 5,750 | 3.12× |
| Instruments (minified) | 17,781 | 6,378 | 2.79× |
| Instruments (prettified) | 23,014 | 6,055 | 3.80× |
| Marine IK Reverse (minified) | 892 | 686 | 1.30× |
| Marine IK Reverse (prettified) | 6,338 | 3,939 | 1.61× |
| Marine IK (minified) | 890 | 636 | 1.40× |
| Marine IK (prettified) | 6,042 | 3,161 | 1.91× |
| Mesh (minified) | 1,630 | 1,099 | 1.48× |
| Mesh (prettified) | 3,042 | 1,828 | 1.66× |
| Random (minified) | 7,230 | 3,608 | 2.00× |
| Random (prettified) | 11,756 | 4,204 | 2.80× |
| Twitter (minified) | 13,455 | 6,536 | 2.06× |
| Twitter (prettified) | 19,286 | 5,438 | 3.55× |

### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,172 | 211 | 252 | 8.63× |
| Double | 337 | 297 | 334 | 1.01× |
| Int64 | 930 | 800 | 951 | 0.98× |
| String | 3,525 | 2,089 | 3,776 | 0.93× |
| Uint64 | 954 | 821 | 935 | 1.02× |
| Canada (minified) | 1,775 | 1,357 | 950 | 1.31× |
| Canada (prettified) | 5,439 | 3,064 | — | 1.78× |
| CitmCatalog (prettified) | 16,206 | 6,291 | — | 2.58× |
| Discord (minified) | 9,506 | 6,221 | 10,916 | 0.87× |
| Discord (prettified) | 13,406 | 6,087 | — | 2.20× |
| Google Maps Response (minified) | 7,563 | 3,889 | 9,104 | 0.83× |
| Google Maps Response (prettified) | 15,462 | 6,265 | — | 2.47× |
| Instruments (minified) | 13,982 | 5,079 | 15,136 | 0.92× |
| Instruments (prettified) | 19,272 | 4,927 | — | 3.91× |
| Marine IK Reverse (minified) | 1,194 | 920 | 829 | 1.30× |
| Marine IK Reverse (prettified) | 5,712 | 3,307 | — | 1.73× |
| Marine IK (minified) | 1,128 | 915 | 860 | 1.23× |
| Marine IK (prettified) | 6,490 | 3,017 | — | 2.15× |
| Mesh (minified) | 2,364 | 1,689 | 1,376 | 1.40× |
| Mesh (prettified) | 4,606 | 2,606 | — | 1.77× |
| Random (minified) | 6,750 | 3,768 | 11,202 | 0.60× |
| Random (prettified) | 12,733 | 4,738 | — | 2.69× |
| Twitter (minified) | 11,488 | 7,462 | 12,630 | 0.91× |
| Twitter (prettified) | 15,586 | 4,875 | — | 3.20× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,790 | 107 | 157 | 11.38× |
| Double | 226 | 174 | 197 | 1.15× |
| Int64 | 547 | 436 | 618 | 0.88× |
| String | 960 | 914 | 955 | 1.01× |
| Uint64 | 688 | 471 | 707 | 0.97× |
| Canada (minified) | 1,719 | 1,152 | 635 | 1.49× |
| Canada (prettified) | 5,954 | 2,708 | — | 2.20× |
| Discord (minified) | 7,321 | 2,774 | 8,273 | 0.88× |
| Discord (prettified) | 7,786 | 2,357 | — | 3.30× |
| Google Maps Response (minified) | 6,651 | 1,446 | 6,726 | 0.99× |
| Google Maps Response (prettified) | 11,478 | 2,678 | — | 4.29× |
| Instruments (minified) | 11,035 | 1,757 | 7,335 | 1.50× |
| Instruments (prettified) | 10,978 | 1,885 | — | 5.83× |
| Marine IK Reverse (prettified) | 3,640 | 2,578 | — | 1.41× |
| Marine IK (prettified) | 4,200 | 2,214 | — | 1.90× |
| Mesh (minified) | 2,116 | 774 | 773 | 2.73× |
| Mesh (prettified) | 3,429 | 1,349 | — | 2.54× |
| Random (minified) | 5,498 | 1,758 | 7,626 | 0.72× |
| Random (prettified) | 6,811 | 2,243 | — | 3.04× |
| Twitter (minified) | 9,342 | 3,075 | 8,966 | 1.04× |
| Twitter (prettified) | 9,482 | 3,431 | — | 2.76× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,316 | 104 | 12.62× |
| Double | 274 | 164 | 1.67× |
| Int64 | 3,123 | 434 | 7.20× |
| String | 764 | 975 | 0.78× |
| Uint64 | 3,047 | 150 | 20.33× |
| Canada (minified) | 2,212 | 1,521 | 1.45× |
| Canada (prettified) | 5,670 | 2,673 | 2.12× |
| Discord (minified) | 6,490 | 1,988 | 3.26× |
| Discord (prettified) | 10,170 | 2,339 | 4.35× |
| Google Maps Response (minified) | 5,617 | 1,497 | 3.75× |
| Google Maps Response (prettified) | 11,713 | 2,447 | 4.79× |
| Instruments (minified) | 10,409 | 1,610 | 6.46× |
| Instruments (prettified) | 17,766 | 2,164 | 8.21× |
| Marine IK (minified) | 1,404 | 802 | 1.75× |
| Marine IK (prettified) | 6,249 | 2,678 | 2.33× |
| Mesh (minified) | 2,657 | 1,112 | 2.39× |
| Mesh (prettified) | 4,830 | 1,778 | 2.72× |
| Random (minified) | 4,287 | 1,821 | 2.35× |
| Random (prettified) | 6,313 | 1,659 | 3.81× |
| Twitter (minified) | 8,936 | 3,134 | 2.85× |
| Twitter (prettified) | 10,823 | 3,432 | 3.15× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 108 of 121 converged write tests. It ties Bool on Linux / GCC and loses 12: eleven to simdjson's reflection writer on the two GCC builds and String on macOS / Clang to Glaze.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 24 | 24 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 24 | 16 | 1 | 7 |
| macOS / GCC 16.2 (Apple M1, NEON) | 24 | 20 | 0 | 4 |
| macOS / Clang 23.1 (Apple M1, NEON) | 24 | 23 | 0 | 1 |
| **Total** | **121** | **108** | **1** | **12** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,308 | 227 | 5.77× |
| Int64 | 2,752 | 881 | 3.13× |
| String | 8,295 | 2,404 | 3.45× |
| Uint64 | 5,322 | 1,096 | 4.86× |
| Canada (minified) | 1,505 | 1,072 | 1.40× |
| Canada (prettified) | 4,208 | 3,056 | 1.38× |
| CitmCatalog (minified) | 8,920 | 4,947 | 1.80× |
| CitmCatalog (prettified) | 14,749 | 7,297 | 2.02× |
| Discord (minified) | 8,392 | 4,966 | 1.69× |
| Discord (prettified) | 11,547 | 5,037 | 2.29× |
| Google Maps Response (minified) | 8,566 | 3,891 | 2.20× |
| Google Maps Response (prettified) | 14,804 | 6,151 | 2.41× |
| Instruments (minified) | 11,564 | 4,595 | 2.52× |
| Instruments (prettified) | 16,873 | 5,387 | 3.13× |
| Marine IK Reverse (minified) | 912 | 740 | 1.23× |
| Marine IK Reverse (prettified) | 4,366 | 3,276 | 1.33× |
| Marine IK (minified) | 922 | 754 | 1.22× |
| Marine IK (prettified) | 4,364 | 3,322 | 1.31× |
| Mesh (minified) | 1,605 | 1,271 | 1.26× |
| Mesh (prettified) | 2,970 | 2,175 | 1.37× |
| Random (minified) | 5,960 | 3,123 | 1.91× |
| Random (prettified) | 9,896 | 4,553 | 2.17× |
| Twitter (minified) | 9,524 | 5,619 | 1.69× |
| Twitter (prettified) | 11,969 | 3,984 | 3.00× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,792 | 857 | 5.59× |
| Double | 925 | 750 | 1.23× |
| Int64 | 5,450 | 2,628 | 2.07× |
| String | 10,328 | 6,059 | 1.70× |
| Uint64 | 4,390 | 2,776 | 1.58× |
| Canada (minified) | 1,607 | 1,081 | 1.49× |
| Canada (prettified) | 5,656 | 4,085 | 1.38× |
| CitmCatalog (minified) | 8,613 | 7,025 | 1.23× |
| CitmCatalog (prettified) | 18,271 | 8,249 | 2.21× |
| Discord (minified) | 11,715 | 7,111 | 1.65× |
| Discord (prettified) | 16,807 | 6,006 | 2.80× |
| Google Maps Response (minified) | 9,465 | 4,233 | 2.24× |
| Google Maps Response (prettified) | 18,676 | 6,162 | 3.03× |
| Instruments (minified) | 17,700 | 7,780 | 2.28× |
| Instruments (prettified) | 22,735 | 7,194 | 3.16× |
| Marine IK Reverse (minified) | 901 | 687 | 1.31× |
| Marine IK Reverse (prettified) | 6,328 | 4,342 | 1.46× |
| Marine IK (minified) | 892 | 687 | 1.30× |
| Marine IK (prettified) | 6,392 | 4,328 | 1.48× |
| Mesh (minified) | 1,609 | 1,149 | 1.40× |
| Mesh (prettified) | 3,050 | 1,930 | 1.58× |
| Random (minified) | 7,251 | 3,804 | 1.91× |
| Random (prettified) | 11,778 | 4,773 | 2.47× |
| Twitter (minified) | 13,877 | 7,989 | 1.74× |
| Twitter (prettified) | 19,139 | 6,181 | 3.10× |

#### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,123 | 745 | 3,101 | 1.01× |
| Double | 811 | 681 | 631 | 1.19× |
| Int64 | 5,049 | 2,349 | 4,170 | 1.21× |
| String | 8,060 | 5,851 | 9,344 | 0.86× |
| Uint64 | 5,644 | 2,327 | 4,164 | 1.36× |
| Canada (prettified) | 5,491 | 3,717 | — | 1.48× |
| CitmCatalog (minified) | 10,364 | 6,326 | 10,980 | 0.94× |
| CitmCatalog (prettified) | 16,512 | 8,850 | — | 1.87× |
| Discord (minified) | 9,656 | 6,656 | 10,735 | 0.90× |
| Discord (prettified) | 13,596 | 7,293 | — | 1.86× |
| Google Maps Response (minified) | 7,945 | 4,477 | 9,468 | 0.84× |
| Google Maps Response (prettified) | 15,960 | 6,926 | — | 2.30× |
| Instruments (minified) | 13,818 | 6,013 | 14,823 | 0.93× |
| Instruments (prettified) | 19,544 | 5,949 | — | 3.28× |
| Marine IK Reverse (minified) | 1,171 | 897 | 851 | 1.31× |
| Marine IK Reverse (prettified) | 5,763 | 3,909 | — | 1.47× |
| Marine IK (minified) | 1,191 | 958 | 870 | 1.24× |
| Marine IK (prettified) | 6,501 | 4,021 | — | 1.62× |
| Mesh (minified) | 2,463 | 1,784 | 1,355 | 1.38× |
| Mesh (prettified) | 4,614 | 2,862 | — | 1.61× |
| Random (minified) | 6,764 | 4,144 | 11,619 | 0.58× |
| Random (prettified) | 12,617 | 5,225 | — | 2.41× |
| Twitter (minified) | 10,761 | 8,779 | 13,948 | 0.77× |
| Twitter (prettified) | 15,998 | 5,442 | — | 2.94× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Double | 773 | 403 | 619 | 1.25× |
| Int64 | 4,624 | 1,924 | 2,750 | 1.68× |
| String | 5,424 | 4,446 | 8,415 | 0.64× |
| Uint64 | 4,684 | 2,144 | 2,656 | 1.76× |
| Canada (minified) | 1,978 | 951 | 612 | 2.08× |
| Canada (prettified) | 6,353 | 3,500 | — | 1.82× |
| CitmCatalog (minified) | 7,714 | 1,895 | 5,961 | 1.29× |
| CitmCatalog (prettified) | 16,395 | 3,724 | — | 4.40× |
| Discord (minified) | 7,583 | 2,990 | 8,558 | 0.89× |
| Discord (prettified) | 8,071 | 2,651 | — | 3.04× |
| Google Maps Response (minified) | 7,225 | 1,668 | 7,769 | 0.93× |
| Google Maps Response (prettified) | 12,435 | 2,967 | — | 4.19× |
| Instruments (minified) | 11,220 | 1,878 | 7,441 | 1.51× |
| Instruments (prettified) | 12,159 | 2,217 | — | 5.48× |
| Marine IK Reverse (minified) | 1,064 | 645 | 621 | 1.65× |
| Marine IK Reverse (prettified) | 3,966 | 2,662 | — | 1.49× |
| Marine IK (minified) | 1,055 | 694 | 645 | 1.52× |
| Marine IK (prettified) | 4,053 | 2,672 | — | 1.52× |
| Mesh (minified) | 2,156 | 950 | 854 | 2.27× |
| Mesh (prettified) | 3,394 | 1,375 | — | 2.47× |
| Random (minified) | 5,436 | 1,898 | 7,365 | 0.74× |
| Random (prettified) | 6,553 | 2,432 | — | 2.69× |
| Twitter (minified) | 9,424 | 3,838 | 8,990 | 1.05× |
| Twitter (prettified) | 9,887 | 3,968 | — | 2.49× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Double | 634 | 592 | 1.07× |
| Int64 | 3,138 | 2,096 | 1.50× |
| String | 2,765 | 4,216 | 0.66× |
| Uint64 | 3,211 | 1,765 | 1.82× |
| Canada (minified) | 2,072 | 1,553 | 1.33× |
| Canada (prettified) | 5,028 | 3,165 | 1.59× |
| CitmCatalog (minified) | 3,650 | 1,561 | 2.34× |
| CitmCatalog (prettified) | 10,848 | 2,566 | 4.23× |
| Discord (minified) | 6,595 | 2,413 | 2.73× |
| Discord (prettified) | 10,494 | 2,740 | 3.83× |
| Google Maps Response (minified) | 5,839 | 1,626 | 3.59× |
| Google Maps Response (prettified) | 12,786 | 2,595 | 4.93× |
| Instruments (minified) | 10,938 | 1,831 | 5.97× |
| Instruments (prettified) | 19,048 | 2,115 | 9.01× |
| Marine IK Reverse (minified) | 945 | 816 | 1.16× |
| Marine IK Reverse (prettified) | 6,054 | 3,044 | 1.99× |
| Marine IK (minified) | 1,403 | 821 | 1.71× |
| Marine IK (prettified) | 6,109 | 3,071 | 1.99× |
| Mesh (minified) | 2,657 | 1,148 | 2.31× |
| Mesh (prettified) | 4,829 | 1,833 | 2.63× |
| Random (minified) | 4,321 | 1,942 | 2.22× |
| Random (prettified) | 6,299 | 2,169 | 2.90× |
| Twitter (minified) | 5,936 | 4,089 | 1.45× |
| Twitter (prettified) | 9,924 | 3,563 | 2.79× |

### Losses

The prettified lead comes from the indent tables: a newline plus indent is a few 8-byte stores, not a loop. Windows / MSVC has no loss or tie in either run. The losses sit on the two GCC builds and on macOS / Clang, and eleven of the twelve in each run are to simdjson's reflection writer:

- **simdjson reflection writer, Linux / GCC and macOS / GCC.** Freshly allocated, Jsonifier is behind it on seven results on Linux / GCC (Int64 at 0.98×, String at 0.93×, and minified Discord at 0.87×, Google Maps at 0.83×, Instruments at 0.92×, Random at 0.60× and Twitter at 0.91×) and on four on macOS / GCC (Int64 at 0.88×, Uint64 at 0.97×, and minified Discord at 0.88× and Random at 0.72×). Reused, the Linux / GCC losses are String at 0.86× and minified CitmCatalog at 0.94×, Discord at 0.90×, Google Maps at 0.84×, Instruments at 0.93×, Random at 0.58× and Twitter at 0.77×, and the macOS / GCC losses are String at 0.64× and minified Discord at 0.89×, Google Maps at 0.93× and Random at 0.74×. On the same two builds Jsonifier is ahead of the reflection writer on Double in both runs and on the minified Canada, Mesh and Marine IK documents (freshly allocated on Linux / GCC: 1.31×, 1.40× and 1.23–1.30×). The documents it loses are mostly built from strings and integers with few or no floating-point values (Random, Discord, Google Maps, Instruments), and the ones it wins are mostly arrays of doubles. Instruments and Twitter go the other way on macOS / GCC, though (1.50× and 1.04× freshly allocated), so this is a tendency and not a rule. Not profiled yet.
- **String test, macOS / Clang.** Glaze runs 975 vs 764 MB/s (Jsonifier at 0.78×) freshly allocated and 4,216 vs 2,765 MB/s (0.66×) reused. On AVX2 Jsonifier wins the same test over Glaze by 1.4–1.8× freshly allocated, so the gap sits in the NEON escape path.

The three freshly allocated ties are Int64 on Linux / Clang (0.99× against Glaze), and String and minified Google Maps on macOS / GCC (1.01× and 0.99×, both against the reflection writer).

## Questions from external review

An outside review of a draft raised five concerns. Each is answered here against the code as it stands; where the answer is "not measured yet", it says so.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds accesses.

The bound covers the largest overstore of each writer:

| Writer | Largest write beyond its true length | Covered by |
| --- | --- | --- |
| Bool | 3 bytes (8 written, 5 reserved) | 64-byte tail slack |
| Integer or float | none past the 32 bytes reserved per number | `staticSize` of 32 |
| Key literal over 16 bytes | up to 31 bytes (rounded to a multiple of 32) | 64-byte tail slack |
| Indent run | up to 7 bytes (rounded to 8) | 64-byte tail slack |
| String SIMD block | one vector width, stored before the escape check | the `6n + 2` reservation, which the copied bytes never exceed |

The largest single overstore is 31 bytes, under the 64-byte slack. That slack is a fixed constant, not derived from the widest writer, so a future writer with a wider overstore must raise it.

### Does `6n + 2` blow up memory on huge strings?

Yes, the reservation is real. A 100 MB string field asks for about 600 MB before the final shrink, and the string keeps that capacity afterwards unless the caller calls `shrink_to_fit`. How much of it is touched depends on the buffer:

- With `resize_and_overwrite`, nothing is zero-filled. On Linux and macOS, pages that are never written are usually never faulted in, so the cost is mostly address space. On Windows, the allocation still counts against the commit limit.
- Without `resize_and_overwrite`, `resize` zero-fills the whole bound, so every page is touched.

A two-tier bound, counting escapable bytes in strings above a size threshold before reserving, would cut this to roughly `n` plus the escapes found. It is not implemented, and its cost on typical small strings has not been measured.

### Is it portable to big-endian targets?

The code has big-endian paths, but none are tested. `packed_blitter`, the bool constants, the digit tables and the SWAR string tail each branch on `std::endian::native` at compile time and use `std::byteswap` where needed. The CI matrix covers only little-endian x86 and ARM, so those branches have never run.

### Why does Glaze win the string test on NEON?

It wins on one of the two M1 builds. On macOS / Clang Glaze runs 975 vs 764 MB/s (Jsonifier at 0.78×) freshly allocated and 4,216 vs 2,765 MB/s (0.66×) reused. On macOS / GCC Jsonifier is ahead of Glaze (960 vs 914 MB/s freshly allocated, 5,424 vs 4,446 MB/s reused), but simdjson's reflection writer is level with it freshly allocated (955 MB/s, a tie) and ahead of it reused (8,415 MB/s, Jsonifier at 0.64×). On AVX2 Jsonifier wins the String test over Glaze by 1.4–1.8× freshly allocated, and the reflection writer is ahead of it on Linux / GCC (0.93× freshly allocated, 0.86× reused).

Not profiled yet. Two features of the current loop are candidates:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

The same code beats Glaze on every AVX2 build and on macOS / GCC, and loses to it only on macOS / Clang, so the Glaze gap is specific to that NEON build. Whether either candidate is the cause is open.

### Does simdjson's writer win doubles on Linux GCC?

No. On the Double test Jsonifier is level with it freshly allocated (337 vs 334 MB/s, 1.01×, a narrow win that the t-test still separates) and ahead of it reused (811 vs 631 MB/s, 1.19×). On macOS / GCC it is ahead by 1.15× freshly allocated and 1.25× reused. It also beats Glaze on the Double test on all five builds, by 1.06–1.67× freshly allocated. What the reflection writer does win on GCC is String, some of the Int64 and Uint64 results, and the minified documents listed under Losses. Why its writer is faster on those has not been measured, and this paper does not guess at its algorithm.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** The size bound is worst case: every number reserves 32 bytes and every string `6n + 2`. A string-heavy document asks for up to about 6× its output size before shrinking. That is cheap with `resize_and_overwrite` but costs peak memory on large payloads.
- **Two walks.** Sizing visits every dynamic container and string length before writing. Types with a `staticSize` skip this, so the cost falls on maps, nested vectors and strings.
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Indent table depth.** Indentation past 8 levels takes the `memset` fallback.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
