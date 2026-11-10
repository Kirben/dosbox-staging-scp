// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/hxc_mfm_image.h"

#include <algorithm>
#include <array>
#include <map>

#include "utils/checks.h"

CHECK_NARROWING();

namespace {

uint32_t read_le(const std::span<const uint8_t> bytes)
{
	uint32_t value = 0;
	for (size_t i = 0; i < bytes.size(); ++i) {
		value |= uint32_t{bytes[i]} << (i * 8);
	}
	return value;
}

uint8_t bit_at(const std::span<const uint8_t> track, const size_t bit)
{
	return static_cast<uint8_t>(
	        (track[(bit / 8) % track.size()] >> (7 - bit % 8)) & 1);
}

uint16_t read_word(const std::span<const uint8_t> track, const size_t bit)
{
	uint16_t word = 0;
	for (size_t i = 0; i < 16; ++i) {
		word = static_cast<uint16_t>((word << 1) | bit_at(track, bit + i));
	}
	return word;
}

uint8_t read_byte(const std::span<const uint8_t> track, const size_t bit)
{
	uint8_t byte = 0;
	for (size_t i = 1; i < 16; i += 2) {
		byte = static_cast<uint8_t>((byte << 1) | bit_at(track, bit + i));
	}
	return byte;
}

uint16_t update_crc(uint16_t crc, const uint8_t byte)
{
	crc ^= static_cast<uint16_t>(byte << 8);
	for (int i = 0; i < 8; ++i) {
		crc = static_cast<uint16_t>((crc << 1) ^
		                            ((crc & 0x8000) ? 0x1021 : 0));
	}
	return crc;
}

struct AddressMark {
	size_t bit   = 0;
	uint8_t type = 0;
};

std::vector<FloppySector> decode_track(const std::span<const uint8_t> track)
{
	constexpr auto SyncWord    = 0x4489;
	constexpr auto IdMark      = 0xfe;
	constexpr auto DataMark    = 0xfb;
	constexpr auto DeletedMark = 0xf8;
	constexpr auto MarkBits    = 64u;
	constexpr auto ByteBits    = 16u;
	constexpr auto SyncCrc = uint16_t{0xcdb4}; // CRC after three A1 bytes

	const auto track_bits          = track.size() * 8;
	std::vector<AddressMark> marks = {};
	auto word                      = read_word(track, 0);
	for (size_t bit = 0; bit < track_bits; ++bit) {
		if (word == SyncWord && read_word(track, bit + 16) == SyncWord &&
		    read_word(track, bit + 32) == SyncWord) {
			const auto mark = read_byte(track, bit + 48);
			if (mark == IdMark || mark == DataMark || mark == DeletedMark) {
				marks.push_back({bit, mark});
			}
		}
		word = static_cast<uint16_t>((word << 1) | bit_at(track, bit + 16));
	}

	std::vector<FloppySector> sectors = {};
	for (size_t i = 0; i < marks.size(); ++i) {
		const auto id = marks[i];
		if (id.type != IdMark) {
			continue;
		}
		const auto id_field         = id.bit + MarkBits;
		std::array<uint8_t, 4> chrn = {};
		auto crc                    = update_crc(SyncCrc, IdMark);
		for (size_t j = 0; j < chrn.size(); ++j) {
			chrn[j] = read_byte(track, id_field + j * ByteBits);
			crc     = update_crc(crc, chrn[j]);
		}
		const auto id_crc = (read_byte(track, id_field + 4 * ByteBits) << 8) |
		                    read_byte(track, id_field + 5 * ByteBits);
		FloppySector sector      = {};
		sector.cylinder          = chrn[0];
		sector.head              = chrn[1];
		sector.id                = chrn[2];
		sector.size_code         = chrn[3];
		sector.revolution_offset = static_cast<double>(id.bit) /
		                           static_cast<double>(track_bits);
		sector.id_crc_error      = crc != id_crc;

		const auto data = marks[(i + 1) % marks.size()];
		if (data.type != IdMark && sector.size_code <= 7) {
			const auto size = size_t{128} << sector.size_code;
			const auto following = marks[(i + 2) % marks.size()].bit;
			const auto gap = (following + track_bits - data.bit) %
			                 track_bits;
			if (gap >= MarkBits + (size + 2) * ByteBits) {
				const auto field = data.bit + MarkBits;
				sector.deleted   = data.type == DeletedMark;
				sector.data.resize(size);
				crc = update_crc(SyncCrc, data.type);
				for (size_t j = 0; j < size; ++j) {
					sector.data[j] = read_byte(track,
					                           field + j * ByteBits);
					crc = update_crc(crc, sector.data[j]);
				}
				const auto data_crc =
				        (read_byte(track, field + size * ByteBits)
				         << 8) |
				        read_byte(track,
				                  field + (size + 1) * ByteBits);
				sector.data_crc_error = crc != data_crc;
			}
		}
		sectors.push_back(std::move(sector));
	}
	return sectors;
}

} // namespace

std::unique_ptr<HxcMfmImage> HxcMfmImage::Load(FILE* file)
{
	constexpr size_t HeaderSize   = 19;
	constexpr size_t EntrySize    = 11;
	constexpr long MaxImageSize   = 16 * 1024 * 1024;
	constexpr size_t MaxTrackSize = 64 * 1024;
	constexpr std::array<uint8_t, 7> Signature = {'H', 'X', 'C', 'M', 'F', 'M', 0};

	if (fseek(file, 0, SEEK_END) != 0) {
		return {};
	}
	const auto size = ftell(file);
	if (size < static_cast<long>(HeaderSize) || size > MaxImageSize ||
	    fseek(file, 0, SEEK_SET) != 0) {
		return {};
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
	    !std::ranges::equal(Signature, std::span(bytes).first(Signature.size()))) {
		return {};
	}
	const std::span<const uint8_t> input = bytes;
	const auto cylinders                 = read_le(input.subspan(7, 2));
	const auto heads                     = input[9];
	const auto rpm                       = read_le(input.subspan(10, 2));
	const auto rate_kbps                 = read_le(input.subspan(12, 2));
	const auto table = static_cast<size_t>(read_le(input.subspan(15, 4)));
	const auto count = static_cast<size_t>(cylinders) * heads;
	if (cylinders == 0 || cylinders > 128 || heads == 0 || heads > 2 ||
	    (rpm && (rpm < 200 || rpm > 600)) || rate_kbps < 245 ||
	    rate_kbps > 1020 || table < HeaderSize || table > input.size() ||
	    count > (input.size() - table) / EntrySize) {
		return {};
	}

	auto image           = std::make_unique<HxcMfmImage>();
	image->cylinders     = cylinders;
	image->heads         = heads;
	image->revolution_ms = rpm ? 60'000.0 / rpm : 200.0;
	image->tracks.resize(count);
	std::vector<bool> seen(count);
	std::map<size_t, size_t> extents = {};
	const auto data_start            = table + count * EntrySize;
	for (size_t i = 0; i < count; ++i) {
		const auto entry = input.subspan(table + i * EntrySize, EntrySize);
		const auto cylinder = read_le(entry.first(2));
		const auto head     = entry[2];
		const auto length = static_cast<size_t>(read_le(entry.subspan(3, 4)));
		const auto offset = static_cast<size_t>(read_le(entry.subspan(7, 4)));
		if (cylinder >= cylinders || head >= heads) {
			return {};
		}
		const auto track = static_cast<size_t>(cylinder) * heads + head;
		if (seen[track]) {
			return {};
		}
		seen[track] = true;
		if (!length) {
			continue;
		}
		if (length < 16 || length > MaxTrackSize || offset < data_start ||
		    offset > input.size() || length > input.size() - offset) {
			return {};
		}
		const auto next = extents.lower_bound(offset);
		if ((next != extents.end() && offset + length > next->first) ||
		    (next != extents.begin() && std::prev(next)->second > offset)) {
			return {};
		}
		extents.emplace_hint(next, offset, offset + length);
		image->tracks[track] = decode_track(input.subspan(offset, length));

		// Build the logical CHS geometry from conventional 512-byte IDs.
		std::array<bool, 256> logical_ids = {};
		for (const auto& sector : image->tracks[track]) {
			if (sector.cylinder == cylinder && sector.head == head &&
			    sector.data.size() == 512 && !sector.id_crc_error) {
				logical_ids[sector.id] = true;
			}
		}
		const auto first_missing = std::find(logical_ids.begin() + 1,
		                                     logical_ids.end(),
		                                     false);
		image->logical_sectors = std::max(image->logical_sectors,
		                                  static_cast<uint32_t>(
		                                          first_missing -
		                                          logical_ids.begin() - 1));
	}
	if (std::ranges::all_of(image->tracks,
	                        [](const auto& track) { return track.empty(); })) {
		return {};
	}
	return image;
}

std::span<const FloppySector> HxcMfmImage::GetTrack(const uint32_t cylinder,
                                                    const uint32_t head) const
{
	if (cylinder >= cylinders || head >= heads) {
		return {};
	}
	return tracks[cylinder * heads + head];
}
