/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/simd.hpp
 */
// Sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/add_tape_values.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/core/fastio.hpp>

namespace jsonifier::internal {

	inline static void printBitsAligned(uint64_t bits, read_buffer_ptr label, read_buffer_ptr __restrict str = nullptr, uint64_t len = 0) noexcept {
		out << label << ":" << endl;
		if (str && len > 0) {
			out << "STR:  ";
			for (uint64_t i = 0; i < std::min<uint64_t>(len, 64); ++i) {
				char c = str[i];
				if (c == '\n' || c == '\r' || c == '\t') {
					c = ' ';
				}
				out << c;
			}
			out << endl;
		}
		out << "BITS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << ((bits >> i) & 1ULL);
		}
		out << endl;
		out << "IDX:  ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << (i % 10);
		}
		out << endl;
		out << "TENS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << ((i / 10) % 10);
		}
		out << endl << endl;
	}

	struct string_block_reader {
		static constexpr uint64_t stepBytes = simdBlocksPerStep * 64;

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			lengthMinusStep = lengthNew < stepBytes ? 0 : lengthNew - stepBytes;
			inString		= std::bit_cast<const uint8_t*>(stringViewNew);
			length			= lengthNew;
			index			= 0;
		}

		JSONIFIER_INLINE const uint8_t* getRemainder() noexcept {
			if (length == index) [[unlikely]] {
				return nullptr;
			}
			std::memset(+block + (length - index), static_cast<uint8_t>(0x20), stepBytes - (length - index));
			jsonifierMemcpy(+block, inString + index, length - index);
			return +block;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return length - index;
		}

		JSONIFIER_INLINE const uint8_t* fullBlock() noexcept {
			const uint8_t* newPtr = inString + index;
			index += stepBytes;
			return newPtr;
		}

		JSONIFIER_INLINE bool hasFullBlock() const noexcept {
			return index < lengthMinusStep;
		}

		alignas(64) uint8_t block[stepBytes]{};
		uint64_t lengthMinusStep{};
		const uint8_t* inString{};
		uint64_t length{};
		uint64_t index{};
	};

	struct string_block_reader_locals {
		static constexpr uint64_t stepBytes = simdBlocksPerStep * 64;

		JSONIFIER_INLINE const uint8_t* getRemainder(uint64_t paddedBytes) noexcept {
			uint8_t* __restrict blockPtr	 = +block;
			const uint8_t* __restrict srcPtr = inString + index;
			std::memset(blockPtr + (length - index), static_cast<uint8_t>(0x20), paddedBytes - (length - index));
			jsonifierMemcpy(blockPtr, srcPtr, length - index);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr __restrict stringViewNew, uint64_t lengthNew) noexcept {
			lengthMinusStep = lengthNew < stepBytes ? 0 : lengthNew - stepBytes;
			inString		= std::bit_cast<const uint8_t*>(stringViewNew);
			length			= lengthNew;
			index			= 0;
		}

		alignas(64) uint8_t block[stepBytes]{};
		const uint8_t* __restrict inString{};
		uint64_t lengthMinusStep{};
		uint64_t length{};
		uint64_t index{};
	};

	template<uint64_t stepBytes = simdBytesPerStep> struct pod_block_reader {
		JSONIFIER_INLINE const uint8_t* getRemainder() noexcept {
			const uint64_t remaining		 = lengthVal - indexVal;
			uint8_t* __restrict blockPtr	 = +block;
			const uint8_t* __restrict srcPtr = inString + indexVal;
			jsonifierMemcpy(blockPtr, srcPtr, remaining);
			std::memset(blockPtr + remaining, static_cast<uint8_t>(0x20), simdBytesPerBlock - remaining);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr __restrict stringViewNew, uint64_t lengthNew) noexcept {
			fullStepEnd	 = lengthNew & ~(stepBytes - 1ull);
			fullBlockEnd = lengthNew & ~(simdBytesPerBlock - 1ull);
			inString	 = std::bit_cast<const uint8_t*>(stringViewNew);
			lengthVal	 = lengthNew;
			indexVal	 = 0;
		}

		JSONIFIER_INLINE const uint8_t* fullBlock() noexcept {
			const uint8_t* __restrict newPtr = inString + indexVal;
			indexVal += simdBytesPerBlock;
			return newPtr;
		}

		JSONIFIER_INLINE const uint8_t* fullStep() noexcept {
			const uint8_t* __restrict newPtr = inString + indexVal;
			indexVal += stepBytes;
			return newPtr;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return lengthVal - indexVal;
		}

		JSONIFIER_INLINE bool hasFullBlock() const noexcept {
			return indexVal < fullBlockEnd;
		}

		JSONIFIER_INLINE bool hasFullStep() const noexcept {
			return indexVal < fullStepEnd;
		}

		JSONIFIER_INLINE uint64_t length() const noexcept {
			return lengthVal;
		}

		JSONIFIER_INLINE uint64_t index() const noexcept {
			return indexVal;
		}

		alignas(64) uint8_t block[simdBytesPerBlock];
		const uint8_t* __restrict inString{};
		uint64_t fullBlockEnd{};
		uint64_t fullStepEnd{};
		uint64_t lengthVal{};
		uint64_t indexVal{};

		static_assert((stepBytes & (stepBytes - 1)) == 0 && stepBytes >= simdBytesPerBlock);
	};

	struct rope_block {
		uint64_t escaped{};
		uint64_t quotes{};
		uint64_t inString{};

		JSONIFIER_INLINE uint64_t stringTail() const noexcept {
			return inString ^ quotes;
		}

		JSONIFIER_INLINE uint64_t nonQuoteOutsideString(uint64_t mask) const noexcept {
			return mask & ~inString;
		}
	};

	template<uint64_t initialBufferSize>
	struct simd_string_reader_locals : string_block_reader_locals, add_tape_values<make_integer_sequence<simdBlocksPerStep>>, alloc_wrapper<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		using allocator = alloc_wrapper<uint32_t>;
		using rope_type = simd::rope_detector<rope_block>;

		template<bool minified> JSONIFIER_INLINE static uint64_t processBlocks(rope_type& __restrict rope, const uint8_t* __restrict blockPtr,
			structural_index_ptr __restrict tapePtr, uint64_t stepBaseIndex) noexcept {
			array<uint64_t, simdBlocksPerStep> bitsArr;
			array<uint64_t, simdBlocksPerStep> cntsArr;
			processBlocksImpl<minified, 0>(rope, bitsArr, cntsArr, blockPtr);
			if constexpr (simdBlocksPerStep > 1) {
				processBlocksImpl<minified, 1>(rope, bitsArr, cntsArr, blockPtr);
				if constexpr (simdBlocksPerStep > 2) {
					processBlocksImpl<minified, 2>(rope, bitsArr, cntsArr, blockPtr);
					processBlocksImpl<minified, 3>(rope, bitsArr, cntsArr, blockPtr);
					if constexpr (simdBlocksPerStep > 4) {
						processBlocksImpl<minified, 4>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 5>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 6>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 7>(rope, bitsArr, cntsArr, blockPtr);
					}
				}
			}

			add_tape_values<make_integer_sequence<simdBlocksPerStep>>::impl(bitsArr, cntsArr, tapePtr, stepBaseIndex);

			uint64_t stepCount = cntsArr[0];
			if constexpr (simdBlocksPerStep > 1) {
				stepCount += cntsArr[1];
				if constexpr (simdBlocksPerStep > 2) {
					stepCount += cntsArr[2];
					stepCount += cntsArr[3];
					if constexpr (simdBlocksPerStep > 4) {
						stepCount += cntsArr[4];
						stepCount += cntsArr[5];
						stepCount += cntsArr[6];
						stepCount += cntsArr[7];
					}
				}
			}
			return stepCount;
		}

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape = allocator::allocate(neededCapacity);
				allocator::deallocate(tape, capacity);
				tape	 = newTape;
				capacity = neededCapacity;
			}

			string_block_reader_locals::reset(rootIter, stringLength);

			resetImpl<minified>();
		}

		template<bool minified, uint64_t I, uint64_t laneCount> JSONIFIER_INLINE static void processBlocksImpl(rope_type& __restrict rope,
			array<uint64_t, laneCount>& __restrict bitsArr, array<uint64_t, laneCount>& __restrict cntsArr, const uint8_t* __restrict blockPtr) noexcept {
			simd_array_t inVals;
			inVals.template set<0>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64));
			if constexpr (simdRegistersPerBlock > 1) {
				inVals.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 1));
				if constexpr (simdRegistersPerBlock > 2) {
					inVals.template set<2>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 2));
					inVals.template set<3>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 3));
				}
			}
			rope.next(inVals);
			if constexpr (minified) {
				const uint64_t structurals = getStructurals(rope, inVals) & ~rope.stringTail();
				bitsArr[I]				   = structurals;
				cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
			} else {
				const uint64_t structurals = getStructuralsWs(rope, inVals) & ~rope.stringTail();
				bitsArr[I]				   = structurals;
				cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
			}
		}

		template<bool minified> JSONIFIER_INLINE void resetImpl() noexcept {
			rope_type rope{};
			structural_index_ptr tapeLocal		= tape;
			const uint8_t* __restrict srcPtr	= string_block_reader_locals::inString;
			const uint64_t lengthMinusStepLocal = string_block_reader_locals::lengthMinusStep;
			const uint64_t lengthLocal			= string_block_reader_locals::length;
			uint64_t tapeCountLocal				= 0;
			uint64_t indexLocal					= 0;

			while (indexLocal < lengthMinusStepLocal) {
				tapeCountLocal += processBlocks<minified>(rope, srcPtr + indexLocal, tapeLocal + tapeCountLocal, indexLocal);
				indexLocal += stepBytes;
			}
			string_block_reader_locals::index = indexLocal;

			if (const uint64_t remaining = lengthLocal - indexLocal; remaining != 0) {
				const uint64_t tailBlocks		  = (remaining + 63) / 64;
				const uint8_t* __restrict tailPtr = string_block_reader_locals::getRemainder(tailBlocks * 64);
				for (uint64_t blockIndex = 0; blockIndex < tailBlocks; ++blockIndex) {
					tapeCountLocal += processTailBlock<minified>(rope, tailPtr + blockIndex * 64, tapeLocal + tapeCountLocal, indexLocal + blockIndex * 64);
				}
				while (tapeCountLocal > 0 && tapeLocal[tapeCountLocal - 1] >= lengthLocal) {
					--tapeCountLocal;
				}
			}
			tapeCount = tapeCountLocal;
		}

		JSONIFIER_INLINE static uint64_t getStructuralsWs(rope_type& __restrict rope, const simd_array_t in_01) noexcept {
			const uint64_t whitespace  = simd::ws_collector::impl(in_01);
			const uint64_t op		   = simd::op_collector::impl(in_01);
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | whitespace | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
		}

		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const simd_array_t in_01) noexcept {
			const uint64_t op		   = simd::op_collector::impl(in_01);
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
		}

		template<bool minified> JSONIFIER_INLINE static uint64_t processTailBlock(rope_type& __restrict rope, const uint8_t* __restrict blockPtr,
			structural_index_ptr __restrict tapePtr, uint64_t blockBaseIndex) noexcept {
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			processBlocksImpl<minified, 0>(rope, bitsArr, cntsArr, blockPtr);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tapePtr, blockBaseIndex);
			return cntsArr[0];
		}

		JSONIFIER_INLINE simd_string_reader_locals& operator=(const simd_string_reader_locals& other) noexcept {
			if (&other != this) {
				if (capacity < other.capacity) {
					auto newTape = allocator::allocate(other.capacity);
					if (tape) {
						allocator::deallocate(tape, capacity);
					}
					tape	 = newTape;
					capacity = other.capacity;
				}
				tapeCount						   = other.tapeCount;
				string_block_reader_locals::length = other.string_block_reader_locals::length;
				if (other.tape) {
					jsonifierMemcpy(tape, other.tape, sizeof(*tape) * (other.tapeCount + 1));
				}
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader_locals& operator=(simd_string_reader_locals&& other) noexcept {
			if (&other != this) {
				std::swap(tapeCount, other.tapeCount);
				std::swap(capacity, other.capacity);
				std::swap(string_block_reader_locals::length, other.string_block_reader_locals::length);
				std::swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE structural_index_ptr begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(string_block_reader_locals::length);
			return tape;
		}

		JSONIFIER_INLINE simd_string_reader_locals() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		JSONIFIER_INLINE ~simd_string_reader_locals() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

		JSONIFIER_INLINE simd_string_reader_locals(simd_string_reader_locals&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		JSONIFIER_INLINE simd_string_reader_locals(const simd_string_reader_locals& other) noexcept : allocator{} {
			*this = other;
		}

		JSONIFIER_INLINE structural_index_ptr end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		structural_index_ptr __restrict tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
	};

	template<uint64_t initialBufferSize> using simd_string_reader = simd_string_reader_locals<initialBufferSize>;

	template<uint64_t initialBufferSize> struct pod_simd_string_reader : alloc_wrapper<uint32_t> {
		using allocator = alloc_wrapper<uint32_t>;
		using rope_type = simd::rope_detector<rope_block>;

		template<bool minified, uint64_t blocksPerStep, uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE static uint64_t processBlocks(rope_type& __restrict rope,
			const uint8_t* __restrict blockPtr, structural_index_ptr __restrict tapePtr, uint64_t stepBaseIndex) noexcept {
			array<uint64_t, blocksPerStep> bitsArr;
			array<uint64_t, blocksPerStep> cntsArr;
			processBlocksImpl<minified, 0, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
			if constexpr (blocksPerStep > 1) {
				processBlocksImpl<minified, 1, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
				if constexpr (blocksPerStep > 2) {
					processBlocksImpl<minified, 2, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
					processBlocksImpl<minified, 3, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
					if constexpr (blocksPerStep > 4) {
						processBlocksImpl<minified, 4, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 5, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 6, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
						processBlocksImpl<minified, 7, registerBytes, registerCount>(rope, bitsArr, cntsArr, blockPtr);
					}
				}
			}

			add_tape_values<make_integer_sequence<blocksPerStep>>::impl(bitsArr, cntsArr, tapePtr, stepBaseIndex);

			uint64_t stepCount = cntsArr[0];
			if constexpr (blocksPerStep > 1) {
				stepCount += cntsArr[1];
				if constexpr (blocksPerStep > 2) {
					stepCount += cntsArr[2];
					stepCount += cntsArr[3];
					if constexpr (blocksPerStep > 4) {
						stepCount += cntsArr[4];
						stepCount += cntsArr[5];
						stepCount += cntsArr[6];
						stepCount += cntsArr[7];
					}
				}
			}
			return stepCount;
		}

		template<bool minified, uint64_t registerBytes, uint64_t registerCount>
		JSONIFIER_INLINE void resetImpl(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			static constexpr uint64_t blocksPerStep{ simdBytesPerStep / simdBytesPerBlock };
			pod_block_reader<simdBytesPerStep> stringBlockReader;
			stringBlockReader.reset(rootIter, stringLength);
			rope_type rope{};
			structural_index_ptr tapeLocal = tape;
			uint64_t tapeCountLocal		   = 0;
			if constexpr (registerCount * registerBytes == simdBytesPerBlock) {
				while (stringBlockReader.hasFullStep()) {
					const uint64_t stepBaseIndex = stringBlockReader.index();
					tapeCountLocal +=
						processBlocks<minified, blocksPerStep, registerBytes, registerCount>(rope, stringBlockReader.fullStep(), tapeLocal + tapeCountLocal, stepBaseIndex);
				}

				while (stringBlockReader.hasFullBlock()) {
					const uint64_t blockBaseIndex = stringBlockReader.index();
					tapeCountLocal += processBlocks<minified, 1, registerBytes, registerCount>(rope, stringBlockReader.fullBlock(), tapeLocal + tapeCountLocal, blockBaseIndex);
				}
			}

			if (stringBlockReader.getRemainderBytes() != 0) {
				const uint64_t blockBaseIndex = stringBlockReader.index();
				tapeCountLocal += processBlocks<minified, 1, registerBytes, registerCount>(rope, stringBlockReader.getRemainder(), tapeLocal + tapeCountLocal, blockBaseIndex);
				while (tapeCountLocal > 0 && tapeLocal[tapeCountLocal - 1] >= stringBlockReader.length()) {
					--tapeCountLocal;
				}
			}
			tapeCount = tapeCountLocal;
			length	  = stringBlockReader.length();
		}

		template<bool minified, uint64_t I, uint64_t registerBytes, uint64_t registerCount, uint64_t blocksPerStep>
		JSONIFIER_INLINE static void processBlocksImpl(rope_type& __restrict rope, array<uint64_t, blocksPerStep>& __restrict bitsArr,
			array<uint64_t, blocksPerStep>& __restrict cntsArr, const uint8_t* __restrict blockPtr) noexcept {
			using simd_type = typename simd_register<registerBytes>::type;
			pod_simd_array_t<registerCount, registerBytes> inVals;
			inVals.template set<0>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock));
			if constexpr (registerCount > 1) {
				inVals.template set<1>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 1));
				if constexpr (registerCount > 2) {
					inVals.template set<2>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 2));
					inVals.template set<3>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 3));
				}
			}
			rope.template nextScalar<registerBytes, registerCount>(inVals);
			if constexpr (minified) {
				const uint64_t structurals = getStructurals<registerBytes, registerCount>(rope, inVals) & ~rope.stringTail();
				bitsArr[I]				   = structurals;
				cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
			} else {
				const uint64_t structurals = getStructuralsWs<registerBytes, registerCount>(rope, inVals) & ~rope.stringTail();
				bitsArr[I]				   = structurals;
				cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
			}
		}

		template<bool minified, uint64_t registerBytes, uint64_t registerCount>
		JSONIFIER_INLINE void resetDispatch(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			using simd_type				  = typename simd_register<registerBytes>::type;
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape = allocator::allocate(neededCapacity);
				if (tape) {
					allocator::deallocate(tape, capacity);
				}
				tape	 = newTape;
				capacity = neededCapacity;
			}

			if constexpr (minified) {
				resetImpl<minified, registerBytes, registerCount>(rootIter, stringLength);
			} else {
				const simd_type whitespaceTableLocal = simd::gatherValues<simd_type>(simd::whitespaceArray<registerBytes>.data());
				resetImpl<minified, registerBytes, registerCount>(rootIter, stringLength);
			}
		}

		template<uint64_t registerBytes, uint64_t registerCount>
		JSONIFIER_INLINE static uint64_t getStructuralsWs(rope_type& __restrict rope, const pod_simd_array_t<registerCount, registerBytes> in_01) noexcept {
			const uint64_t whitespace  = simd::pod_ws_collector<registerBytes, registerCount>::impl(in_01);
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in_01);
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | whitespace | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
		}

		template<uint64_t registerBytes, uint64_t registerCount>
		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const pod_simd_array_t<registerCount, registerBytes> in_01) noexcept {
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in_01);
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
		}

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
			if (stringLength <= 32) {
				return resetDispatch<minified, 32, 1>(rootIter, stringLength);
			}
	#endif
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
			if (stringLength <= 32) {
				return resetDispatch<minified, 16, 2>(rootIter, stringLength);
			}
#endif
			resetDispatch<minified, simdBytesPerRegister, simdRegistersPerBlock>(rootIter, stringLength);
		}

		JSONIFIER_INLINE pod_simd_string_reader& operator=(const pod_simd_string_reader& other) noexcept {
			if (&other != this) {
				if (capacity < other.capacity) {
					auto newTape = allocator::allocate(other.capacity);
					if (tape) {
						allocator::deallocate(tape, capacity);
					}
					tape	 = newTape;
					capacity = other.capacity;
				}
				tapeCount = other.tapeCount;
				length	  = other.length;
				if (other.tape) {
					jsonifierMemcpy(tape, other.tape, sizeof(*tape) * (other.tapeCount + 1));
				}
			}
			return *this;
		}

		JSONIFIER_INLINE pod_simd_string_reader& operator=(pod_simd_string_reader&& other) noexcept {
			if (&other != this) {
				std::swap(tapeCount, other.tapeCount);
				std::swap(capacity, other.capacity);
				std::swap(length, other.length);
				std::swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE pod_simd_string_reader() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		JSONIFIER_INLINE ~pod_simd_string_reader() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

		JSONIFIER_INLINE pod_simd_string_reader(pod_simd_string_reader&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		JSONIFIER_INLINE structural_index_ptr begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(length);
			return tape;
		}

		JSONIFIER_INLINE pod_simd_string_reader(const pod_simd_string_reader& other) noexcept : allocator{} {
			*this = other;
		}

		JSONIFIER_INLINE structural_index_ptr end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		structural_index_ptr __restrict tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
		uint64_t length{};
	};

}
