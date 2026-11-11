// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <iterator>
#include <map>

#include "misc/support.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

constexpr size_t DiskInfoOffset    = 0x100;
constexpr size_t SkewTableOffset   = 0x105;
constexpr size_t BlockTableOffset  = 0x305;
constexpr size_t LengthTableOffset = 0x505;
constexpr size_t HeaderBytes       = 0x905;
constexpr long MaxImageBytes       = 0x1010000;
constexpr size_t MaxCylinders      = 128;
constexpr size_t BlockBytes        = 256;
constexpr size_t SkewTicksPerTurn  = 40'000;
constexpr auto NoSkew              = 0x1111;

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

} // namespace

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::LoadTransCopy(
        FILE* file, const std::string& path, const bool read_only, std::string& error)
{
	const auto fail = [&](const char* message) {
		error = message;
		return std::unique_ptr<FloppyTrackImage>{};
	};
	if (fseek(file, 0, SEEK_END) != 0) {
		return fail("cannot seek TransCopy image");
	}
	const auto size = ftell(file);
	if (size < static_cast<long>(HeaderBytes) || size > MaxImageBytes ||
	    fseek(file, 0, SEEK_SET) != 0) {
		return fail("invalid TransCopy image size");
	}

	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
	    bytes[0] != 0x5a || bytes[1] != 0xa5) {
		return fail("invalid TransCopy header");
	}
	const auto type = bytes[DiskInfoOffset];
	if (type != 0x02 && type != 0x03 && type != 0x07 && type != 0xff) {
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

	auto image       = std::make_unique<FloppyTrackImage>();
	image->cylinders = last + 1u;
	image->heads     = heads;
	image->tracks.resize(image->cylinders * heads);
	std::map<size_t, size_t> extents = {};
	bool found_ids                   = false;
	for (size_t cylinder = 0; cylinder <= last; ++cylinder) {
		const auto captured = cylinder >= first &&
		                      (cylinder - first) % step == 0;
		for (size_t head = 0; head < heads; ++head) {
			const auto entry  = cylinder * heads + head;
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

			auto& track = image->tracks[entry];
			const auto high_density = type == 0x02 || type == 0x03 ||
			                          (type == 0xff && length >= 18'000);
			const auto rate_kbps = high_density ? 500 : 250;
			const auto bit_count = size_t{length} * 8;
			track.cycle_ms       = static_cast<double>(bit_count) /
			                 (2 * rate_kbps);
			track.format_cycle_ms = track.cycle_ms;

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
			track.sectors    = DecodeMfmTrack(
                                std::span(bytes).subspan(offset, length),
                                bit_count,
                                check_cast<size_t>(index),
                                true);
			image->UpdateLogicalGeometry(static_cast<uint32_t>(cylinder),
			                             static_cast<uint32_t>(head),
			                             track.sectors);
			found_ids |= !track.sectors.empty();
		}
	}
	if (!found_ids) {
		return fail("no IBM MFM sector IDs found");
	}
	if (!image->OpenOverlay(path, read_only, FloppyImageFingerprint(bytes), error)) {
		return {};
	}
	return image;
}
