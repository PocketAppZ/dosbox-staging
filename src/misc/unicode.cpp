/*
 *  Copyright (C) 2022-2023  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosbox.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "checks.h"
#include "dos_inc.h"
#include "string_utils.h"

CHECK_NARROWING();

class Grapheme final {
public:
	Grapheme() = default;
	Grapheme(const uint16_t code_point);

	[[nodiscard]] bool IsEmpty() const;
	[[nodiscard]] bool IsValid() const;
	[[nodiscard]] bool HasMark() const;
	[[nodiscard]] uint16_t GetCodePoint() const;
	void PushInto(std::vector<uint16_t>& str_out) const;

	void Invalidate();
	void AddMark(const uint16_t code_point);
	void StripMarks();
	void Decompose();

	bool operator==(const Grapheme& other) const;
	bool operator<(const Grapheme& other) const;

private:
	// Unicode code point
	uint16_t code_point = static_cast<uint16_t>(' ');
	// Combining marks
	std::vector<uint16_t> marks        = {};
	std::vector<uint16_t> marks_sorted = {};

	bool is_empty = true;
	bool is_valid = true;
};

// Unicode to DOS code page mapping
using code_page_mapping_t = std::map<Grapheme, uint8_t>;
// DOS code page to Unicode mapping
using code_page_mapping_reverse_t = std::map<uint8_t, Grapheme>;

// ` add description
using decomposition_rules_t = std::map<uint16_t, Grapheme>;

using config_duplicates_t = std::map<uint16_t, uint16_t>;
using config_aliases_t    = std::vector<std::pair<uint16_t, uint16_t>>;

struct ConfigMappingEntry {
	bool valid                          = false;
	code_page_mapping_reverse_t mapping = {};
	uint16_t extends_code_page          = 0;
	std::string extends_dir             = "";
	std::string extends_file            = "";
};

using config_mappings_t = std::map<uint16_t, ConfigMappingEntry>;

static const std::string file_name_main          = "MAIN.TXT";
static const std::string file_name_ascii         = "ASCII.TXT";
static const std::string file_name_decomposition = "DECOMPOSITION.TXT";
static const std::string dir_name_mapping        = "mapping";

// Thresholds for UTF-8 decoding/encoding
constexpr uint8_t decode_threshold_non_ascii = 0b1'000'0000;
constexpr uint8_t decode_threshold_2_bytes   = 0b1'100'0000;
constexpr uint8_t decode_threshold_3_bytes   = 0b1'110'0000;
constexpr uint8_t decode_threshold_4_bytes   = 0b1'111'0000;
constexpr uint8_t decode_threshold_5_bytes   = 0b1'111'1000;
constexpr uint8_t decode_threshold_6_bytes   = 0b1'111'1100;
constexpr uint16_t encode_threshold_2_bytes  = 0x0080;
constexpr uint16_t encode_threshold_3_bytes  = 0x0800;

// Use the character below if there is no sane way to handle the Unicode glyph
constexpr uint8_t unknown_character = 0x3f; // '?'

// End of file marking, use in some files from unicode.org
constexpr uint8_t end_of_file_marking = 0x1a;

// Main information about how to create Unicode mappings for given DOS code page
static config_mappings_t config_mappings = {};

// Unicode -> Unicode fallback mapping (alias),
// use before fallback to 7-bit ASCII
static config_aliases_t config_aliases = {};
// Information about code pages which are exact duplicates
static config_duplicates_t config_duplicates = {};

// Unicode -> 7-bit ASCII mapping, use as a last resort mapping
static code_page_mapping_t mapping_ascii = {};

// Unicode 'KD' decomposition rules
static decomposition_rules_t decomposition_rules = {};

// Concrete Unicode -> codepage mappings
static std::map<uint16_t, code_page_mapping_t> mappings_normalized_by_codepage = {};
static std::map<uint16_t, code_page_mapping_t> mappings_decomposed_by_codepage = {};
// Additional Unicode -> codepage mappings, to avoid unknown characters
static std::map<uint16_t, code_page_mapping_t> aliases_normalized_by_codepage = {};
static std::map<uint16_t, code_page_mapping_t> aliases_decomposed_by_codepage = {};
// Reverse mappings, codepage -> Unicode
static std::map<uint16_t, code_page_mapping_reverse_t> mappings_reverse_by_codepage = {};

// ***************************************************************************
// Grapheme type implementation
// ***************************************************************************

static bool is_combining_mark(const uint32_t code_point)
{
	static constexpr std::pair<uint16_t, uint16_t> ranges[] = {
		{0x0300, 0x036f}, // Combining Diacritical Marks
		{0x0653, 0x065f}, // Arabic Combining Marks
		// Note: Arabic Combining Marks start from 0x064b, but some are
		// present as standalone characters in arabic code pages. To
		// allow this, we do not recognize them as combining marks!
		{0x1ab0, 0x1aff}, // Combining Diacritical Marks Extended
		{0x1dc0, 0x1dff}, // Combining Diacritical Marks Supplement
		{0x20d0, 0x20ff}, // Combining Diacritical Marks for Symbols
		{0xfe20, 0xfe2f}, // Combining Half Marks
	};

	auto in_range = [code_point](const auto& range) {
		return code_point >= range.first && code_point <= range.second;
	};
	return std::any_of(std::begin(ranges), std::end(ranges), in_range);
}

Grapheme::Grapheme(const uint16_t code_point)
        : code_point(code_point),
          is_empty(false)
{
	// It is not valid to have a combining mark
	// as a main code point of the grapheme

	if (is_combining_mark(code_point)) {
		Invalidate();
	}
}

bool Grapheme::IsEmpty() const
{
	return is_empty;
}

bool Grapheme::IsValid() const
{
	return is_valid;
}

bool Grapheme::HasMark() const
{
	return !marks.empty();
}

uint16_t Grapheme::GetCodePoint() const
{
	return code_point;
}

void Grapheme::PushInto(std::vector<uint16_t>& str_out) const
{
	if (is_empty || !is_valid) {
		return;
	}

	str_out.push_back(code_point);
	for (const auto mark : marks) {
		str_out.push_back(mark);
	}
}

void Grapheme::Invalidate()
{
	is_empty = false;
	is_valid = false;

	code_point = unknown_character;
	marks.clear();
	marks_sorted.clear();
}

void Grapheme::AddMark(const uint16_t in_code_point)
{
	if (!is_valid) {
		// Can't add combining mark to invalid grapheme
		return;
	}

	if (!is_combining_mark(in_code_point) || is_empty) {
		// Not a combining mark or empty grapheme
		Invalidate();
		return;
	}

	if (std::find(marks.cbegin(), marks.cend(), in_code_point) != marks.cend()) {
		// Combining mark already present
		return;
	}

	marks.push_back(in_code_point);
	marks_sorted.push_back(in_code_point);
	std::sort(marks_sorted.begin(), marks_sorted.end());
}

void Grapheme::StripMarks()
{
	marks.clear();
	marks_sorted.clear();
}

void Grapheme::Decompose()
{
	if (!is_valid || is_empty) {
		// Can't decompose invalid or empty grapheme
		return;
	}

	while (decomposition_rules.count(code_point) != 0) {
		const auto& rule = decomposition_rules.at(code_point);
		code_point       = rule.code_point;
		for (const auto mark : rule.marks) {
			AddMark(mark);
		}
	}
}

bool Grapheme::operator==(const Grapheme& other) const
{
	return (is_empty == other.is_empty) && (is_valid == other.is_valid) &&
	       (code_point == other.code_point) &&
	       (marks_sorted == other.marks_sorted);
}

bool Grapheme::operator<(const Grapheme& other) const
{
	if (code_point < other.code_point) {
		return true;
	}
	if (code_point > other.code_point) {
		return false;
	}

	if (marks_sorted.empty() && other.marks_sorted.empty()) {
		return false;
	}

	if (marks_sorted.size() < other.marks_sorted.size()) {
		return true;
	}
	if (marks_sorted.size() > other.marks_sorted.size()) {
		return false;
	}

	for (size_t idx = 0; idx < marks_sorted.size(); ++idx) {
		if (marks_sorted[idx] < other.marks_sorted[idx]) {
			return true;
		}
		if (marks_sorted[idx] > other.marks_sorted[idx]) {
			return false;
		}
	}

	assert(is_empty == other.is_empty);
	assert(is_valid == other.is_valid);

	return false;
}

// ***************************************************************************
// Conversion routines
// ***************************************************************************

static bool utf8_to_wide(const std::string& str_in, std::vector<uint16_t>& str_out)
{
	// Convert UTF-8 string to a sequence of decoded integers

	// For UTF-8 encoding explanation see here:
	// -
	// https://www.codeproject.com/Articles/38242/Reading-UTF-8-with-C-streams
	// - https://en.wikipedia.org/wiki/UTF-8#Encoding

	bool status = true;

	str_out.clear();
	str_out.reserve(str_in.size());

	for (size_t i = 0; i < str_in.size(); ++i) {
		const size_t remaining = str_in.size() - i - 1;
		const uint8_t byte_1   = static_cast<uint8_t>(str_in[i]);
		const uint8_t byte_2   = (remaining >= 1)
		                               ? static_cast<uint8_t>(str_in[i + 1]) : 0;
		const uint8_t byte_3   = (remaining >= 2)
		                               ? static_cast<uint8_t>(str_in[i + 2]) : 0;

		auto advance = [&](const size_t bytes) {
			auto counter = std::min(remaining, bytes);
			while (counter--) {
				const auto byte_next = static_cast<uint8_t>(str_in[i + 1]);
				if (byte_next < decode_threshold_non_ascii ||
				    byte_next >= decode_threshold_2_bytes) {
					break;
				}
				++i;
			}

			status = false; // advancing without decoding
		};

		// Retrieve code point
		uint32_t code_point = unknown_character;

		// Support code point needing up to 3 bytes to encode; this
		// includes Latin, Greek, Cyrillic, Hebrew, Arabic, VGA charset
		// symbols, etc. More bytes are needed mainly for historic
		// scripts, emoji, etc.

		if (GCC_UNLIKELY(byte_1 >= decode_threshold_6_bytes)) {
			// 6-byte code point (>= 31 bits), no support
			advance(5);
		} else if (GCC_UNLIKELY(byte_1 >= decode_threshold_5_bytes)) {
			// 5-byte code point (>= 26 bits), no support
			advance(4);
		} else if (GCC_UNLIKELY(byte_1 >= decode_threshold_4_bytes)) {
			// 4-byte code point (>= 21 bits), no support
			advance(3);
		} else if (GCC_UNLIKELY(byte_1 >= decode_threshold_3_bytes)) {
			// 3-byte code point - decode 1st byte
			code_point = static_cast<uint8_t>(
			        byte_1 - decode_threshold_3_bytes);
			// Decode 2nd byte
			code_point = code_point << 6;
			if (byte_2 >= decode_threshold_non_ascii &&
			    byte_2 < decode_threshold_2_bytes) {
				++i;
				code_point = code_point + byte_2 -
				             decode_threshold_non_ascii;
			} else {
				status = false; // code point encoding too short
			}
			// Decode 3rd byte
			code_point = code_point << 6;
			if (byte_2 >= decode_threshold_non_ascii &&
			    byte_2 < decode_threshold_2_bytes &&
			    byte_3 >= decode_threshold_non_ascii &&
			    byte_3 < decode_threshold_2_bytes) {
				++i;
				code_point = code_point + byte_3 -
				             decode_threshold_non_ascii;
			} else {
				status = false; // code point encoding too short
			}
		} else if (GCC_UNLIKELY(byte_1 >= decode_threshold_2_bytes)) {
			// 2-byte code point - decode 1st byte
			code_point = static_cast<uint8_t>(
			        byte_1 - decode_threshold_2_bytes);
			// Decode 2nd byte
			code_point = code_point << 6;
			if (byte_2 >= decode_threshold_non_ascii &&
			    byte_2 < decode_threshold_2_bytes) {
				++i;
				code_point = code_point + byte_2 -
				             decode_threshold_non_ascii;
			} else {
				status = false; // code point encoding too short
			}
		} else if (byte_1 < decode_threshold_non_ascii) {
			// 1-byte code point, ASCII compatible
			code_point = byte_1;
		} else {
			status = false; // not UTF8 encoding
		}

		str_out.push_back(static_cast<uint16_t>(code_point));
	}

	return status;
}

static void wide_to_utf8(const std::vector<uint16_t>& str_in, std::string& str_out)
{
	str_out.clear();
	str_out.reserve(str_in.size() * 2);

	auto push = [&](const int value) {
		const auto byte = static_cast<uint8_t>(value);
		str_out.push_back(static_cast<char>(byte));
	};

	for (const auto code_point : str_in) {
		if (code_point < encode_threshold_2_bytes) {
			// Encode using 1 byte
			push(code_point);
		} else if (code_point < encode_threshold_3_bytes) {
			// Encode using 2 bytes
			const auto to_byte_1 = code_point >> 6;
			const auto to_byte_2 = 0b0'011'1111 & code_point;
			push(to_byte_1 | 0b1'100'0000);
			push(to_byte_2 | 0b1'000'0000);
		} else {
			// Encode using 3 bytes
			const auto to_byte_1 = code_point >> 12;
			const auto to_byte_2 = 0b0'011'1111 & (code_point >> 6);
			const auto to_byte_3 = 0b0'011'1111 & code_point;
			push(to_byte_1 | 0b1'110'0000);
			push(to_byte_2 | 0b1'000'0000);
			push(to_byte_3 | 0b1'000'0000);
		}
	}

	str_out.shrink_to_fit();
}

static void warn_code_point(const uint16_t code_point)
{
	static std::set<uint16_t> already_warned;
	if (already_warned.count(code_point)) {
		return;
	}
	already_warned.insert(code_point);
	LOG_WARNING("UNICODE: No fallback mapping for code point 0x%04x", code_point);
}

static void warn_code_page(const uint16_t code_page)
{
	static std::set<uint16_t> already_warned;
	if (already_warned.count(code_page)) {
		return;
	}
	already_warned.insert(code_page);
	LOG_WARNING("UNICODE: Requested unknown code page %d", code_page);
}

static void warn_default_code_page()
{
	static bool already_warned = false;
	if (already_warned) {
		return;
	}
	already_warned = true;
	LOG_WARNING("UNICODE: Unable to prepare default code page");
}

static bool wide_to_dos(const std::vector<uint16_t>& str_in,
                        std::string& str_out, const uint16_t code_page)
{
	bool status = true;
	str_out.clear();
	str_out.reserve(str_in.size());

	code_page_mapping_t* mapping_normalized = nullptr;
	code_page_mapping_t* mapping_decomposed = nullptr;
	code_page_mapping_t* aliases_normalized = nullptr;
	code_page_mapping_t* aliases_decomposed = nullptr;

	// Try to find UTF8 -> code page mapping
	if (code_page != 0) {
		const auto it_normalized = mappings_normalized_by_codepage.find(code_page);
		if (it_normalized != mappings_normalized_by_codepage.end()) {
			mapping_normalized = &it_normalized->second;
		} else {
			warn_code_page(code_page);
		}

		const auto it_decomposed = mappings_decomposed_by_codepage.find(code_page);
		if (it_decomposed != mappings_decomposed_by_codepage.end()) {
			mapping_decomposed = &it_decomposed->second;
		}

		const auto it_alias_normalized = aliases_normalized_by_codepage.find(code_page);
		if (it_alias_normalized != aliases_normalized_by_codepage.end()) {
			aliases_normalized = &it_alias_normalized->second;
		}

		const auto it_alias_decomposed = aliases_decomposed_by_codepage.find(code_page);
		if (it_alias_decomposed != aliases_decomposed_by_codepage.end()) {
			aliases_decomposed = &it_alias_decomposed->second;
		}
	}

	// Handle code points which are 7-bit ASCII characters
	auto push_7bit = [&str_out](const Grapheme& grapheme) {
		if (grapheme.HasMark()) {
			return false; // not a 7-bit ASCII character
		}

		const auto code_point = grapheme.GetCodePoint();
		if (code_point >= decode_threshold_non_ascii) {
			return false; // not a 7-bit ASCII character
		}

		str_out.push_back(static_cast<char>(code_point));
		return true;
	};

	// Handle code points belonging to selected code page
	auto push_code_page = [&str_out](const code_page_mapping_t* mapping,
	                                 const Grapheme& grapheme) {
		if (!mapping) {
			return false;
		}
		const auto it = mapping->find(grapheme);
		if (it == mapping->end()) {
			return false;
		}

		str_out.push_back(static_cast<char>(it->second));
		return true;
	};

	// Handle code points which can only be mapped to ASCII
	// using a fallback Unicode mapping table
	auto push_fallback = [&str_out](const Grapheme& grapheme) {
		if (grapheme.HasMark()) {
			return false;
		}

		const auto it = mapping_ascii.find(grapheme.GetCodePoint());
		if (it == mapping_ascii.end()) {
			return false;
		}

		str_out.push_back(static_cast<char>(it->second));
		return true;
	};

	// Handle unknown code points
	auto push_unknown = [&str_out, &status](const uint16_t code_point) {
		str_out.push_back(static_cast<char>(unknown_character));
		warn_code_point(code_point);
		status = false;
	};

	// Helper for handling normalized graphemes
	auto push_normalized = [&](const Grapheme& grapheme) {
		return push_7bit(grapheme) ||
		       push_code_page(mapping_normalized, grapheme) ||
		       push_code_page(aliases_normalized, grapheme) ||
		       push_fallback(grapheme);
	};

	// Helper for handling non-normalized graphemes
	auto push_decomposed = [&](const Grapheme& grapheme) {
		Grapheme decomposed = grapheme;
		decomposed.Decompose();

		return push_code_page(mapping_decomposed, decomposed) ||
		       push_code_page(aliases_decomposed, decomposed);
	};

	for (size_t i = 0; i < str_in.size(); ++i) {
		Grapheme grapheme(str_in[i]);
		while (i + 1 < str_in.size() && is_combining_mark(str_in[i + 1])) {
			++i;
			grapheme.AddMark(str_in[i]);
		}

		// Try to push matching character
		if (push_normalized(grapheme) || push_decomposed(grapheme)) {
			continue;
		}

		// Last, desperate attempt: decompose and strip marks
		const auto original_code_point = grapheme.GetCodePoint();
		grapheme.Decompose();
		if (grapheme.HasMark()) {
			grapheme.StripMarks();
			if (push_normalized(grapheme)) {
				continue;
			}
		}

		// We are unable to match this grapheme at all
		push_unknown(original_code_point);
	}

	str_out.shrink_to_fit();
	return status;
}

static void dos_to_wide(const std::string& str_in,
                        std::vector<uint16_t>& str_out, const uint16_t code_page)
{
	// Unicode code points for screen codes from 0x00 to 0x1f
	// see: https://en.wikipedia.org/wiki/Code_page_437
	constexpr uint16_t codes[0x20] = {
	        0x0020, 0x263a, 0x263b, 0x2665, // 00-03
	        0x2666, 0x2663, 0x2660, 0x2022, // 04-07
	        0x25d8, 0x25cb, 0x25d9, 0x2642, // 08-0b
	        0x2640, 0x266a, 0x266b, 0x263c, // 0c-0f
	        0x25ba, 0x25c4, 0x2195, 0x203c, // 10-13
	        0x00b6, 0x00a7, 0x25ac, 0x21a8, // 14-17
	        0x2191, 0x2193, 0x2192, 0x2190, // 18-1b
	        0x221f, 0x2194, 0x25b2, 0x25bc, // 1c-1f
	};

	constexpr uint16_t codepoint_7f = 0x2302;

	str_out.clear();
	str_out.reserve(str_in.size());

	for (const auto character : str_in) {
		const auto byte = static_cast<uint8_t>(character);
		if (GCC_UNLIKELY(byte >= decode_threshold_non_ascii)) {
			// Character above 0x07f - take from code page mapping

			if (!mappings_reverse_by_codepage.count(code_page) ||
			    !mappings_reverse_by_codepage[code_page].count(byte)) {
				str_out.push_back(unknown_character);
			} else {
				(mappings_reverse_by_codepage[code_page])[byte].PushInto(str_out);
			}

		} else {
			if (GCC_UNLIKELY(byte == 0x7f)) {
				str_out.push_back(codepoint_7f);
			} else if (byte >= 0x20) {
				str_out.push_back(byte);
			} else {
				str_out.push_back(codes[byte]);
			}
		}
	}
}

// ***************************************************************************
// Read resources from files
// ***************************************************************************

static bool prepare_code_page(const uint16_t code_page);

template <typename T1, typename T2>
bool add_if_not_mapped(std::map<T1, T2>& mapping, T1 first, T2 second)
{
	[[maybe_unused]] const auto& [item, was_added] =
		mapping.try_emplace(first, second);

	return was_added;
}

static std::ifstream open_mapping_file(const std_fs::path& path_root,
                                       const std::string& file_name)
{
	const std_fs::path file_path = path_root / file_name;
	std::ifstream in_file(file_path.string());
	if (!in_file) {
		LOG_ERR("UNICODE: Could not open mapping file %s",
		        file_name.c_str());
	}

	return in_file;
}

static bool get_line(std::ifstream& in_file, std::string& line_str, size_t& line_num)
{
	line_str.clear();

	while (line_str.empty()) {
		if (!std::getline(in_file, line_str)) {
			return false;
		}
		if (line_str.length() >= 1 && line_str[0] == end_of_file_marking) {
			return false; // end of definitions
		}
		++line_num;
	}

	return true;
}

static bool get_tokens(const std::string& line, std::vector<std::string>& tokens)
{
	// Split the line into tokens, strip away comments

	bool token_started = false;
	size_t start_pos   = 0;

	tokens.clear();
	for (size_t i = 0; i < line.size(); ++i) {
		if (line[i] == '#') {
			break; // comment started
		}

		const bool is_space = (line[i] == ' ') || (line[i] == '\t') ||
		                      (line[i] == '\r') || (line[i] == '\n');

		if (!token_started && !is_space) {
			// We are at first character of the token
			token_started = true;
			start_pos     = i;
			continue;
		}

		if (token_started && is_space) {
			// We are at the first character after the token
			token_started = false;
			tokens.emplace_back(line.substr(start_pos, i - start_pos));
			continue;
		}
	}

	if (token_started) {
		// We have a token which ends with the end of the line
		tokens.emplace_back(line.substr(start_pos));
	}

	return !tokens.empty();
}

static bool get_hex_8bit(const std::string& token, uint8_t& out)
{
	if (token.size() != 4 || token[0] != '0' || token[1] != 'x' ||
	    !isxdigit(token[2]) || !isxdigit(token[3])) {
		return false;
	}

	out = static_cast<uint8_t>(strtoul(token.c_str() + 2, nullptr, 16));
	return true;
}

static bool get_hex_16bit(const std::string& token, uint16_t& out)
{
	if (token.size() != 6 || token[0] != '0' || token[1] != 'x' ||
	    !isxdigit(token[2]) || !isxdigit(token[3]) || !isxdigit(token[4]) ||
	    !isxdigit(token[5])) {
		return false;
	}

	out = static_cast<uint16_t>(strtoul(token.c_str() + 2, nullptr, 16));
	return true;
}

static bool get_ascii(const std::string& token, uint8_t& out)
{
	if (token.length() == 1) {
		out = static_cast<uint8_t>(token[0]);
	} else if (GCC_UNLIKELY(token == "SPC")) {
		out = ' ';
	} else if (GCC_UNLIKELY(token == "HSH")) {
		out = '#';
	} else if (token == "NNN") {
		out = unknown_character;
	} else {
		return false;
	}

	return true;
}

static bool get_code_page(const std::string& token, uint16_t& code_page)
{
	if (GCC_UNLIKELY(token.empty() || token.length() > 5)) {
		return false;
	}

	for (const auto character : token) {
		if (GCC_UNLIKELY(character < '0' || character > '9')) {
			return false;
		}
	}

	const auto tmp = std::atoi(token.c_str());
	if (GCC_UNLIKELY(tmp < 1 || tmp > UINT16_MAX)) {
		return false;
	}

	code_page = static_cast<uint16_t>(tmp);
	return true;
}

static bool get_grapheme(const std::vector<std::string>& tokens, Grapheme& grapheme)
{
	uint16_t code_point = 0;
	if (tokens.size() < 2 || !get_hex_16bit(tokens[1], code_point)) {
		return false;
	}

	Grapheme new_grapheme(code_point);

	if (tokens.size() >= 3) {
		if (!get_hex_16bit(tokens[2], code_point)) {
			return false;
		}
		new_grapheme.AddMark(code_point);
	}

	if (tokens.size() >= 4) {
		if (!get_hex_16bit(tokens[3], code_point)) {
			return false;
		}
		new_grapheme.AddMark(code_point);
	}

	grapheme = new_grapheme;
	return true;
}

static void error_parsing(const std::string& file_name, const size_t line_num,
                          const std::string& details = "")
{
	if (details.empty()) {
		LOG_ERR("UNICODE: Error parsing mapping file %s, line %d",
		        file_name.c_str(),
		        static_cast<int>(line_num));
	} else {
		LOG_ERR("UNICODE: Error parsing mapping file %s, line %d: %s",
		        file_name.c_str(),
		        static_cast<int>(line_num),
		        details.c_str());
	}
}

static void error_not_combining_mark(const size_t position,
                                     const std::string& file_name,
                                     const size_t line_num)
{
	const auto details = std::string("token #") + std::to_string(position) +
	                     " is not a supported combining mark";
	error_parsing(file_name, line_num, details);
}

static void error_code_page_invalid(const std::string& file_name, const size_t line_num)
{
	error_parsing(file_name, line_num, "invalid code page number");
}

static void error_code_page_defined(const std::string& file_name, const size_t line_num)
{
	error_parsing(file_name, line_num, "code page already defined");
}

static void error_code_page_none(const std::string& file_name, const size_t line_num)
{
	error_parsing(file_name, line_num, "not currently defining a code page");
}

static bool check_import_status(const std::ifstream& in_file,
                                const std::string& file_name, const bool empty)
{
	if (in_file.fail() && !in_file.eof()) {
		LOG_ERR("UNICODE: Error reading mapping file %s", file_name.c_str());
		return false;
	}

	if (empty) {
		LOG_ERR("UNICODE: Mapping file %s has no entries",
		        file_name.c_str());
		return false;
	}

	return true;
}

static bool check_grapheme_valid(const Grapheme& grapheme,
                                 const std::string& file_name, const size_t line_num)
{
	if (grapheme.IsValid()) {
		return true;
	}

	LOG_ERR("UNICODE: Error, invalid grapheme defined in file %s, line %d",
	        file_name.c_str(),
	        static_cast<int>(line_num));
	return false;
}

static bool import_mapping_code_page(const std_fs::path& path_root,
                                     const std::string& file_name,
                                     code_page_mapping_reverse_t& mapping)
{
	// Import code page character -> UTF-8 mapping from external file

	// Open the file
	auto in_file = open_mapping_file(path_root, file_name);
	if (!in_file) {
		LOG_ERR("UNICODE: Error opening mapping file %s", file_name.c_str());
		return false;
	}

	// Read and parse
	std::string line_str = " ";
	size_t line_num      = 0;

	code_page_mapping_reverse_t new_mapping;

	while (get_line(in_file, line_str, line_num)) {
		std::vector<std::string> tokens;
		if (!get_tokens(line_str, tokens)) {
			continue; // empty line
		}

		uint8_t character_code = 0;
		if (!get_hex_8bit(tokens[0], character_code)) {
			error_parsing(file_name, line_num);
			return false;
		}

		if (GCC_UNLIKELY(tokens.size() == 1)) {
			// Handle undefined character entry, ignore 7-bit ASCII
			// codes
			if (character_code >= decode_threshold_non_ascii) {
				Grapheme grapheme;
				add_if_not_mapped(new_mapping, character_code, grapheme);
			}

		} else if (tokens.size() <= 4) {
			// Handle mapping entry, ignore 7-bit ASCII codes
			if (character_code >= decode_threshold_non_ascii) {
				Grapheme grapheme;
				if (!get_grapheme(tokens, grapheme)) {
					error_parsing(file_name, line_num);
					return false;
				}

				// Invalid grapheme that is not added
				// (overridden) is OK here; at least CP 1258
				// definition from Unicode.org contains mapping
				// of code page characters to combining marks,
				// which is fine for converting texts, but a
				// no-no for DOS emulation (where the number of
				// output characters has to match the number of
				// input characters). For such code page
				// definitions, just override problematic mappings
				// in the main mapping configuration file.
				if (add_if_not_mapped(new_mapping, character_code, grapheme) &&
				    !check_grapheme_valid(grapheme, file_name, line_num)) {
					return false;
				}
			}
		} else {
			error_parsing(file_name, line_num);
			return false;
		}
	}

	if (!check_import_status(in_file, file_name, new_mapping.empty())) {
		return false;
	}

	// Reading/parsing succeeded - use all the data read from file
	mapping = new_mapping;
	return true;
}

static void import_config_main(const std_fs::path& path_root)
{
	// Import main configuration file, telling how to construct UTF-8
	// mappings for each and every supported code page

	// Open the file
	auto in_file = open_mapping_file(path_root, file_name_main);
	if (!in_file)
		return;

	// Read and parse
	bool file_empty      = true;
	std::string line_str = " ";
	size_t line_num      = 0;

	uint16_t curent_code_page = 0;

	config_mappings_t new_config_mappings;
	config_duplicates_t new_config_duplicates;
	config_aliases_t new_config_aliases;

	while (get_line(in_file, line_str, line_num)) {
		std::vector<std::string> tokens;
		if (!get_tokens(line_str, tokens))
			continue; // empty line
		uint8_t character_code = 0;

		if (GCC_UNLIKELY(tokens[0] == "ALIAS")) {
			if ((tokens.size() != 3 && tokens.size() != 4) ||
			    (tokens.size() == 4 && tokens[3] != "BIDIRECTIONAL")) {
				error_parsing(file_name_main, line_num);
				return;
			}

			uint16_t code_point_1 = 0;
			uint16_t code_point_2 = 0;
			if (!get_hex_16bit(tokens[1], code_point_1) ||
			    !get_hex_16bit(tokens[2], code_point_2)) {
				error_parsing(file_name_main, line_num);
				return;
			}

			new_config_aliases.emplace_back(
			        std::make_pair(code_point_1, code_point_2));

			if (tokens.size() == 4) // check if bidirectional
				new_config_aliases.emplace_back(
				        std::make_pair(code_point_2, code_point_1));

			curent_code_page = 0;

		} else if (GCC_UNLIKELY(tokens[0] == "CODEPAGE")) {
			auto check_no_code_page = [&](const uint16_t code_page) {
				if ((new_config_mappings.find(code_page) != new_config_mappings.end() && new_config_mappings[code_page].valid) ||
				    new_config_duplicates.find(code_page) != new_config_duplicates.end()) {
					error_code_page_defined(file_name_main, line_num);
					return false;
				}
				return true;
			};

			if (tokens.size() == 4 && tokens[2] == "DUPLICATES") {
				uint16_t code_page_1 = 0;
				uint16_t code_page_2 = 0;
				if (!get_code_page(tokens[1], code_page_1) ||
				    !get_code_page(tokens[3], code_page_2)) {
					error_code_page_invalid(file_name_main,
					                        line_num);
					return;
				}

				// Make sure code page definition does not exist yet
				if (!check_no_code_page(code_page_1)) {
					return;
				}

				new_config_duplicates[code_page_1] = code_page_2;
				curent_code_page = 0;

			} else {
				uint16_t code_page = 0;
				if (tokens.size() != 2 ||
				    !get_code_page(tokens[1], code_page)) {
					error_code_page_invalid(file_name_main,
					                        line_num);
					return;
				}

				// Make sure code page definition does not exist yet
				if (!check_no_code_page(code_page)) {
					return;
				}

				new_config_mappings[code_page].valid = true;
				curent_code_page = code_page;
			}

		} else if (GCC_UNLIKELY(tokens[0] == "EXTENDS")) {
			if (!curent_code_page) {
				error_code_page_none(file_name_main, line_num);
				return;
			}

			if (tokens.size() == 3 && tokens[1] == "CODEPAGE") {
				uint16_t code_page = 0;
				if (!get_code_page(tokens[2], code_page)) {
					error_code_page_invalid(file_name_main,
					                        line_num);
					return;
				}

				new_config_mappings[curent_code_page].extends_code_page = code_page;
			} else if (tokens.size() == 4 && tokens[1] == "FILE") {
				new_config_mappings[curent_code_page].extends_dir =
				        tokens[2];
				new_config_mappings[curent_code_page].extends_file =
				        tokens[3];
				// some meaningful mapping provided
				file_empty = false;
			} else {
				error_parsing(file_name_main, line_num);
				return;
			}

			curent_code_page = 0;

		} else if (get_hex_8bit(tokens[0], character_code)) {
			if (!curent_code_page) {
				error_code_page_none(file_name_main, line_num);
				return;
			}

			auto& new_mapping =
			        new_config_mappings[curent_code_page].mapping;

			if (tokens.size() == 1) {
				// Handle undefined character entry
				if (character_code >= decode_threshold_non_ascii) {
					// ignore 7-bit ASCII codes
					Grapheme grapheme; // placeholder
					add_if_not_mapped(new_mapping,
					                  character_code,
					                  grapheme);
					file_empty = false; // some meaningful
					                    // mapping provided
				}

			} else if (tokens.size() <= 4) {
				// Handle mapping entry
				if (character_code >= decode_threshold_non_ascii) {
					// ignore 7-bit ASCII codes
					Grapheme grapheme; // placeholder
					if (!get_grapheme(tokens, grapheme)) {
						error_parsing(file_name_main,
						              line_num);
						return;
					}

					if (!check_grapheme_valid(grapheme,
					                          file_name_main,
					                          line_num))
						return;

					add_if_not_mapped(new_mapping,
					                  character_code,
					                  grapheme);
					// some meaningful mapping provided
					file_empty = false;
				}

			} else {
				error_parsing(file_name_main, line_num);
				return;
			}

		} else {
			error_parsing(file_name_main, line_num);
			return;
		}
	}

	if (!check_import_status(in_file, file_name_main, file_empty)) {
		return;
	}

	// Reading/parsing succeeded - use all the data read from file
	config_mappings   = new_config_mappings;
	config_duplicates = new_config_duplicates;
	config_aliases    = new_config_aliases;
}

static void import_decomposition(const std_fs::path& path_root)
{
	// Import Unicode decomposition rules; will be used to handle
	// non-normalized Unicode input

	// Open the file
	auto in_file = open_mapping_file(path_root, file_name_decomposition);
	if (!in_file) {
		return;
	}

	// Read and parse
	std::string line_str = "";
	size_t line_num      = 0;

	decomposition_rules_t new_rules;

	while (get_line(in_file, line_str, line_num)) {
		std::vector<std::string> tokens;
		if (!get_tokens(line_str, tokens)) {
			continue; // empty line
		}

		uint16_t code_point_1 = 0;
		uint16_t code_point_2 = 0;

		if (tokens.size() < 3 || !get_hex_16bit(tokens[0], code_point_1) ||
		    !get_hex_16bit(tokens[1], code_point_2)) {
			error_parsing(file_name_decomposition, line_num);
			return;
		}

		new_rules[code_point_1] = code_point_2;
		for (size_t idx = 2; idx < tokens.size(); ++idx) {
			if (!get_hex_16bit(tokens[idx], code_point_2)) {
				error_parsing(file_name_decomposition, line_num);
				return;
			}
			if (!is_combining_mark(code_point_2)) {
				error_not_combining_mark(idx + 1,
				                         file_name_decomposition,
				                         line_num);
				return;
			}
			new_rules[code_point_1].AddMark(code_point_2);
		}
	}

	if (!check_import_status(in_file, file_name_decomposition, new_rules.empty())) {
		return;
	}

	// Reading/parsing succeeded - use the rules
	decomposition_rules = new_rules;
}

static void import_mapping_ascii(const std_fs::path& path_root)
{
	// Import fallback mapping, from Unicode to 7-bit ASCII;
	// this mapping will only be used if everything else fails

	// Open the file
	auto in_file = open_mapping_file(path_root, file_name_ascii);
	if (!in_file) {
		return;
	}

	// Read and parse
	std::string line_str = "";
	size_t line_num      = 0;

	code_page_mapping_t new_mapping_ascii;

	while (get_line(in_file, line_str, line_num)) {
		std::vector<std::string> tokens;
		if (!get_tokens(line_str, tokens)) {
			continue; // empty line
		}

		uint16_t code_point = 0;
		uint8_t character   = 0;

		if (tokens.size() != 2 || !get_hex_16bit(tokens[0], code_point) ||
		    !get_ascii(tokens[1], character)) {
			error_parsing(file_name_ascii, line_num);
			return;
		}

		new_mapping_ascii[code_point] = character;
	}

	if (!check_import_status(in_file, file_name_ascii, new_mapping_ascii.empty())) {
		return;
	}

	// Reading/parsing succeeded - use the mapping
	mapping_ascii = new_mapping_ascii;
}

static uint16_t deduplicate_code_page(const uint16_t code_page)
{
	const auto it = config_duplicates.find(code_page);

	if (it == config_duplicates.end()) {
		return code_page;
	}

	return it->second;
}

static void construct_decomposed(const code_page_mapping_t& normalized,
                                 code_page_mapping_t& decomposed)
{
	decomposed.clear();
	for (const auto& [grapheme, character_code] : normalized) {
		auto tmp = grapheme;
		tmp.Decompose();
		if (grapheme == tmp) {
			continue;
		}

		decomposed[tmp] = character_code;
	}
}

static bool construct_mapping(const uint16_t code_page)
{
	// Prevent processing if previous attempt failed;
	// also protect against circular dependencies

	static std::set<uint16_t> already_tried;
	if (already_tried.count(code_page)) {
		return false;
	}
	already_tried.insert(code_page);

	assert(config_mappings.count(code_page));
	assert(!mappings_normalized_by_codepage.count(code_page));
	assert(!mappings_decomposed_by_codepage.count(code_page));
	assert(!mappings_reverse_by_codepage.count(code_page));

	// First apply mapping found in main config file

	const auto& config_mapping      = config_mappings[code_page];
	code_page_mapping_t new_mapping = {};
	code_page_mapping_reverse_t new_mapping_reverse = {};

	auto add_to_mappings = [&](const uint8_t first, const Grapheme& second) {
		if (first < decode_threshold_non_ascii) {
			return;
		}
		if (!add_if_not_mapped(new_mapping_reverse, first, second)) {
			return;
		}
		if (second.IsEmpty() || !second.IsValid()) {
			return;
		}
		if (add_if_not_mapped(new_mapping, second, first)) {
			return;
		}

		LOG_WARNING("UNICODE: Mapping for code page %d uses a code point twice; character 0x%02x",
		            code_page,
		            first);
	};

	for (const auto& entry : config_mapping.mapping) {
		add_to_mappings(entry.first, entry.second);
	}

	// If code page is expansion of other code page, copy remaining entries

	if (config_mapping.extends_code_page) {
		const uint16_t dependency = deduplicate_code_page(config_mapping.extends_code_page);
		if (!prepare_code_page(dependency)) {
			LOG_ERR("UNICODE: Code page %d mapping requires code page %d mapping",
			        code_page,
			        dependency);
			return false;
		}

		for (const auto& entry : mappings_normalized_by_codepage[dependency]) {
			add_to_mappings(entry.second, entry.first);
		}
	}

	// If code page uses external mapping file, load appropriate entries

	if (!config_mapping.extends_file.empty()) {
		code_page_mapping_reverse_t mapping_file;

		if (!import_mapping_code_page(GetResourcePath(config_mapping.extends_dir),
		                              config_mapping.extends_file,
		                              mapping_file)) {
			return false;
		}

		for (const auto& entry : mapping_file) {
			add_to_mappings(entry.first, entry.second);
		}
	}

	mappings_normalized_by_codepage[code_page] = new_mapping;
	mappings_reverse_by_codepage[code_page]    = new_mapping_reverse;

	// Construct decomposed mapping
	construct_decomposed(mappings_normalized_by_codepage[code_page],
	                     mappings_decomposed_by_codepage[code_page]);

	return true;
}

static void construct_aliases(const uint16_t code_page)
{
	assert(!aliases_normalized_by_codepage.count(code_page));
	assert(!aliases_decomposed_by_codepage.count(code_page));
	assert(mappings_normalized_by_codepage.count(code_page));

	const auto& mapping      = mappings_normalized_by_codepage[code_page];
	auto& aliases_normalized = aliases_normalized_by_codepage[code_page];

	for (const auto& alias : config_aliases) {
		if (!mapping.count(alias.first) && mapping.count(alias.second) &&
		    !aliases_normalized.count(alias.first)) {
			aliases_normalized[alias.first] =
			        mapping.find(alias.second)->second;
		}
	}

	// Construct decomposed aliases
	construct_decomposed(aliases_normalized_by_codepage[code_page],
	                     aliases_decomposed_by_codepage[code_page]);
}

static bool prepare_code_page(const uint16_t code_page)
{
	if (mappings_normalized_by_codepage.count(code_page)) {
		return true; // code page already prepared
	}

	if (!config_mappings.count(code_page) || !construct_mapping(code_page)) {
		// Unsupported code page or error
		mappings_normalized_by_codepage.erase(code_page);
		mappings_decomposed_by_codepage.erase(code_page);
		mappings_reverse_by_codepage.erase(code_page);
		return false;
	}

	construct_aliases(code_page);
	return true;
}

static void load_config_if_needed()
{
	// If this is the first time we are requested to prepare the code page,
	// load the top-level configuration and fallback 7-bit ASCII mapping

	static bool config_loaded = false;
	if (!config_loaded) {
		const auto path_root = GetResourcePath(dir_name_mapping);
		import_decomposition(path_root);
		import_mapping_ascii(path_root);
		import_config_main(path_root);
		config_loaded = true;
	}
}

static uint16_t get_default_code_page()
{
	constexpr uint16_t default_code_page = 437; // United States

	if (!prepare_code_page(default_code_page)) {
		warn_default_code_page();
		return 0;
	}

	return default_code_page;
}

static uint16_t get_custom_code_page(const uint16_t custom_code_page)
{
	if (custom_code_page == 0) {
		return 0;
	}

	const uint16_t code_page = deduplicate_code_page(custom_code_page);
	if (!prepare_code_page(code_page)) {
		return get_default_code_page();
	}

	return code_page;
}

// ***************************************************************************
// External interface
// ***************************************************************************

uint16_t get_utf8_code_page()
{
	if (!IS_EGAVGA_ARCH) {
		// Below EGA it wasn't possible to change the character set
		return get_default_code_page();
	}

	load_config_if_needed();
	const uint16_t code_page = deduplicate_code_page(dos.loaded_codepage);

	// For unsupported code pages revert to default one
	if (prepare_code_page(code_page)) {
		return code_page;
	}

	return get_default_code_page();
}

bool utf8_to_dos(const std::string& str_in, std::string& str_out)
{
	load_config_if_needed();

	const uint16_t cp = get_utf8_code_page();

	std::vector<uint16_t> str_wide = {};

	const bool status1 = utf8_to_wide(str_in, str_wide);
	const bool status2 = wide_to_dos(str_wide, str_out, cp);

	return status1 && status2;
}

bool utf8_to_dos(const std::string& str_in, std::string& str_out,
                 const uint16_t code_page)
{
	load_config_if_needed();

	const uint16_t cp = get_custom_code_page(code_page);

	std::vector<uint16_t> str_wide = {};

	const bool status1 = utf8_to_wide(str_in, str_wide);
	const bool status2 = wide_to_dos(str_wide, str_out, cp);

	return status1 && status2;
}

void dos_to_utf8(const std::string& str_in, std::string& str_out)
{
	load_config_if_needed();

	const uint16_t cp = get_utf8_code_page();

	std::vector<uint16_t> str_wide = {};

	dos_to_wide(str_in, str_wide, cp);
	wide_to_utf8(str_wide, str_out);
}

void dos_to_utf8(const std::string& str_in, std::string& str_out,
                 const uint16_t code_page)
{
	load_config_if_needed();

	const uint16_t cp = get_custom_code_page(code_page);

	std::vector<uint16_t> str_wide = {};

	dos_to_wide(str_in, str_wide, cp);
	wide_to_utf8(str_wide, str_out);
}
