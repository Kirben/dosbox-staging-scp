// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <map>
#include <string_view>

#include "utils/checks.h"

CHECK_NARROWING();

namespace {

uint16_t le16(const std::span<const uint8_t> bytes, const size_t offset)
{
	return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

} // namespace

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::Load86f(FILE* file,
                                                            const std::string& path,
                                                            const bool read_only,
                                                            std::string& error)
{
	error.clear();
	const auto fail = [&](std::string_view message) {
		error = message;
		return std::unique_ptr<FloppyTrackImage>{};
	};

	constexpr std::array<uint8_t, 4> Signature = {'8', '6', 'B', 'F'};
	constexpr size_t header_size               = 8;
	constexpr size_t entries_per_head          = 256;
	constexpr long max_file_size               = 128 * 1024 * 1024;
	if (fseek(file, 0, SEEK_END)) {
		return fail("cannot seek to end of 86F image");
	}
	const auto size = ftell(file);
	if (size < static_cast<long>(header_size) || size > max_file_size ||
	    fseek(file, 0, SEEK_SET)) {
		return fail("invalid 86F image size");
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
	    !std::ranges::equal(std::span(bytes).first(4), Signature) ||
	    le16(bytes, 4) != 0x020c) {
		return fail("expected an uncompressed 86F v2.12 image");
	}

	const auto le32 = [&](size_t offset) {
		return uint32_t{le16(bytes, offset)} |
		       (uint32_t{le16(bytes, offset + 2)} << 16);
	};
	const auto flags = le16(bytes, 6);
	if (flags & ~0x1eff) {
		return fail("unsupported 86F disk flags");
	}
	const bool has_surface    = flags & 1;
	constexpr auto write_protect_flag = 0x10;
	auto capture_read_only            = bool(flags & write_protect_flag);
	const size_t capacity     = (flags >> 1) & 3;
	const size_t num_heads    = (flags & 8) ? 2 : 1;
	const size_t shift        = (flags >> 5) & 3;
	const bool has_count      = flags & 0x80;
	const bool little_endian  = flags & 0x800;
	const bool speedup        = flags & 0x1000;
	const bool total_count    = speedup && !shift && has_count;
	const size_t track_header = has_count ? 10 : 6;
	const size_t entries      = entries_per_head * num_heads;
	const size_t data_start   = header_size + entries * 4;
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
		const auto track_flags = le16(bytes, pos);
		const auto rate_code   = track_flags & 7;
		const auto rpm_code    = (track_flags >> 5) & 7;
		if ((track_flags & 0x18) != 8 || rpm_code > 1 || rate_code > 3) {
			return fail("unsupported 86F track encoding, speed, or data rate");
		}
		const auto rate       = RatesKbps[rate_code];
		const auto rpm        = rpm_code ? 360 : 300;
		const auto base_cells = (120'000ull * rate * speed_num /
		                         (rpm * speed_den)) &
		                        ~15ull;
		const auto encoded_count = has_count ? le32(pos + 2) : 0;
		const auto extra    = std::bit_cast<int32_t>(encoded_count);
		const int64_t cells = total_count
		                            ? int64_t{encoded_count}
		                            : static_cast<int64_t>(base_cells) + extra;
		const auto storage       = total_count ? cells
		                                       : int64_t{stored_bits} + extra;
		const size_t index = le32(pos + track_header - 4);
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

	auto image       = std::make_unique<FloppyTrackImage>();
	image->cylinders = static_cast<uint32_t>(num_cylinders / step);
	image->heads     = static_cast<uint32_t>(num_heads);
	image->tracks.resize(image->cylinders * image->heads);
	bool found_ids = false;
	for (size_t cylinder = 0; cylinder < num_cylinders; cylinder += step) {
		for (size_t head = 0; head < num_heads; ++head) {
			const auto& track = entries_by_track[cylinder * num_heads + head];
			if (track.record.empty()) {
				continue;
			}
			const auto payload = track.record.subspan(track_header);
			const auto used_bytes = ((track.cells + 15) / 16) * 2;
			std::vector<uint8_t> data(used_bytes);
			for (size_t i = 0; i < used_bytes; ++i) {
				const auto source = little_endian ? i ^ 1u : i;
				data[i]           = payload[source];
				const auto first_bit = i * 8;
				if (has_surface && !capture_read_only &&
				    first_bit < track.cells) {
					// A marked zero is a physical hole that
					// a sector overlay cannot reproduce
					// through a write or format.
					const auto bits = std::min(size_t{8},
					                           track.cells - first_bit);
					const auto mask = static_cast<uint8_t>(
					        0xffu << (8 - bits));
					const auto surface =
					        payload[track.array_bytes + source];
					capture_read_only = (surface & ~data[i] &
					                     mask) != 0;
				}
			}
			const auto logical_cylinder = cylinder / step;
			auto& decoded = image->tracks[logical_cylinder * num_heads + head];
			decoded.cycle_ms = static_cast<double>(track.cells) /
			                   (2 * track.rate);
			decoded.format_cycle_ms = decoded.cycle_ms;
			decoded.sectors = DecodeMfmTrack(data, track.cells, track.index);
			image->UpdateLogicalGeometry(static_cast<uint32_t>(
			                                     logical_cylinder),
			                             static_cast<uint32_t>(head),
			                             decoded.sectors);
			found_ids |= !decoded.sectors.empty();
		}
	}
	if (!found_ids) {
		return fail("no IBM MFM sector IDs found");
	}
	if (!image->OpenOverlay(path,
	                        read_only || capture_read_only,
	                        FloppyImageFingerprint(bytes),
	                        error)) {
		return {};
	}
	return image;
}
