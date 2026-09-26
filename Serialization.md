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

On the Bool test, which serializes individual bools one at a time in a loop, this writer runs at 629–2,598 MB/s depending on the build, 6–13× Glaze's 89–216 MB/s.

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

On the Uint64 and Int64 tests, which serialize individual values one at a time in a loop, jsonifier wins on every build where they converged: 1.02–1.14× Glaze on x86 GCC and Clang, 1.44× on MSVC, 1.51–1.77× on M1 GCC, and 6.7–7.2× on M1 Clang.

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

Jsonifier was fastest in 117 of the 121 converged serialization tests on five builds and lost 4, with no ties. With the output string reused across iterations it was fastest in 120 of 122, tied 1 and lost 1. On Linux / GCC, the closest build, its full-document lead over the runner-up is 1.0–1.7× minified and 1.6–3.4× prettified.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 3, 2026: Jsonifier [4724a1a](https://github.com/nihilai-collective/jsonifier/commit/4724a1a), Glaze [52971fe](https://github.com/stephenberry/glaze/commit/52971fe), simdjson [2a690bc](https://github.com/simdjson/simdjson/commit/2a690bc), BenchmarkSuite [4e7c701](https://github.com/nihilai-collective/benchmarksuite/commit/4e7c701). Every test runs twice: once with a freshly allocated output string per iteration, and once with a reused one (see Method). Sampling and tie rules are listed under Method below and match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection, and only in minified tests, because its writer has no single-pass pretty mode. The Minify and Prettify tests reformat existing text instead of serializing objects, so they are left out of the counts.

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

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes and Jsonifier 1,811. Each library is credited with its own output size.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the i9 builds, or under 10% and 5% on the virtualized M1. Results that do not converge are left out of every ranking, which is why builds report different test counts.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test cannot separate their kept epochs. Win, tie and loss counts use only converged results.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The i9 numbers were taken under WSL2 and the M1 numbers in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 23 | 23 | 0 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 24 | 23 | 0 | 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 25 | 24 | 0 | 1 |
| macOS / GCC 16.2 (Apple M1, NEON) | 24 | 23 | 0 | 1 |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 24 | 0 | 1 |
| **Total** | **121** | **117** | **0** | **4** |

Every converged write test per build follows, in MB/s, freshly allocated run first and reused run after it. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,175 | 89 | 13.20× |
| Double | 214 | 144 | 1.48× |
| Int64 | 532 | 370 | 1.44× |
| String | 1,179 | 873 | 1.35× |
| Uint64 | 535 | 373 | 1.44× |
| Canada (minified) | 1,196 | 815 | 1.47× |
| Canada (prettified) | 2,559 | 1,421 | 1.80× |
| CitmCatalog (minified) | 10,443 | 4,578 | 2.28× |
| CitmCatalog (prettified) | 4,366 | 1,942 | 2.25× |
| Discord (minified) | 8,397 | 4,549 | 1.85× |
| Discord (prettified) | 11,446 | 4,443 | 2.58× |
| Google Maps Response (minified) | 6,779 | 3,303 | 2.05× |
| Google Maps Response (prettified) | 13,173 | 5,392 | 2.44× |
| Instruments (minified) | 10,885 | 4,085 | 2.66× |
| Instruments (prettified) | 16,775 | 4,698 | 3.57× |
| Marine IK Reverse (minified) | 801 | 677 | 1.18× |
| Marine IK Reverse (prettified) | 2,684 | 1,785 | 1.50× |
| Marine IK (minified) | 813 | 593 | 1.37× |
| Marine IK (prettified) | 2,709 | 1,254 | 2.16× |
| Mesh (minified) | 1,281 | 914 | 1.40× |
| Mesh (prettified) | 2,042 | 1,479 | 1.38× |
| Random (minified) | 2,773 | 2,866 | 0.97× |
| Random (prettified) | 3,828 | 2,239 | 1.71× |
| Twitter (minified) | 9,261 | 4,313 | 2.15× |
| Twitter (prettified) | 12,425 | 3,659 | 3.40× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,598 | 216 | 12.03× |
| Double | 338 | 299 | 1.13× |
| Int64 | 953 | 854 | 1.12× |
| Uint64 | 927 | 911 | 1.02× |
| Canada (minified) | 1,490 | 951 | 1.57× |
| Canada (prettified) | 5,172 | 3,207 | 1.61× |
| CitmCatalog (minified) | 9,999 | 5,455 | 1.83× |
| CitmCatalog (prettified) | 19,576 | 5,685 | 3.44× |
| Discord (minified) | 11,128 | 5,946 | 1.87× |
| Discord (prettified) | 16,391 | 5,039 | 3.25× |
| Google Maps Response (minified) | 8,516 | 3,883 | 2.19× |
| Google Maps Response (prettified) | 16,664 | 5,040 | 3.31× |
| Instruments (minified) | 16,839 | 6,058 | 2.78× |
| Instruments (prettified) | 20,908 | 5,409 | 3.87× |
| Marine IK Reverse (minified) | 762 | 631 | 1.21× |
| Marine IK Reverse (prettified) | 5,871 | 3,668 | 1.60× |
| Marine IK (minified) | 784 | 603 | 1.30× |
| Marine IK (prettified) | 5,832 | 2,986 | 1.95× |
| Mesh (minified) | 1,428 | 1,017 | 1.40× |
| Mesh (prettified) | 2,787 | 1,750 | 1.59× |
| Random (minified) | 6,287 | 3,357 | 1.87× |
| Random (prettified) | 10,776 | 3,922 | 2.75× |
| Twitter (minified) | 12,529 | 5,743 | 2.18× |

### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,090 | 204 | 330 | 6.33× |
| Double | 316 | 290 | 402 | 0.79× |
| Int64 | 909 | 851 | 633 | 1.07× |
| String | 3,634 | 1,989 | 1,588 | 1.83× |
| Uint64 | 966 | 845 | 605 | 1.14× |
| Canada (minified) | 1,891 | 1,420 | 921 | 1.33× |
| Canada (prettified) | 5,395 | 3,085 | — | 1.75× |
| CitmCatalog (minified) | 11,161 | 5,690 | 6,458 | 1.73× |
| CitmCatalog (prettified) | 18,447 | 6,479 | — | 2.85× |
| Discord (minified) | 10,366 | 6,030 | 8,809 | 1.18× |
| Discord (prettified) | 14,474 | 6,079 | — | 2.38× |
| Google Maps Response (prettified) | 15,087 | 5,987 | — | 2.52× |
| Instruments (minified) | 14,136 | 5,567 | 8,683 | 1.63× |
| Instruments (prettified) | 19,033 | 5,685 | — | 3.35× |
| Marine IK Reverse (minified) | 1,207 | 957 | 696 | 1.26× |
| Marine IK Reverse (prettified) | 5,740 | 3,561 | — | 1.61× |
| Marine IK (minified) | 1,203 | 930 | 688 | 1.29× |
| Marine IK (prettified) | 5,998 | 2,913 | — | 2.06× |
| Mesh (minified) | 2,235 | 1,531 | 1,109 | 1.46× |
| Mesh (prettified) | 4,171 | 2,567 | — | 1.62× |
| Random (minified) | 6,155 | 3,567 | 5,933 | 1.04× |
| Random (prettified) | 11,517 | 4,446 | — | 2.59× |
| Twitter (minified) | 10,389 | 6,565 | 7,824 | 1.33× |
| Twitter (prettified) | 14,910 | 4,843 | — | 3.08× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Double | 244 | 187 | 110 | 1.30× |
| Int64 | 670 | 379 | 217 | 1.77× |
| String | 847 | 927 | 631 | 0.91× |
| Uint64 | 715 | 475 | 280 | 1.51× |
| Canada (minified) | 1,581 | 1,407 | 521 | 1.12× |
| Canada (prettified) | 5,837 | 2,854 | — | 2.04× |
| CitmCatalog (minified) | 7,944 | 1,754 | 4,237 | 1.87× |
| CitmCatalog (prettified) | 16,902 | 3,471 | — | 4.87× |
| Discord (minified) | 7,903 | 2,640 | 3,960 | 2.00× |
| Discord (prettified) | 8,893 | 2,312 | — | 3.85× |
| Google Maps Response (minified) | 7,186 | 1,529 | 4,345 | 1.65× |
| Google Maps Response (prettified) | 11,307 | 2,686 | — | 4.21× |
| Instruments (minified) | 11,215 | 1,700 | 4,627 | 2.42× |
| Instruments (prettified) | 12,130 | 2,065 | — | 5.88× |
| Marine IK Reverse (minified) | 1,146 | 750 | 611 | 1.53× |
| Marine IK Reverse (prettified) | 4,321 | 2,591 | — | 1.67× |
| Marine IK (minified) | 1,163 | 720 | 618 | 1.62× |
| Marine IK (prettified) | 4,306 | 2,368 | — | 1.82× |
| Mesh (minified) | 2,160 | 984 | 779 | 2.19× |
| Mesh (prettified) | 3,391 | 1,401 | — | 2.42× |
| Random (minified) | 5,488 | 1,762 | 4,989 | 1.10× |
| Random (prettified) | 7,039 | 2,361 | — | 2.98× |
| Twitter (minified) | 9,108 | 1,478 | 5,663 | 1.61× |
| Twitter (prettified) | 9,951 | 3,105 | — | 3.20× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 629 | 105 | 5.97× |
| Double | 261 | 183 | 1.42× |
| Int64 | 2,641 | 397 | 6.65× |
| String | 741 | 856 | 0.87× |
| Uint64 | 2,920 | 405 | 7.21× |
| Canada (minified) | 2,012 | 1,519 | 1.32× |
| Canada (prettified) | 4,746 | 2,317 | 2.05× |
| CitmCatalog (minified) | 5,646 | 1,601 | 3.53× |
| CitmCatalog (prettified) | 14,178 | 2,386 | 5.94× |
| Discord (minified) | 7,183 | 2,347 | 3.06× |
| Discord (prettified) | 11,679 | 2,746 | 4.25× |
| Google Maps Response (minified) | 6,083 | 1,616 | 3.76× |
| Google Maps Response (prettified) | 11,998 | 2,745 | 4.37× |
| Instruments (minified) | 11,366 | 1,851 | 6.14× |
| Instruments (prettified) | 20,051 | 2,166 | 9.26× |
| Marine IK Reverse (minified) | 1,171 | 720 | 1.63× |
| Marine IK Reverse (prettified) | 5,596 | 1,955 | 2.86× |
| Marine IK (minified) | 1,201 | 698 | 1.72× |
| Marine IK (prettified) | 5,742 | 2,343 | 2.45× |
| Mesh (minified) | 2,655 | 1,022 | 2.60× |
| Mesh (prettified) | 4,278 | 1,689 | 2.53× |
| Random (minified) | 3,944 | 1,646 | 2.40× |
| Random (prettified) | 6,094 | 2,122 | 2.87× |
| Twitter (minified) | 9,651 | 3,539 | 2.73× |
| Twitter (prettified) | 11,554 | 3,680 | 3.14× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 120 of 122 converged write tests. It ties Canada (minified) on macOS / Clang and loses String there.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| macOS / Clang 23.1 (Apple M1, NEON) | 24 | 22 | 1 | 1 |
| macOS / GCC 16.2 (Apple M1, NEON) | 23 | 23 | 0 | 0 |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| **Total** | **122** | **120** | **1** | **1** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,778 | 257 | 6.92× |
| Double | 753 | 306 | 2.46× |
| Int64 | 4,734 | 1,082 | 4.38× |
| String | 7,917 | 2,383 | 3.32× |
| Uint64 | 4,987 | 1,112 | 4.49× |
| Canada (minified) | 1,501 | 1,090 | 1.38× |
| Canada (prettified) | 4,244 | 3,058 | 1.39× |
| CitmCatalog (minified) | 10,551 | 5,056 | 2.09× |
| CitmCatalog (prettified) | 16,348 | 7,475 | 2.19× |
| Discord (minified) | 8,655 | 5,209 | 1.66× |
| Discord (prettified) | 12,137 | 5,003 | 2.43× |
| Google Maps Response (minified) | 7,264 | 4,075 | 1.78× |
| Google Maps Response (prettified) | 14,356 | 6,539 | 2.20× |
| Instruments (minified) | 11,246 | 4,499 | 2.50× |
| Instruments (prettified) | 17,285 | 5,354 | 3.23× |
| Marine IK Reverse (minified) | 912 | 732 | 1.25× |
| Marine IK Reverse (prettified) | 4,326 | 3,262 | 1.33× |
| Marine IK (minified) | 920 | 745 | 1.23× |
| Marine IK (prettified) | 4,513 | 3,337 | 1.35× |
| Mesh (minified) | 1,607 | 1,281 | 1.25× |
| Mesh (prettified) | 3,037 | 2,192 | 1.39× |
| Random (minified) | 5,504 | 3,145 | 1.75× |
| Random (prettified) | 9,612 | 4,630 | 2.08× |
| Twitter (minified) | 9,435 | 4,979 | 1.90× |
| Twitter (prettified) | 12,414 | 4,104 | 3.03× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,033 | 793 | 5.09× |
| Double | 850 | 543 | 1.56× |
| Int64 | 5,010 | 2,383 | 2.10× |
| String | 9,517 | 5,712 | 1.67× |
| Uint64 | 4,277 | 2,582 | 1.66× |
| Canada (minified) | 1,480 | 989 | 1.50× |
| Canada (prettified) | 5,129 | 3,793 | 1.35× |
| CitmCatalog (minified) | 9,495 | 6,458 | 1.47× |
| CitmCatalog (prettified) | 19,729 | 7,565 | 2.61× |
| Discord (minified) | 12,003 | 6,819 | 1.76× |
| Discord (prettified) | 16,469 | 5,860 | 2.81× |
| Google Maps Response (minified) | 8,847 | 4,073 | 2.17× |
| Google Maps Response (prettified) | 17,265 | 5,412 | 3.19× |
| Instruments (minified) | 16,910 | 7,303 | 2.32× |
| Instruments (prettified) | 20,924 | 6,382 | 3.28× |
| Marine IK Reverse (minified) | 752 | 594 | 1.27× |
| Marine IK Reverse (prettified) | 5,915 | 3,979 | 1.49× |
| Marine IK (minified) | 774 | 621 | 1.25× |
| Marine IK (prettified) | 5,902 | 4,026 | 1.47× |
| Mesh (minified) | 1,409 | 1,001 | 1.41× |
| Mesh (prettified) | 2,778 | 1,824 | 1.52× |
| Random (minified) | 6,807 | 3,591 | 1.90× |
| Random (prettified) | 10,861 | 4,190 | 2.59× |
| Twitter (minified) | 12,936 | 7,100 | 1.82× |
| Twitter (prettified) | 16,753 | 5,419 | 3.09× |

#### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,119 | 760 | 333 | 4.10× |
| Double | 820 | 629 | 449 | 1.30× |
| Int64 | 4,992 | 2,396 | 1,577 | 2.08× |
| String | 7,960 | 5,813 | 3,546 | 1.37× |
| Uint64 | 5,426 | 2,420 | 1,477 | 2.24× |
| Canada (minified) | 1,886 | 1,488 | 922 | 1.27× |
| Canada (prettified) | 5,514 | 3,654 | — | 1.51× |
| CitmCatalog (minified) | 11,124 | 6,190 | 6,357 | 1.75× |
| CitmCatalog (prettified) | 18,151 | 8,969 | — | 2.02× |
| Discord (minified) | 10,423 | 7,173 | 9,075 | 1.15× |
| Discord (prettified) | 14,972 | 7,361 | — | 2.03× |
| Google Maps Response (minified) | 7,786 | 4,366 | 6,286 | 1.24× |
| Google Maps Response (prettified) | 16,144 | 6,869 | — | 2.35× |
| Instruments (minified) | 14,093 | 6,597 | 8,874 | 1.59× |
| Instruments (prettified) | 19,270 | 6,894 | — | 2.80× |
| Marine IK Reverse (minified) | 1,209 | 962 | 569 | 1.26× |
| Marine IK Reverse (prettified) | 5,693 | 3,916 | — | 1.45× |
| Marine IK (minified) | 1,204 | 972 | 595 | 1.24× |
| Marine IK (prettified) | 5,914 | 3,974 | — | 1.49× |
| Mesh (minified) | 2,242 | 1,620 | 1,104 | 1.38× |
| Mesh (prettified) | 4,191 | 2,714 | — | 1.54× |
| Random (minified) | 6,174 | 3,963 | 5,897 | 1.05× |
| Random (prettified) | 11,659 | 5,146 | — | 2.27× |
| Twitter (minified) | 10,675 | 8,035 | 7,896 | 1.33× |
| Twitter (prettified) | 15,574 | 5,447 | — | 2.86× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,928 | 709 | 100 | 4.13× |
| Double | 664 | 386 | 122 | 1.72× |
| Int64 | 3,587 | 1,718 | 302 | 2.09× |
| String | 5,912 | 3,977 | 820 | 1.49× |
| Uint64 | 4,658 | 2,160 | 411 | 2.16× |
| Canada (prettified) | 4,995 | 2,791 | — | 1.79× |
| CitmCatalog (minified) | 7,964 | 1,934 | 4,248 | 1.87× |
| CitmCatalog (prettified) | 16,252 | 4,046 | — | 4.02× |
| Discord (minified) | 8,348 | 3,013 | 4,959 | 1.68× |
| Discord (prettified) | 9,011 | 2,622 | — | 3.44× |
| Google Maps Response (minified) | 7,635 | 1,774 | 4,335 | 1.76× |
| Google Maps Response (prettified) | 12,316 | 2,908 | — | 4.24× |
| Instruments (minified) | 11,027 | 1,849 | 4,092 | 2.69× |
| Instruments (prettified) | 12,344 | 2,273 | — | 5.43× |
| Marine IK Reverse (minified) | 1,165 | 753 | 609 | 1.55× |
| Marine IK Reverse (prettified) | 4,348 | 2,704 | — | 1.61× |
| Marine IK (minified) | 1,227 | 758 | 620 | 1.62× |
| Marine IK (prettified) | 4,440 | 2,725 | — | 1.63× |
| Mesh (minified) | 2,166 | 1,009 | 777 | 2.15× |
| Mesh (prettified) | 3,455 | 1,433 | — | 2.41× |
| Random (minified) | 5,653 | 1,894 | 4,730 | 1.20× |
| Random (prettified) | 7,109 | 2,480 | — | 2.87× |
| Twitter (prettified) | 9,974 | 3,839 | — | 2.60× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,553 | 640 | 3.99× |
| Double | 763 | 583 | 1.31× |
| Int64 | 3,225 | 1,848 | 1.74× |
| String | 2,383 | 3,978 | 0.60× |
| Uint64 | 3,367 | 643 | 5.23× |
| Canada (minified) | 1,432 | 1,508 | 0.95× |
| Canada (prettified) | 4,696 | 2,825 | 1.66× |
| CitmCatalog (minified) | 5,888 | 1,904 | 3.09× |
| CitmCatalog (prettified) | 15,755 | 2,793 | 5.64× |
| Discord (minified) | 6,935 | 2,458 | 2.82× |
| Discord (prettified) | 12,068 | 3,084 | 3.91× |
| Google Maps Response (minified) | 6,292 | 1,732 | 3.63× |
| Google Maps Response (prettified) | 14,154 | 2,903 | 4.87× |
| Instruments (minified) | 11,646 | 1,994 | 5.84× |
| Instruments (prettified) | 20,287 | 2,374 | 8.54× |
| Marine IK Reverse (minified) | 1,116 | 720 | 1.55× |
| Marine IK (minified) | 1,245 | 716 | 1.74× |
| Marine IK (prettified) | 5,168 | 2,357 | 2.19× |
| Mesh (minified) | 2,335 | 1,090 | 2.14× |
| Mesh (prettified) | 4,316 | 1,670 | 2.58× |
| Random (minified) | 3,860 | 1,802 | 2.14× |
| Random (prettified) | 6,501 | 2,389 | 2.72× |
| Twitter (minified) | 9,799 | 4,193 | 2.34× |
| Twitter (prettified) | 12,007 | 4,121 | 2.91× |

### Losses

The prettified lead comes from the indent tables: a newline plus indent is a few 8-byte stores, not a loop. Three of the four freshly allocated losses are on the POD tests, which serialize individual values one at a time in a loop, and the fourth is a full document on MSVC:

- **String test, M1 / GCC and M1 / Clang.** Glaze runs 927 vs 847 MB/s on GCC (Jsonifier at 0.91×) and 856 vs 741 MB/s on Clang (0.87×). On AVX2 Jsonifier wins the same test by 1.35–1.83×, so the gap sits in the NEON escape path.
- **Double test, Linux GCC.** simdjson's reflection writer runs 402 vs 316 MB/s (Jsonifier at 0.79×). Jsonifier still beats Glaze there and wins the same test on every other build where it converged.
- **Random (minified), Windows MSVC.** Glaze runs 2,866 vs 2,773 MB/s (Jsonifier at 0.97×). Jsonifier wins the prettified version of the same document on MSVC by 1.71×, and the minified version on every other build by 1.04–2.40×. Not profiled yet.

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

Not profiled yet. Two features of the current loop are candidates:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

The same code wins this test by 1.35–1.83× on AVX2 and loses it on both M1 compilers, so the gap is specific to the NEON build. Whether either candidate is the cause is open.

### Why does simdjson's writer win doubles on Linux GCC?

Not profiled yet. The loss is 0.79×, on one build only; Jsonifier wins the same test on the other four builds and beats Glaze on all five. Why simdjson's float formatter is faster on GCC has not been measured, and this paper does not guess at its algorithm.

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
