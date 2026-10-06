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
		const write_buffer_ptr bufferPtr = serialize<options>::impl(object, ptrNew, 0);
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

On the Bool test, which serializes individual bools one at a time in a loop, this writer runs at 1,178–2,337 MB/s depending on the build, 9.5–17.3× Glaze's 91–217 MB/s. On Linux / GCC simdjson's reflection writer runs 247 MB/s, so the lead there is 7.9×.

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

On the Int64 and Uint64 tests, which serialize individual values one at a time in a loop, Jsonifier is ahead of Glaze on every build where they converged: Int64 by 1.07× on Linux / Clang, 1.13× on Linux / GCC, 1.38× on MSVC and 1.52× on M1 GCC, and Uint64 by 1.06×, 1.12×, 1.41× and 1.47× on the same builds (M1 Clang did not converge on either). simdjson's reflection writer is level with it on GCC: Jsonifier is at 0.99× on Int64 (a tie) and 1.06× on Uint64 on Linux, and at 1.02× and 1.01× on M1. With the string reused, Jsonifier is ahead of the fastest other library by 1.2–5.0× on both tests on every build.

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

Jsonifier was fastest in 104 of the 115 converged serialization tests on five builds, tied 2 and lost 9. With the output string reused across iterations it was fastest in 107 of 116, tied 1 and lost 8. Against Glaze alone it is ahead in 113 of the 115 freshly allocated tests and in all 116 reused ones. Eight of the nine freshly allocated losses are to simdjson's reflection writer, which exists only on the two GCC builds; the ninth is minified Random on Windows / MSVC, against Glaze. All eight reused losses are to the reflection writer. On the three builds with no reflection writer it is fastest in 72 of 73 freshly allocated tests and in all 72 reused. Prettified documents are where the lead is widest: 1.4–12.1× over Glaze, the only other writer there, across the five builds with freshly allocated output.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep of October 6–7, 2026: Jsonifier [6733e6d](https://github.com/nihilai-collective/jsonifier/commit/6733e6d) ([9d1ff54](https://github.com/nihilai-collective/jsonifier/commit/9d1ff54) on Windows / MSVC), Glaze [52971fe](https://github.com/stephenberry/glaze/commit/52971fe), simdjson [e9cdb87](https://github.com/simdjson/simdjson/commit/e9cdb87) ([ed60f06](https://github.com/simdjson/simdjson/commit/ed60f06) on macOS / GCC), BenchmarkSuite [c4f600a](https://github.com/nihilai-collective/benchmarksuite/commit/c4f600a). Every test runs twice: once with a freshly allocated output string per iteration, and once with a reused one (see Method). Sampling and tie rules are listed under Method below and match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection, and only for minified output, because its writer has no single-pass pretty mode. The Minify and Prettify tests reformat existing text instead of serializing objects, so they are left out of the counts.

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
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 24 | 0 | 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 22 | 16 | 1 | 5 |
| macOS / GCC 16.2 (Apple M1, NEON) | 20 | 16 | 1 | 3 |
| macOS / Clang 23.1 (Apple M1, NEON) | 23 | 23 | 0 | 0 |
| **Total** | **115** | **104** | **2** | **9** |

Every converged write test per build follows, in MB/s, freshly allocated run first and reused run after it. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,178 | 91 | 12.95× |
| Double | 215 | 145 | 1.48× |
| Int64 | 525 | 381 | 1.38× |
| String | 1,261 | 877 | 1.44× |
| Uint64 | 552 | 390 | 1.41× |
| Canada (minified) | 1,191 | 805 | 1.48× |
| Canada (prettified) | 2,613 | 1,455 | 1.80× |
| CitmCatalog (minified) | 7,011 | 4,428 | 1.58× |
| CitmCatalog (prettified) | 4,466 | 1,926 | 2.32× |
| Discord (minified) | 6,245 | 4,338 | 1.44× |
| Discord (prettified) | 9,479 | 4,430 | 2.14× |
| Google Maps Response (minified) | 6,135 | 3,459 | 1.77× |
| Google Maps Response (prettified) | 13,452 | 5,288 | 2.54× |
| Instruments (minified) | 7,845 | 3,752 | 2.09× |
| Instruments (prettified) | 13,081 | 4,354 | 3.00× |
| Marine IK Reverse (minified) | 797 | 669 | 1.19× |
| Marine IK Reverse (prettified) | 2,903 | 1,916 | 1.52× |
| Marine IK (minified) | 803 | 597 | 1.34× |
| Marine IK (prettified) | 2,820 | 1,325 | 2.13× |
| Mesh (minified) | 1,265 | 913 | 1.38× |
| Mesh (prettified) | 2,056 | 1,466 | 1.40× |
| Random (minified) | 2,764 | 2,930 | 0.94× |
| Random (prettified) | 3,822 | 2,227 | 1.72× |
| Twitter (minified) | 6,773 | 4,461 | 1.52× |
| Twitter (prettified) | 10,147 | 3,444 | 2.95× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,337 | 217 | 10.76× |
| Double | 328 | 300 | 1.10× |
| Int64 | 960 | 895 | 1.07× |
| String | 4,026 | 2,090 | 1.93× |
| Uint64 | 969 | 916 | 1.06× |
| Canada (minified) | 1,709 | 956 | 1.79× |
| Canada (prettified) | 5,156 | 3,145 | 1.64× |
| CitmCatalog (minified) | 9,585 | 5,501 | 1.74× |
| CitmCatalog (prettified) | 17,599 | 5,600 | 3.14× |
| Discord (minified) | 11,087 | 5,854 | 1.89× |
| Discord (prettified) | 15,666 | 4,845 | 3.23× |
| Google Maps Response (minified) | 4,085 | 2,312 | 1.77× |
| Google Maps Response (prettified) | 9,162 | 2,716 | 3.37× |
| Instruments (minified) | 11,727 | 4,204 | 2.79× |
| Instruments (prettified) | 21,738 | 2,835 | 7.67× |
| Marine IK Reverse (minified) | 874 | 625 | 1.40× |
| Marine IK Reverse (prettified) | 5,693 | 3,640 | 1.56× |
| Marine IK (minified) | 821 | 616 | 1.33× |
| Marine IK (prettified) | 5,659 | 2,888 | 1.96× |
| Mesh (minified) | 1,251 | 991 | 1.26× |
| Mesh (prettified) | 3,895 | 1,698 | 2.29× |
| Random (minified) | 8,248 | 3,367 | 2.45× |
| Random (prettified) | 13,457 | 3,937 | 3.42× |
| Twitter (minified) | 14,608 | 5,637 | 2.59× |
| Twitter (prettified) | 18,498 | 4,420 | 4.19× |

### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,944 | 204 | 247 | 7.86× |
| Double | 333 | 297 | 335 | 0.99× |
| Int64 | 935 | 824 | 941 | 0.99× |
| String | 3,771 | 2,023 | 3,847 | 0.98× |
| Uint64 | 954 | 850 | 904 | 1.06× |
| Canada (prettified) | 5,726 | 3,133 | 173 | 1.83× |
| CitmCatalog (minified) | 9,464 | 5,499 | 10,813 | 0.88× |
| CitmCatalog (prettified) | 17,974 | 5,808 | 482 | 3.09× |
| Discord (minified) | 11,337 | 5,772 | 10,965 | 1.03× |
| Discord (prettified) | 16,111 | 6,228 | 391 | 2.59× |
| Google Maps Response (minified) | 8,346 | 3,679 | 9,026 | 0.92× |
| Google Maps Response (prettified) | 17,086 | 5,987 | 487 | 2.85× |
| Instruments (minified) | 17,585 | 5,486 | 14,414 | 1.22× |
| Instruments (prettified) | 23,473 | 5,737 | 473 | 4.09× |
| Marine IK Reverse (minified) | 1,212 | 951 | 842 | 1.27× |
| Marine IK (minified) | 1,172 | 927 | 858 | 1.26× |
| Mesh (minified) | 2,254 | 1,395 | 1,251 | 1.62× |
| Mesh (prettified) | 4,252 | 2,600 | 157 | 1.64× |
| Random (minified) | 7,417 | 3,658 | 9,954 | 0.75× |
| Random (prettified) | 13,810 | 4,279 | 489 | 3.23× |
| Twitter (minified) | 14,157 | 6,592 | 12,459 | 1.14× |
| Twitter (prettified) | 17,053 | 5,035 | 500 | 3.39× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,032 | 118 | 211 | 9.61× |
| Double | 257 | 199 | 257 | 1.00× |
| Int64 | 754 | 496 | 740 | 1.02× |
| String | 943 | 989 | 1,211 | 0.78× |
| Uint64 | 747 | 507 | 739 | 1.01× |
| Canada (minified) | 2,272 | 1,531 | 758 | 1.48× |
| CitmCatalog (minified) | 8,558 | 1,936 | 7,296 | 1.17× |
| CitmCatalog (prettified) | 19,247 | 3,921 | 226 | 4.91× |
| Discord (minified) | 8,399 | 2,671 | 8,448 | 0.99× |
| Discord (prettified) | 9,564 | 1,982 | 345 | 4.83× |
| Google Maps Response (minified) | 7,061 | 1,461 | 8,168 | 0.86× |
| Google Maps Response (prettified) | 10,497 | 2,491 | 247 | 4.21× |
| Instruments (minified) | 12,445 | 1,582 | 7,223 | 1.72× |
| Instruments (prettified) | 15,123 | 1,660 | 221 | 9.11× |
| Marine IK Reverse (minified) | 904 | 532 | 656 | 1.38× |
| Mesh (minified) | 2,076 | 929 | 809 | 2.24× |
| Mesh (prettified) | 3,750 | 1,275 | 67 | 2.94× |
| Random (minified) | 5,182 | 1,557 | 7,125 | 0.73× |
| Random (prettified) | 8,059 | 2,135 | 260 | 3.77× |
| Twitter (prettified) | 12,277 | 3,393 | 383 | 3.62× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,704 | 112 | 15.24× |
| Double | 264 | 103 | 2.57× |
| String | 915 | 787 | 1.16× |
| Canada (minified) | 1,754 | 1,105 | 1.59× |
| Canada (prettified) | 4,447 | 2,326 | 1.91× |
| CitmCatalog (minified) | 6,183 | 1,465 | 4.22× |
| CitmCatalog (prettified) | 16,521 | 2,703 | 6.11× |
| Discord (minified) | 8,127 | 1,684 | 4.83× |
| Discord (prettified) | 11,355 | 2,651 | 4.28× |
| Google Maps Response (minified) | 6,036 | 1,529 | 3.95× |
| Google Maps Response (prettified) | 13,627 | 2,415 | 5.64× |
| Instruments (minified) | 11,960 | 1,747 | 6.85× |
| Instruments (prettified) | 21,808 | 1,808 | 12.06× |
| Marine IK Reverse (minified) | 1,338 | 821 | 1.63× |
| Marine IK Reverse (prettified) | 5,785 | 2,418 | 2.39× |
| Marine IK (minified) | 1,232 | 716 | 1.72× |
| Marine IK (prettified) | 6,573 | 2,685 | 2.45× |
| Mesh (minified) | 2,663 | 1,112 | 2.39× |
| Mesh (prettified) | 4,983 | 1,780 | 2.80× |
| Random (minified) | 5,603 | 1,829 | 3.06× |
| Random (prettified) | 8,964 | 2,385 | 3.76× |
| Twitter (minified) | 10,402 | 3,310 | 3.14× |
| Twitter (prettified) | 15,020 | 3,873 | 3.88× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 107 of 116 converged write tests. It ties String on Linux / GCC and loses 8, all to simdjson's reflection writer on the two GCC builds.

| Platform / compiler | Write tests converged | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 25 | 0 | 0 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 23 | 18 | 1 | 4 |
| macOS / GCC 16.2 (Apple M1, NEON) | 21 | 17 | 0 | 4 |
| macOS / Clang 23.1 (Apple M1, NEON) | 22 | 22 | 0 | 0 |
| **Total** | **116** | **107** | **1** | **8** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,821 | 252 | 7.24× |
| Double | 750 | 305 | 2.46× |
| Int64 | 4,437 | 1,063 | 4.17× |
| String | 8,615 | 2,412 | 3.57× |
| Uint64 | 5,329 | 1,057 | 5.04× |
| Canada (minified) | 1,496 | 1,086 | 1.38× |
| Canada (prettified) | 4,480 | 2,944 | 1.52× |
| CitmCatalog (minified) | 7,076 | 4,971 | 1.42× |
| CitmCatalog (prettified) | 14,323 | 7,335 | 1.95× |
| Discord (minified) | 6,421 | 4,848 | 1.32× |
| Discord (prettified) | 9,814 | 5,128 | 1.91× |
| Google Maps Response (minified) | 6,705 | 3,880 | 1.73× |
| Google Maps Response (prettified) | 14,654 | 5,977 | 2.45× |
| Instruments (minified) | 8,143 | 4,685 | 1.74× |
| Instruments (prettified) | 12,763 | 5,213 | 2.45× |
| Marine IK Reverse (minified) | 889 | 734 | 1.21× |
| Marine IK Reverse (prettified) | 4,407 | 3,304 | 1.33× |
| Marine IK (minified) | 910 | 746 | 1.22× |
| Marine IK (prettified) | 4,595 | 3,304 | 1.39× |
| Mesh (minified) | 1,623 | 1,278 | 1.27× |
| Mesh (prettified) | 2,967 | 2,126 | 1.40× |
| Random (minified) | 5,116 | 3,170 | 1.61× |
| Random (prettified) | 8,830 | 4,601 | 1.92× |
| Twitter (minified) | 7,355 | 5,327 | 1.38× |
| Twitter (prettified) | 10,124 | 3,858 | 2.62× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 4,484 | 793 | 5.65× |
| Double | 830 | 673 | 1.23× |
| Int64 | 5,011 | 2,402 | 2.09× |
| String | 10,197 | 5,750 | 1.77× |
| Uint64 | 4,222 | 2,567 | 1.64× |
| Canada (minified) | 1,741 | 995 | 1.75× |
| Canada (prettified) | 5,177 | 3,792 | 1.37× |
| CitmCatalog (minified) | 9,851 | 6,384 | 1.54× |
| CitmCatalog (prettified) | 17,923 | 7,652 | 2.34× |
| Discord (minified) | 11,688 | 6,672 | 1.75× |
| Discord (prettified) | 15,233 | 5,403 | 2.82× |
| Google Maps Response (minified) | 9,092 | 4,388 | 2.07× |
| Google Maps Response (prettified) | 9,765 | 3,318 | 2.94× |
| Instruments (minified) | 8,267 | 3,973 | 2.08× |
| Instruments (prettified) | 22,493 | 6,411 | 3.51× |
| Marine IK Reverse (minified) | 858 | 624 | 1.37× |
| Marine IK Reverse (prettified) | 5,534 | 4,010 | 1.38× |
| Marine IK (minified) | 819 | 629 | 1.30× |
| Marine IK (prettified) | 5,628 | 3,853 | 1.46× |
| Mesh (minified) | 1,250 | 1,037 | 1.21× |
| Mesh (prettified) | 3,875 | 1,742 | 2.22× |
| Random (minified) | 8,252 | 3,706 | 2.23× |
| Random (prettified) | 14,138 | 4,491 | 3.15× |
| Twitter (minified) | 14,381 | 6,565 | 2.19× |
| Twitter (prettified) | 18,442 | 5,087 | 3.63× |

#### Linux / GCC 16.1 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,906 | 759 | 3,100 | 0.94× |
| Double | 816 | 690 | 619 | 1.18× |
| Int64 | 5,035 | 2,349 | 4,260 | 1.18× |
| String | 9,171 | 5,886 | 9,250 | 0.99× |
| Uint64 | 5,574 | 2,391 | 4,241 | 1.31× |
| Canada (minified) | 1,956 | 1,486 | 994 | 1.32× |
| Canada (prettified) | 5,630 | 3,723 | 171 | 1.51× |
| CitmCatalog (minified) | 9,591 | 6,318 | 11,072 | 0.87× |
| Discord (minified) | 11,551 | 6,434 | 10,964 | 1.05× |
| Discord (prettified) | 16,358 | 7,210 | 304 | 2.27× |
| Google Maps Response (minified) | 8,803 | 4,487 | 9,184 | 0.96× |
| Google Maps Response (prettified) | 17,654 | 6,937 | 488 | 2.54× |
| Instruments (minified) | 18,094 | 6,518 | 14,622 | 1.24× |
| Instruments (prettified) | 23,772 | 6,846 | 481 | 3.47× |
| Marine IK Reverse (minified) | 1,218 | 961 | 844 | 1.27× |
| Marine IK Reverse (prettified) | 5,950 | 3,900 | 197 | 1.53× |
| Marine IK (minified) | 1,183 | 961 | 862 | 1.23× |
| Mesh (minified) | 2,257 | 1,498 | 1,233 | 1.51× |
| Mesh (prettified) | 4,123 | 2,761 | 156 | 1.49× |
| Random (minified) | 7,496 | 3,955 | 10,230 | 0.73× |
| Random (prettified) | 13,590 | 5,031 | 498 | 2.70× |
| Twitter (minified) | 13,920 | 7,779 | 11,884 | 1.17× |
| Twitter (prettified) | 18,276 | 5,640 | 511 | 3.24× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,263 | 815 | 2,580 | 1.27× |
| Double | 829 | 425 | 664 | 1.25× |
| Int64 | 5,086 | 2,090 | 3,009 | 1.69× |
| String | 7,291 | 4,819 | 10,014 | 0.73× |
| Uint64 | 5,067 | 2,307 | 3,067 | 1.65× |
| Canada (minified) | 2,276 | 1,614 | 755 | 1.41× |
| CitmCatalog (minified) | 8,552 | 2,061 | 7,329 | 1.17× |
| CitmCatalog (prettified) | 16,795 | 4,559 | 236 | 3.68× |
| Discord (minified) | 8,592 | 2,982 | 8,727 | 0.98× |
| Discord (prettified) | 10,214 | 2,603 | 327 | 3.92× |
| Google Maps Response (minified) | 7,132 | 1,616 | 7,875 | 0.91× |
| Google Maps Response (prettified) | 10,709 | 2,733 | 253 | 3.92× |
| Instruments (minified) | 11,626 | 1,769 | 7,278 | 1.60× |
| Instruments (prettified) | 15,003 | 1,455 | 192 | 10.31× |
| Marine IK (minified) | 1,058 | 707 | 649 | 1.50× |
| Mesh (minified) | 1,954 | 926 | 774 | 2.11× |
| Mesh (prettified) | 3,659 | 1,241 | 88 | 2.95× |
| Random (minified) | 5,209 | 1,759 | 8,179 | 0.64× |
| Random (prettified) | 7,599 | 2,168 | 296 | 3.50× |
| Twitter (minified) | 9,690 | 3,660 | 8,787 | 1.10× |
| Twitter (prettified) | 12,463 | 3,929 | 383 | 3.17× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,599 | 635 | 4.09× |
| Int64 | 2,779 | 1,401 | 1.98× |
| Uint64 | 3,478 | 1,800 | 1.93× |
| Canada (prettified) | 4,830 | 1,939 | 2.49× |
| CitmCatalog (minified) | 6,140 | 1,511 | 4.07× |
| CitmCatalog (prettified) | 16,662 | 2,755 | 6.05× |
| Discord (minified) | 8,436 | 2,592 | 3.25× |
| Discord (prettified) | 10,846 | 2,996 | 3.62× |
| Google Maps Response (minified) | 6,158 | 1,595 | 3.86× |
| Google Maps Response (prettified) | 15,147 | 2,554 | 5.93× |
| Instruments (minified) | 11,021 | 1,992 | 5.53× |
| Instruments (prettified) | 22,270 | 2,372 | 9.39× |
| Marine IK Reverse (minified) | 1,466 | 819 | 1.79× |
| Marine IK Reverse (prettified) | 5,930 | 2,690 | 2.20× |
| Marine IK (minified) | 1,302 | 644 | 2.02× |
| Marine IK (prettified) | 6,583 | 3,066 | 2.15× |
| Mesh (minified) | 2,665 | 1,152 | 2.31× |
| Mesh (prettified) | 4,989 | 1,830 | 2.73× |
| Random (minified) | 5,612 | 1,946 | 2.88× |
| Random (prettified) | 9,009 | 2,578 | 3.49× |
| Twitter (minified) | 10,512 | 4,041 | 2.60× |
| Twitter (prettified) | 15,250 | 4,443 | 3.43× |

### Losses

The prettified lead comes from the indent tables: a newline plus indent is a few 8-byte stores, not a loop. Linux / Clang and macOS / Clang have no loss or tie in either run, and Windows / MSVC has one loss, freshly allocated. The rest sit on the two GCC builds, and all but one of the nine freshly allocated losses, and all eight reused ones, are to simdjson's reflection writer:

- **simdjson reflection writer, Linux / GCC and macOS / GCC.** Freshly allocated, Jsonifier is behind it on five results on Linux / GCC (Double at 0.99×, String at 0.98×, and minified CitmCatalog at 0.88×, Google Maps at 0.92× and Random at 0.75×) and on three on macOS / GCC (String at 0.78×, and minified Google Maps at 0.86× and Random at 0.73×). Reused, the Linux / GCC losses are Bool at 0.94× and minified CitmCatalog at 0.87×, Google Maps at 0.96× and Random at 0.73×, and the macOS / GCC losses are String at 0.73× and minified Discord at 0.98×, Google Maps at 0.91× and Random at 0.64×. On the same two builds Jsonifier is ahead of the reflection writer on minified Mesh and Marine IK (freshly allocated on Linux / GCC: 1.80× and 1.37–1.44×) and on Instruments (1.22× and 1.72×), and it is ahead on Double reused (1.32× and 1.25×). The documents it loses are mostly built from strings and integers with few or no floating-point values (Random, Google Maps, CitmCatalog), and the ones it wins are mostly arrays of doubles (Canada, Mesh, Marine IK). Discord and Twitter go either way across the two builds (Discord at 1.03× on Linux / GCC and 0.99× on macOS / GCC, freshly allocated), so this is a tendency and not a rule. Not profiled yet.
- **String test on macOS / GCC.** Freshly allocated, Jsonifier runs 943 MB/s against Glaze's 989 (0.95×) and the reflection writer's 1,211 (0.78×). Reused it is ahead of Glaze, 7,291 vs 4,819 MB/s (1.51×), but behind the reflection writer's 10,014 (0.73×). On macOS / Clang it is ahead of Glaze freshly allocated, 915 vs 787 MB/s (1.16×); the reused String test did not converge there. On the AVX2 builds Jsonifier wins the same test over Glaze by 1.4–1.9× freshly allocated, so the gap sits in the NEON escape path on GCC.
- **Random (minified), Windows / MSVC.** Glaze runs 2,930 vs 2,764 MB/s (Jsonifier at 0.94×) freshly allocated. Reused, Jsonifier is ahead of it, 5,116 vs 3,170 MB/s (1.61×).

The two freshly allocated ties are Int64 on Linux / GCC (0.99× against the reflection writer) and minified Discord on macOS / GCC (0.99×, also against the reflection writer). The one reused tie is String on Linux / GCC (0.99×).

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

### Why is the String test slower on NEON than on AVX2?

Only on one M1 build, and only freshly allocated. On macOS / GCC Jsonifier runs 943 vs Glaze's 989 MB/s (0.95×) freshly allocated, and simdjson's reflection writer is ahead of it at 1,211 MB/s (0.78×). Reused, Jsonifier is ahead of Glaze (7,291 vs 4,819 MB/s) but the reflection writer is still ahead at 10,014 MB/s (0.73×). On macOS / Clang Jsonifier is ahead of Glaze freshly allocated (915 vs 787 MB/s, 1.16×), and the reused run did not converge. On AVX2 Jsonifier wins the String test over Glaze by 1.4–1.9× freshly allocated, and the reflection writer is level with it on Linux / GCC (0.98× freshly allocated, a narrow loss, and 0.99× reused, a tie).

Not profiled yet. Two features of the current loop are candidates:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

The same code beats Glaze on every AVX2 build and on macOS / Clang, and trails it only on macOS / GCC freshly allocated, so the Glaze gap is specific to that one NEON build. Whether either candidate is the cause is open.

### Does simdjson's writer win doubles on Linux GCC?

Only by a hair, and only freshly allocated. On the Double test Jsonifier runs 333 vs 335 MB/s (0.99×, a narrow loss that the t-test still separates) freshly allocated and is ahead of it reused (816 vs 619 MB/s, 1.32×). On macOS / GCC it is level freshly allocated (257 vs 257 MB/s, 1.00×) and ahead reused (829 vs 664 MB/s, 1.25×). It also beats Glaze on the Double test on all five builds freshly allocated, by 1.10–2.57×. What the reflection writer does win on GCC is String, some of the Bool, Int64 and Uint64 results, and the minified documents listed under Losses. Why its writer is faster on those has not been measured, and this paper does not guess at its algorithm.

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
