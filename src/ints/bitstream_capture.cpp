// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bitstream_capture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

#include "utils/checks.h"

CHECK_NARROWING();

namespace {

constexpr size_t DiskInfoOffset    = 0x100;
constexpr size_t SkewTableOffset   = 0x105;
constexpr size_t BlockTableOffset  = 0x305;
constexpr size_t LengthTableOffset = 0x505;
constexpr size_t FlagsTableOffset  = 0x705;
constexpr size_t HeaderBytes       = 0x905;
constexpr size_t MaxCylinders      = 128;
constexpr size_t BlockBytes        = 256;
constexpr size_t SkewTicksPerTurn  = 40'000;
constexpr auto NoSkew              = 0x1111;

enum class TransCopyType : uint8_t {
	MfmHighDensity      = 0x02,
	MfmDoubleDensity360 = 0x03,
	FmSingleDensity     = 0x05,
	MfmDoubleDensity    = 0x07,
	AtariFm             = 0x0c,
	Unknown             = 0xff,
};

bool is_pc_transcopy_type(const uint8_t type)
{
	switch (static_cast<TransCopyType>(type)) {
	case TransCopyType::MfmHighDensity:
	case TransCopyType::MfmDoubleDensity360:
	case TransCopyType::FmSingleDensity:
	case TransCopyType::MfmDoubleDensity:
	case TransCopyType::AtariFm:
	case TransCopyType::Unknown: return true;
	default: return false;
	}
}

int transcopy_rate_kbps(const uint8_t type, const size_t length)
{
	switch (static_cast<TransCopyType>(type)) {
	case TransCopyType::MfmHighDensity:
	case TransCopyType::MfmDoubleDensity360: return 500;
	case TransCopyType::FmSingleDensity:
	case TransCopyType::AtariFm:
		// FM captures can use the native single-density clock or
		// sample the same transitions at the double-density clock.
		return length < 8000 ? 125 : 250;
	case TransCopyType::MfmDoubleDensity: return 250;
	case TransCopyType::Unknown: return length >= 18'000 ? 500 : 250;
	default: return 0;
	}
}

uint16_t read_le16(const std::span<const uint8_t> bytes, const size_t offset)
{
	return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

size_t read_track_offset(const std::span<const uint8_t> bytes, const size_t entry)
{
	const auto offset = BlockTableOffset + 2 * entry;
	return static_cast<size_t>((bytes[offset] << 8) | bytes[offset + 1]) *
	       BlockBytes;
}

uint32_t capture_read_le(const std::span<const uint8_t> bytes)
{
	uint32_t value = 0;
	for (size_t i = 0; i < bytes.size(); ++i) {
		value |= uint32_t{bytes[i]} << (8 * i);
	}
	return value;
}

} // namespace

std::optional<BitstreamCapture> ParseTransCopyCapture(const std::span<const uint8_t> bytes,
                                                      std::string& error)
{
	error.clear();
	const auto fail = [&](const char* message) -> std::optional<BitstreamCapture> {
		error = message;
		return {};
	};
	if (bytes.size() < HeaderBytes || bytes.size() > 0x1010000 ||
	    bytes[0] != 0x5a || bytes[1] != 0xa5) {
		return fail("invalid TransCopy header or image size");
	}
	const auto type = bytes[DiskInfoOffset];
	if (!is_pc_transcopy_type(type)) {
		return fail("unsupported TransCopy encoding");
	}
	const auto first = bytes[DiskInfoOffset + 1];
	const auto last  = bytes[DiskInfoOffset + 2];
	const auto heads = bytes[DiskInfoOffset + 3];
	const auto step  = bytes[DiskInfoOffset + 4];
	if (first > last || last >= MaxCylinders || heads == 0 || heads > 2 ||
	    step == 0) {
		return fail("invalid TransCopy geometry");
	}

	BitstreamCapture capture = {};
	capture.cylinders        = last + 1u;
	capture.heads            = heads;
	capture.tracks.resize(capture.cylinders * heads);
	std::map<size_t, size_t> extents = {};
	for (size_t cylinder = 0; cylinder <= last; ++cylinder) {
		const auto captured = cylinder >= first &&
		                      (cylinder - first) % step == 0;
		for (size_t head = 0; head < heads; ++head) {
			// On-disk tables reserve both heads even for
			// single-sided disks.
			const auto entry  = cylinder * 2 + head;
			const auto offset = read_track_offset(bytes, entry);
			if (!captured) {
				if (step > 1 && offset) {
					return fail("TransCopy track occupies a skipped cylinder");
				}
				continue;
			}
			if (!offset) {
				continue;
			}

			const auto length = read_le16(bytes,
			                              LengthTableOffset + 2 * entry);
			if (!length || offset < HeaderBytes ||
			    offset > bytes.size() || length > bytes.size() - offset) {
				return fail("invalid TransCopy track extent");
			}
			const auto next = extents.lower_bound(offset);
			if ((next != extents.end() && offset + length > next->first) ||
			    (next != extents.begin() &&
			     std::prev(next)->second > offset)) {
				return fail("overlapping TransCopy tracks");
			}
			extents.emplace_hint(next, offset, offset + length);

			// Original captures can mix FM and MFM tracks. Missing
			// track types and the unused-table sentinel defer to
			// the disk type; bit 15 is an independent capture flag.
			const auto flags = read_le16(bytes,
			                             FlagsTableOffset + 2 * entry);
			auto track_type = static_cast<uint8_t>((flags >> 8) & 0x7f);
			if (track_type == 0 || track_type == 0x7f || flags == 0x4444) {
				track_type = type;
			}
			if (!is_pc_transcopy_type(track_type)) {
				return fail("unsupported TransCopy track encoding");
			}
			auto& track = capture.tracks[cylinder * heads + head];
			const auto rate_kbps = transcopy_rate_kbps(track_type, length);
			const auto bit_count = size_t{length} * 8;
			track.cycle_ms       = static_cast<double>(bit_count) /
			                 (2 * rate_kbps);

			// The skew table always reserves two entries per cylinder.
			const auto skew_offset = SkewTableOffset +
			                         cylinder * 4 + head * 2;
			const auto skew  = read_le16(bytes, skew_offset);
			const auto shift = skew == NoSkew
			                         ? size_t{0}
			                         : (uint64_t{skew % SkewTicksPerTurn} *
			                                    bit_count +
			                            SkewTicksPerTurn / 2) /
			                                   SkewTicksPerTurn;
			const auto index = (bit_count - shift) % bit_count;
			const auto data  = bytes.subspan(offset, length);
			track.cells.assign(data.begin(), data.end());
			track.bit_count   = bit_count;
			track.index       = static_cast<size_t>(index);
			track.detect_weak = true;
		}
	}
	return capture;
}

std::optional<BitstreamCapture> ParseHxcMfmCapture(const std::span<const uint8_t> bytes,
                                                   std::string& error)
{
	error.clear();
	const auto fail = [&](const char* message) -> std::optional<BitstreamCapture> {
		error = message;
		return {};
	};
	constexpr size_t HeaderSize   = 19;
	constexpr size_t EntrySize    = 11;
	constexpr size_t MaxTrackSize = 64 * 1024;
	constexpr std::array<uint8_t, 7> Signature = {'H', 'X', 'C', 'M', 'F', 'M', 0};
	if (bytes.size() < HeaderSize || bytes.size() > 16 * 1024 * 1024 ||
	    !std::ranges::equal(Signature, bytes.first(Signature.size()))) {
		return fail("invalid HxC MFM header or image size");
	}
	const auto input     = bytes;
	const auto cylinders = capture_read_le(input.subspan(7, 2));
	const auto heads     = input[9];
	const auto rpm       = capture_read_le(input.subspan(10, 2));
	const auto rate_kbps = capture_read_le(input.subspan(12, 2));
	const auto table = static_cast<size_t>(capture_read_le(input.subspan(15, 4)));
	const auto count = static_cast<size_t>(cylinders) * heads;
	if (cylinders == 0 || cylinders > 128 || heads == 0 || heads > 2 ||
	    (rpm && (rpm < 200 || rpm > 600)) || rate_kbps < 125 ||
	    rate_kbps > 1020 || table < HeaderSize || table > input.size() ||
	    count > (input.size() - table) / EntrySize) {
		return fail("invalid HxC MFM track geometry or extent");
	}

	BitstreamCapture capture = {};
	capture.cylinders        = cylinders;
	capture.heads            = heads;
	const auto turn_ms       = rpm ? 60'000.0 / rpm : 200.0;
	capture.tracks.resize(count);
	std::vector<bool> seen(count);
	std::map<size_t, size_t> extents = {};
	const auto data_start            = table + count * EntrySize;
	for (size_t i = 0; i < count; ++i) {
		const auto entry = input.subspan(table + i * EntrySize, EntrySize);
		const auto cylinder = capture_read_le(entry.first(2));
		const auto head     = entry[2];
		const auto length   = static_cast<size_t>(
                        capture_read_le(entry.subspan(3, 4)));
		const auto offset = static_cast<size_t>(
		        capture_read_le(entry.subspan(7, 4)));
		if (cylinder >= cylinders || head >= heads) {
			return fail("invalid HxC MFM track geometry or extent");
		}
		const auto track = static_cast<size_t>(cylinder) * heads + head;
		if (seen[track]) {
			return fail("invalid HxC MFM track geometry or extent");
		}
		seen[track]      = true;
		auto& decoded    = capture.tracks[track];
		decoded.cycle_ms = turn_ms;
		if (!length) {
			continue;
		}
		if (length < 16 || length > MaxTrackSize || offset < data_start ||
		    offset > input.size() || length > input.size() - offset) {
			return fail("invalid HxC MFM track geometry or extent");
		}
		const auto next = extents.lower_bound(offset);
		if ((next != extents.end() && offset + length > next->first) ||
		    (next != extents.begin() && std::prev(next)->second > offset)) {
			return fail("invalid HxC MFM track geometry or extent");
		}
		extents.emplace_hint(next, offset, offset + length);
		const auto cells = input.subspan(offset, length);
		decoded.cells.assign(cells.begin(), cells.end());
		decoded.bit_count = length * 8;
	}
	return capture;
}

std::optional<BitstreamCapture> Parse86fCapture(const std::span<const uint8_t> bytes,
                                                std::string& error)
{
	error.clear();
	const auto fail =
	        [&](std::string_view message) -> std::optional<BitstreamCapture> {
		error = message;
		return {};
	};
	constexpr std::array<uint8_t, 4> Signature = {'8', '6', 'B', 'F'};
	constexpr size_t header_size               = 8;
	constexpr size_t entries_per_head          = 256;
	if (bytes.size() < header_size || bytes.size() > 128 * 1024 * 1024 ||
	    !std::ranges::equal(bytes.first(4), Signature) ||
	    read_le16(bytes, 4) != 0x020c) {
		return fail("expected an uncompressed 86F v2.12 image");
	}
	const auto le32 = [&](size_t offset) {
		return uint32_t{read_le16(bytes, offset)} |
		       (uint32_t{read_le16(bytes, offset + 2)} << 16);
	};
	const auto flags = read_le16(bytes, 6);
	if (flags & ~0x1eff) {
		return fail("unsupported 86F disk flags");
	}
	const bool has_surface            = flags & 1;
	constexpr auto write_protect_flag = 0x10;
	auto capture_read_only            = bool(flags & write_protect_flag);
	const size_t capacity             = (flags >> 1) & 3;
	const size_t num_heads            = (flags & 8) ? 2 : 1;
	const size_t shift                = (flags >> 5) & 3;
	const bool has_count              = flags & 0x80;
	const bool little_endian          = flags & 0x800;
	const bool speedup                = flags & 0x1000;
	const bool total_count            = speedup && !shift && has_count;
	const size_t track_header         = has_count ? 10 : 6;
	const size_t entries              = entries_per_head * num_heads;
	const size_t data_start           = header_size + entries * 4;
	if (bytes.size() < data_start) {
		return fail("truncated 86F track table");
	}

	// The stored array is sized by disk capacity. A revolution uses
	// only the cells implied by the track's rate and RPM.
	constexpr std::array BaseWords = {
	        std::array{std::array{12500, 12625, 12687, 12750},
	                   std::array{12500, 12376, 12315, 12254}},
	        std::array{std::array{25000, 25250, 25375, 25500},
	                   std::array{25000, 24752, 24630, 24509}},
	        std::array{std::array{50000, 50500, 50750, 51000},
	                   std::array{50000, 49504, 49261, 49019}},
	};
	constexpr std::array SpeedFactors = {1000u, 1010u, 1015u, 1020u};
	constexpr std::array RatesKbps    = {500u, 300u, 250u, 1000u};
	const auto density                = capacity < 2 ? 0 : capacity - 1;
	const auto stored_bits = BaseWords[density][speedup][shift] * 16;
	const auto speed_num   = speedup ? 1000 : SpeedFactors[shift];
	const auto speed_den   = speedup ? SpeedFactors[shift] : 1000;
	struct TrackRecord {
		std::span<const uint8_t> record = {};
		size_t array_bytes              = 0;
		size_t cells                    = 0;
		size_t index                    = 0;
		uint32_t rate                   = 0;
	};
	std::vector<TrackRecord> entries_by_track(entries);
	std::map<size_t, size_t> ranges;
	size_t num_cylinders = 0;
	for (size_t entry = 0; entry < entries; ++entry) {
		const size_t pos = le32(header_size + entry * 4);
		if (!pos) {
			continue;
		}
		if (pos < data_start || pos > bytes.size() ||
		    track_header > bytes.size() - pos) {
			return fail("invalid 86F track offset");
		}
		const auto track_flags = read_le16(bytes, pos);
		const auto rate_code   = track_flags & 7;
		const auto rpm_code    = (track_flags >> 5) & 7;
		if ((track_flags & 0x18) > 8 || rpm_code > 1 || rate_code > 3) {
			return fail("unsupported 86F track encoding, speed, or data rate");
		}
		const auto rate = RatesKbps[rate_code] /
		                  ((track_flags & 0x18) == 0 ? 2u : 1u);
		const auto rpm        = rpm_code ? 360 : 300;
		const auto base_cells = (120'000ull * rate * speed_num /
		                         (rpm * speed_den)) &
		                        ~15ull;
		const auto encoded_count = has_count ? le32(pos + 2) : 0;
		const auto extra    = std::bit_cast<int32_t>(encoded_count);
		const int64_t cells = total_count
		                            ? int64_t{encoded_count}
		                            : static_cast<int64_t>(base_cells) + extra;
		const auto storage  = total_count ? cells
		                                  : int64_t{stored_bits} + extra;
		const size_t index  = le32(pos + track_header - 4);
		if (cells <= 0 || cells > storage || storage > 850'000 ||
		    index >= static_cast<size_t>(cells)) {
			return fail("invalid 86F track length or index");
		}
		const auto array_bytes = ((static_cast<size_t>(storage) + 15) / 16) * 2;
		const auto length = track_header +
		                    array_bytes * (has_surface ? 2 : 1);
		if (length > bytes.size() - pos) {
			return fail("truncated 86F track");
		}
		const auto next = ranges.lower_bound(pos);
		if ((next != ranges.end() && pos + length > next->first) ||
		    (next != ranges.begin() && std::prev(next)->second > pos)) {
			return fail("overlapping 86F tracks");
		}
		ranges.emplace_hint(next, pos, pos + length);
		entries_by_track[entry] = {std::span(bytes).subspan(pos, length),
		                           array_bytes,
		                           static_cast<size_t>(cells),
		                           index,
		                           rate};
		num_cylinders = entry / num_heads + 1;
	}

	// 86Box exports thick DD tracks as identical pairs of half tracks.
	bool double_stepped = capacity == 0 && num_cylinders >= 4 &&
	                      num_cylinders % 2 == 0;
	for (size_t cylinder = 0; double_stepped && cylinder < num_cylinders;
	     cylinder += 2) {
		for (size_t head = 0; head < num_heads; ++head) {
			const auto first = cylinder * num_heads + head;
			double_stepped &= std::ranges::equal(
			        entries_by_track[first].record,
			        entries_by_track[first + num_heads].record);
		}
	}
	const size_t step = double_stepped ? 2 : 1;
	if (!num_cylinders) {
		return fail("unsupported 86F geometry");
	}

	BitstreamCapture capture = {};
	capture.cylinders        = static_cast<uint32_t>(num_cylinders / step);
	capture.heads            = static_cast<uint32_t>(num_heads);
	capture.tracks.resize(capture.cylinders * capture.heads);
	for (size_t cylinder = 0; cylinder < num_cylinders; cylinder += step) {
		for (size_t head = 0; head < num_heads; ++head) {
			const auto& track = entries_by_track[cylinder * num_heads + head];
			if (track.record.empty()) {
				continue;
			}
			const auto payload = track.record.subspan(track_header);
			const auto used_bytes = ((track.cells + 15) / 16) * 2;
			std::vector<uint8_t> data(used_bytes);
			std::vector<bool> weak_cells(has_surface ? track.cells : 0);
			for (size_t i = 0; i < used_bytes; ++i) {
				const auto source = little_endian ? i ^ 1u : i;
				data[i]           = payload[source];
				const auto first_bit = i * 8;
				if (has_surface && first_bit < track.cells) {
					// A marked zero is a physical hole that
					// a sector overlay cannot reproduce
					// through a write or format.
					const auto bits = std::min(size_t{8},
					                           track.cells - first_bit);
					const auto mask = static_cast<uint8_t>(
					        0xffu << (8 - bits));
					const auto surface =
					        payload[track.array_bytes + source];
					capture_read_only |= (surface & ~data[i] &
					                      mask) != 0;
					for (size_t bit = 0; bit < bits; ++bit) {
						weak_cells[first_bit + bit] =
						        (surface &
						         (0x80u >> bit)) != 0;
					}
				}
			}
			const auto logical_cylinder = cylinder / step;
			auto& decoded = capture.tracks[logical_cylinder * num_heads + head];
			decoded.cycle_ms = static_cast<double>(track.cells) /
			                   (2 * track.rate);
			decoded.cells      = std::move(data);
			decoded.weak_cells = std::move(weak_cells);
			decoded.bit_count  = track.cells;
			decoded.index      = track.index;
		}
	}
	capture.read_only = capture_read_only;
	return capture;
}
