/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/validator.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>

namespace jsonifier::internal {

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value + strLen(value));
	}

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value);
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type& value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value.data() + value.size());
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type& value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value.data());
	}

	template<typename derived_type_new> struct validator {
		using derived_type = derived_type_new;

		static constexpr parse_options optionsVal{};

		using cursor = json_cursor<optionsVal, structural_index_ptr>;

		template<string_t string_type> inline bool validateJson(string_type&& in) noexcept {
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			derivedRef.section.template reset<optionsVal.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			parse_context<optionsVal, structural_index_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors, rootIter,
				endIter, derivedRef.section.end() };
			auto newSize = static_cast<uint64_t>(endIter - rootIter) / 2;
			if (derivedRef.stringBuffer.size() < newSize) {
				derivedRef.stringBuffer.resize(newSize);
			}
			derivedRef.errors.clear();
			const structural_index_ptr iter{ derivedRef.section.begin() };
			if (!cursor::anyInput(iter, context)) {
				return false;
			}
			const structural_index_ptr iterNew = impl(iter, 0, context);
			if (!iterNew) {
				return false;
			}
			static_cast<void>(cursor::checkIfDone(iterNew, context));
			return derivedRef.errors.size() == 0;
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		validator& operator=(const validator& other) = delete;
		validator& operator=(validator&& other)		 = delete;
		validator(const validator& other)			 = delete;
		validator(validator&& other)				 = delete;
		inline ~validator() noexcept				 = default;
		inline validator() noexcept					 = default;

		template<typename context_type> inline static structural_index_ptr impl(structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::notAtEnd(iter, context)) {
				return nullptr;
			}
			const auto c = *cursor::valuePtr(iter, context);
			if (c == '{') {
				return validateObject(iter, depth, context);
			} else if (c == '[') {
				return validateArray(iter, depth, context);
			} else if (c == '"') {
				return validateString(iter, context);
			} else if (numberTable[static_cast<uint8_t>(c)]) {
				return validateNumber(iter, context);
			} else if (boolTable[static_cast<uint8_t>(c)]) {
				return validateBool(iter, context);
			} else if (c == 'n') {
				return validateNull(iter, context);
			} else {
				return nullptr;
			}
		}

		template<typename context_type> inline static structural_index_ptr validateObject(structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'{'>(iter, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<'}'>(iter, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, context)) {
				iter = validateString(iter, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::template checkChar<':'>(iter, context)) [[unlikely]] {
					return nullptr;
				}
				++iter;
				iter = validator<derived_type_new>::impl(iter, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<'}'>(iter, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		template<typename context_type> inline static structural_index_ptr validateArray(structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'['>(iter, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<']'>(iter, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, context)) {
				iter = validator<derived_type_new>::impl(iter, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<']'>(iter, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		// The structural index holds only the first byte of each scalar, so the end of a scalar is not indexed. A scalar ends
		// either at the next structural index or at the end of the input, and only whitespace may come between the two.
		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr scalarBound(structural_index_ptr nextIter, context_type& context) noexcept {
			return cursor::notAtEnd(nextIter, context) ? cursor::valuePtr(nextIter, context) : context.stringEnd;
		}

		JSONIFIER_INLINE static bool onlyWhitespace(read_buffer_ptr ptr, read_buffer_ptr bound) noexcept {
			for (; ptr < bound; ++ptr) {
				if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
					return false;
				}
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static structural_index_ptr validateString(structural_index_ptr iter, context_type& context) noexcept {
			if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
				return nullptr;
			}
			const auto contentPtr = cursor::valuePtr(iter, context) + 1;
			++iter;
			const auto bound   = scalarBound(iter, context);
			using scanner_type = string_scanner<optionsVal>;
			auto& scratch	   = context.getStringBuffer();
			const auto needed  = static_cast<uint64_t>(bound - contentPtr) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto result = scanner_type::impl(contentPtr, bound, scratch.data());
			if (result.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return nullptr;
			}
			// rawLength is the offset of the closing quote.
			return onlyWhitespace(contentPtr + result.rawLength + 1, bound) ? iter : nullptr;
		}

		// RFC 8259: [ "-" ] ( "0" / digit1-9 *DIGIT ) [ "." 1*DIGIT ] [ ( "e" / "E" ) [ "-" / "+" ] 1*DIGIT ]
		template<typename context_type> JSONIFIER_INLINE static structural_index_ptr validateNumber(structural_index_ptr iter, context_type& context) noexcept {
			auto newPtr = cursor::valuePtr(iter, context);
			++iter;
			const auto bound = scalarBound(iter, context);
			consumeChar('-', newPtr, bound);
			if (consumeChar('0', newPtr, bound)) {
				if (newPtr < bound && is_digit(static_cast<uint8_t>(*newPtr))) [[unlikely]] {
					return nullptr;
				}
			} else if (!consumeDigits(newPtr, bound)) [[unlikely]] {
				return nullptr;
			}
			if (consumeChar('.', newPtr, bound) && !consumeDigits(newPtr, bound)) [[unlikely]] {
				return nullptr;
			}
			if (consumeChar('e', newPtr, bound) || consumeChar('E', newPtr, bound)) {
				if (!consumeChar('-', newPtr, bound)) {
					consumeChar('+', newPtr, bound);
				}
				if (!consumeDigits(newPtr, bound)) [[unlikely]] {
					return nullptr;
				}
			}
			return onlyWhitespace(newPtr, bound) ? iter : nullptr;
		}

		JSONIFIER_INLINE static bool consumeDigits(read_buffer_ptr& newerPtr, read_buffer_ptr bound) noexcept {
			const auto start = newerPtr;
			while (newerPtr < bound && is_digit(static_cast<uint8_t>(*newerPtr))) {
				++newerPtr;
			}
			return newerPtr != start;
		}

		JSONIFIER_INLINE static bool consumeChar(char expected, read_buffer_ptr& newerPtr, read_buffer_ptr bound) noexcept {
			if (newerPtr < bound && *newerPtr == expected) {
				++newerPtr;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static structural_index_ptr validateBool(structural_index_ptr iter, context_type& context) noexcept {
			const auto newPtr = cursor::valuePtr(iter, context);
			if (!jsonifier::internal::validateBool(newPtr, context.stringEnd)) [[unlikely]] {
				return nullptr;
			}
			++iter;
			return onlyWhitespace(newPtr + (*newPtr == 't' ? 4 : 5), scalarBound(iter, context)) ? iter : nullptr;
		}

		template<typename context_type> JSONIFIER_INLINE static structural_index_ptr validateNull(structural_index_ptr iter, context_type& context) noexcept {
			const auto newPtr = cursor::valuePtr(iter, context);
			if (!jsonifier::internal::validateNull(newPtr, context.stringEnd)) [[unlikely]] {
				return nullptr;
			}
			++iter;
			return onlyWhitespace(newPtr + 4, scalarBound(iter, context)) ? iter : nullptr;
		}
	};

}// namespace internal
