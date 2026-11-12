// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
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

struct MfmBitstream {
	std::span<const uint8_t> bytes = {};
	size_t bit_count               = 0;
};

uint8_t bit_at(const MfmBitstream track, size_t bit)
{
	if (bit >= track.bit_count) {
		bit %= track.bit_count;
	}
	return static_cast<uint8_t>((track.bytes[bit / 8] >> (7 - bit % 8)) & 1);
}

uint16_t read_word(const MfmBitstream track, const size_t bit)
{
	uint16_t word = 0;
	for (size_t i = 0; i < 16; ++i) {
		word = static_cast<uint16_t>((word << 1) | bit_at(track, bit + i));
	}
	return word;
}

uint8_t read_byte(const MfmBitstream track, const size_t bit)
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

constexpr auto SyncCrc = uint16_t{0xcdb4}; // CRC after three A1 bytes

std::vector<bool> find_weak_cells(const MfmBitstream track)
{
	// TransCopy no-flux regions contain at least 16 consecutive zero cells.
	constexpr size_t MinWeakRun = 16;
	std::vector<bool> weak(track.bit_count);
	size_t first_transition = 0;
	while (first_transition < track.bit_count &&
	       !bit_at(track, first_transition)) {
		++first_transition;
	}
	if (first_transition == track.bit_count) {
		return weak; // A blank track contains no data fields.
	}

	const auto end = first_transition + track.bit_count + 1;
	auto start     = first_transition + 1;
	for (auto bit = start; bit < end; ++bit) {
		if (!bit_at(track, bit)) {
			continue;
		}
		if (bit - start >= MinWeakRun) {
			for (auto cell = start; cell < bit; ++cell) {
				weak[cell % track.bit_count] = true;
			}
		}
		start = bit + 1;
	}
	return weak;
}

} // namespace

bool FloppySector::SampleWeakData(const uint64_t turn,
                                  std::vector<uint8_t>& sampled) const
{
	assert(weak_mask.size() == data.size() + 2);
	sampled    = data;
	auto state = (turn + 1) * 0x9e3779b97f4a7c15ull ^ weak_seed;
	if (!state) {
		state = 1;
	}
	const auto sample_byte = [&](const uint8_t byte, const uint8_t mask) {
		if (!mask) {
			return byte;
		}
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		const auto noise = static_cast<uint8_t>(
		        (state * 0x2545f4914f6cdd1dull) >> 56);
		return static_cast<uint8_t>((byte & ~mask) | (noise & mask));
	};
	auto crc = update_crc(SyncCrc, deleted ? 0xf8 : 0xfb);
	for (size_t i = 0; i < data.size(); ++i) {
		sampled[i] = sample_byte(data[i], weak_mask[i]);
		crc        = update_crc(crc, sampled[i]);
	}
	const auto high = sample_byte(static_cast<uint8_t>(recorded_crc >> 8),
	                              weak_mask[data.size()]);
	const auto low  = sample_byte(static_cast<uint8_t>(recorded_crc),
                                     weak_mask[data.size() + 1]);
	return crc != ((high << 8) | low);
}

bool FloppySector::ReadTrackData(const size_t size, const uint64_t turn,
                                 std::vector<uint8_t>& sampled, bool& bad_crc) const
{
	if (!mfm_track || !size || size > 16384) {
		return false;
	}
	const MfmBitstream track = {mfm_track->cells, mfm_track->bit_count};
	FloppySector field       = {};
	field.data.resize(size);
	field.deleted   = deleted;
	field.weak_seed = weak_seed;
	if (!mfm_track->weak_cells.empty()) {
		field.weak_mask.resize(size + 2);
	}
	auto crc    = update_crc(SyncCrc, deleted ? 0xf8 : 0xfb);
	auto offset = data_bit;
	for (size_t i = 0; i < size + 2; ++i) {
		// The data field keeps its byte cadence until the commanded
		// size is complete. Embedded address marks and index do not
		// resync it.
		const auto byte = read_byte(track, offset);
		if (i < size) {
			field.data[i] = byte;
			crc           = update_crc(crc, byte);
		} else {
			field.recorded_crc = static_cast<uint16_t>(
			        (field.recorded_crc << 8) | byte);
		}
		for (size_t bit = 0; !field.weak_mask.empty() && bit < 8; ++bit) {
			const auto cell = (offset + 2 * bit + 1) % track.bit_count;
			if (mfm_track->weak_cells[cell]) {
				field.weak_mask[i] |= check_cast<uint8_t>(0x80u >> bit);
			}
		}
		offset += 16;
	}
	if (std::ranges::any_of(field.weak_mask,
	                        [](const auto mask) { return mask != 0; })) {
		bad_crc = field.SampleWeakData(turn, sampled);
	} else {
		bad_crc = crc != field.recorded_crc;
		sampled = std::move(field.data);
	}
	return true;
}

void FloppySector::UpdateTrackData()
{
	if (!mfm_track) {
		return;
	}
	auto& track   = *mfm_track;
	auto position = (data_bit + track.bit_count - 16) % track.bit_count;
	const auto write_bit = [&](const bool value) {
		const auto mask = check_cast<uint8_t>(0x80u >> (position % 8));
		auto& byte      = track.cells[position / 8];
		byte            = check_cast<uint8_t>(
                        value ? byte | mask : byte & static_cast<uint8_t>(~mask));
		if (!track.weak_cells.empty()) {
			track.weak_cells[position] = false;
		}
		position = (position + 1) % track.bit_count;
	};
	bool previous = true; // Last data bit of the preceding A1 sync byte.
	const auto append = [&](const uint8_t byte) {
		for (int bit = 7; bit >= 0; --bit) {
			const auto value = ((byte >> bit) & 1) != 0;
			write_bit(!(previous || value));
			write_bit(value);
			previous = value;
		}
	};
	const auto mark = deleted ? uint8_t{0xf8} : uint8_t{0xfb};
	auto crc        = update_crc(SyncCrc, mark);
	append(mark);
	for (const auto byte : data) {
		append(byte);
		crc = update_crc(crc, byte);
	}
	append(check_cast<uint8_t>(crc >> 8));
	append(static_cast<uint8_t>(crc));
	const MfmBitstream cells = {track.cells, track.bit_count};
	write_bit(!(previous || bit_at(cells, position + 1)));
}

std::vector<FloppySector> FloppyTrackImage::DecodeMfmTrack(
        const std::span<const uint8_t> bytes, const size_t bit_count,
        const size_t index, const bool detect_weak,
        std::shared_ptr<FloppyMfmTrack> captured)
{
	constexpr auto SyncWord    = 0x4489;
	constexpr auto IdMark      = 0xfe;
	constexpr auto DataMark    = 0xfb;
	constexpr auto DeletedMark = 0xf8;
	constexpr auto MarkBits    = 64u;
	constexpr auto ByteBits    = 16u;

	const auto track_bits = bit_count ? bit_count : bytes.size() * 8;
	if (!track_bits || track_bits > bytes.size() * 8 || index >= track_bits) {
		return {};
	}
	const MfmBitstream track = {bytes, track_bits};
	if (!captured) {
		captured = std::make_shared<FloppyMfmTrack>();
		captured->cells.assign(bytes.begin(), bytes.end());
		captured->bit_count  = track_bits;
		captured->index      = index;
		captured->weak_cells = detect_weak ? find_weak_cells(track)
		                                   : std::vector<bool>{};
	}
	const auto& weak_cells         = captured->weak_cells;
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
		FloppySector sector   = {};
		sector.id_bit         = id.bit;
		sector.cylinder       = chrn[0];
		sector.head           = chrn[1];
		sector.id             = chrn[2];
		sector.size_code      = chrn[3];
		sector.cycle_fraction = static_cast<double>(
		                                (id.bit + track_bits - index) %
		                                track_bits) /
		                        static_cast<double>(track_bits);
		sector.id_crc_error = crc != id_crc;

		const auto data = marks[(i + 1) % marks.size()];
		if (data.type != IdMark && sector.size_code <= 7) {
			const auto size = size_t{128} << sector.size_code;
			// Once a data mark is found, the controller consumes
			// the entire field, including embedded address marks
			// and index crossings.
			const auto field       = data.bit + MarkBits;
			sector.mfm_track       = captured;
			sector.data_bit        = field % track_bits;
			sector.data_delay_bits = (field + track_bits - id.bit) %
			                         track_bits;
			sector.weak_seed = data.bit;
			sector.deleted   = data.type == DeletedMark;
			sector.data.resize(size);
			crc = update_crc(SyncCrc, data.type);
			for (size_t j = 0; j < size; ++j) {
				sector.data[j] = read_byte(track,
				                           field + j * ByteBits);
				crc = update_crc(crc, sector.data[j]);
			}
			const auto data_crc =
			        (read_byte(track, field + size * ByteBits) << 8) |
			        read_byte(track, field + (size + 1) * ByteBits);
			sector.data_crc_error = crc != data_crc;
			if (!weak_cells.empty()) {
				std::vector<uint8_t> mask(size + 2);
				for (size_t byte = 0; byte < mask.size(); ++byte) {
					for (size_t bit = 0; bit < 8; ++bit) {
						const auto cell = field +
						                  byte * ByteBits +
						                  2 * bit + 1;
						if (weak_cells[cell % track_bits]) {
							mask[byte] |= static_cast<uint8_t>(
							        0x80u >> bit);
						}
					}
				}
				if (std::ranges::any_of(mask, [](const auto byte) {
					    return byte != 0;
				    })) {
					sector.weak_mask = std::move(mask);
					sector.recorded_crc = static_cast<uint16_t>(
					        data_crc);
					sector.weak_seed = data.bit;
				}
			}
		}
		if (sector.id_crc_error && data.type != IdMark &&
		    !sector.data.empty()) {
			// Read Track uses the command's size even when the ID
			// was damaged.
			const auto following = marks[(i + 2) % marks.size()].bit;
			const auto gap = (following + track_bits - data.bit) %
			                 track_bits;
			const auto field = data.bit + MarkBits;
			auto field_crc   = update_crc(SyncCrc, data.type);
			for (auto size = size_t{128};
			     size <= 16384 && gap >= MarkBits + (size + 2) * ByteBits;
			     size *= 2) {
				const auto start = size == 128 ? size_t{0}
				                               : size / 2;
				for (auto j = start; j < size; ++j) {
					field_crc = update_crc(
					        field_crc,
					        read_byte(track, field + j * ByteBits));
				}
				const auto stored_crc =
				        (read_byte(track, field + size * ByteBits)
				         << 8) |
				        read_byte(track,
				                  field + (size + 1) * ByteBits);
				if (size > sector.data.size() &&
				    field_crc == stored_crc) {
					sector.extended_data.resize(size);
					for (size_t j = 0; j < size; ++j) {
						sector.extended_data[j] = read_byte(
						        track, field + j * ByteBits);
					}
					break;
				}
			}
		}
		sectors.push_back(std::move(sector));
	}
	if (index) {
		std::stable_sort(sectors.begin(),
		                 sectors.end(),
		                 [](const auto& a, const auto& b) {
			                 return a.cycle_fraction < b.cycle_fraction;
		                 });
	}
	return sectors;
}

std::vector<FloppySector> FloppyTrackImage::FormatMfmTrack(
        const std::span<const FloppySector> previous, const size_t revolution,
        const double duration_ms, const TrackFormat& format)
{
	const auto found = std::ranges::find_if(previous, [&](const auto& sector) {
		return sector.revolution == revolution && sector.mfm_track;
	});
	const auto source = found != previous.end() ? found->mfm_track : nullptr;
	const auto bits = source ? source->bit_count
	                         : check_cast<size_t>(std::lround(duration_ms * 500));
	auto captured       = std::make_shared<FloppyMfmTrack>();
	captured->bit_count = bits;
	captured->cells.resize((bits + 7) / 8);
	const auto count   = format.ids.size() / 4;
	const auto partial = count < format.requested;
	const auto cutoff  = bits * count / format.requested;
	if (partial && source && !source->weak_cells.empty()) {
		captured->weak_cells.resize(bits);
	}
	// Keep the original suffix on an interrupted format, normalized to index.
	for (size_t bit = 0; bit < bits; ++bit) {
		const auto old      = source ? (bit + source->index) % bits : 0;
		const auto preserve = partial && source && bit >= cutoff;
		const auto value = preserve ? bit_at({source->cells, bits}, old)
		                            : (0x9254u >> (15 - bit % 16)) & 1u;
		captured->cells[bit / 8] |= check_cast<uint8_t>(value
		                                                << (7 - bit % 8));
		if (preserve && !captured->weak_cells.empty()) {
			captured->weak_cells[bit] = source->weak_cells[old];
		}
	}
	size_t position      = 0;
	bool last_data       = false;
	const auto write_bit = [&](const bool value) {
		if (!partial || position < cutoff) {
			const auto mask = check_cast<uint8_t>(0x80u >>
			                                      (position % 8));
			auto& byte      = captured->cells[position / 8];
			byte            = check_cast<uint8_t>(
			        value ? byte | mask
			              : byte & static_cast<uint8_t>(~mask));
			if (!captured->weak_cells.empty()) {
				captured->weak_cells[position] = false;
			}
		}
		// A partial field must not overwrite the surviving suffix or
		// wrap back into the committed prefix.
		position = partial ? position + 1 : (position + 1) % bits;
	};
	const auto append = [&](const uint8_t byte) {
		for (int bit = 7; bit >= 0; --bit) {
			const auto data = ((byte >> bit) & 1) != 0;
			write_bit(!(last_data || data));
			write_bit(data);
			last_data = data;
		}
	};
	const auto sync = [&]() {
		for (auto byte = 0; byte < 3; ++byte) {
			for (auto bit = 15; bit >= 0; --bit) {
				write_bit(((0x4489u >> bit) & 1u) != 0);
			}
		}
		last_data = true;
	};
	const auto append_crc = [&](const uint16_t crc) {
		append(check_cast<uint8_t>(crc >> 8));
		append(static_cast<uint8_t>(crc));
	};
	for (size_t i = 0; i < count; ++i) {
		position = i * bits / format.requested;
		sync();
		append(0xfe);
		auto crc      = update_crc(SyncCrc, 0xfe);
		const auto id = std::span(format.ids).subspan(i * 4, 4);
		for (const auto byte : id) {
			append(byte);
			crc = update_crc(crc, byte);
		}
		append_crc(crc);
		for (auto gap = 0; gap < 22; ++gap) {
			append(0x4e);
		}
		for (auto gap = 0; gap < 12; ++gap) {
			append(0);
		}
		sync();
		append(0xfb);
		crc = update_crc(SyncCrc, 0xfb);
		// Command N controls the data field; ID N is just an ID byte.
		const auto size_code = format.size_code == 0xff ? id[3]
		                                                : format.size_code;
		for (size_t byte = 0; byte < (size_t{128} << size_code); ++byte) {
			append(format.fill);
			crc = update_crc(crc, format.fill);
		}
		append_crc(crc);
	}
	return DecodeMfmTrack(captured->cells, bits, 0, false, captured);
}

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::LoadHxcMfm(
        FILE* file, const std::string& path, const bool read_only, std::string& error)
{
	constexpr size_t HeaderSize                = 19;
	constexpr size_t EntrySize                 = 11;
	constexpr long MaxImageSize                = 16 * 1024 * 1024;
	constexpr size_t MaxTrackSize              = 64 * 1024;
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

	auto image         = std::make_unique<FloppyTrackImage>();
	image->cylinders   = cylinders;
	image->heads       = heads;
	const auto turn_ms = rpm ? 60'000.0 / rpm : 200.0;
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
		auto& decoded    = image->tracks[track];
		decoded.cycle_ms = turn_ms;
		decoded.format_cycle_ms = turn_ms;
		decoded.sectors = DecodeMfmTrack(input.subspan(offset, length));
		image->UpdateLogicalGeometry(cylinder, head, decoded.sectors);
	}
	if (std::ranges::all_of(image->tracks, [](const auto& track) {
		    return track.sectors.empty();
	    })) {
		return {};
	}
	if (!image->OpenOverlay(path, read_only, FloppyImageFingerprint(bytes), error)) {
		return {};
	}
	return image;
}

std::span<const FloppySector> FloppyTrackImage::GetTrack(const uint32_t cylinder,
                                                         const uint32_t head) const
{
	if (cylinder >= cylinders || head >= heads) {
		return {};
	}
	const auto& track = tracks[cylinder * heads + head];
	if (track.dirty) {
		// Keep the original slots stable for overlay addresses, but
		// rebuild the visible IDs and data from the cells changed by a
		// write.
		std::map<const FloppyMfmTrack*, std::vector<FloppySector>> decoded = {};
		track.visible_sectors.clear();
		for (size_t i = 0; i < track.sectors.size(); ++i) {
			const auto& original = track.sectors[i];
			auto sector          = original;
			auto raw             = original.mfm_track;
			if (!raw && track.cells_changed) {
				const auto same_turn = std::ranges::find_if(
				        track.sectors, [&](const auto& other) {
					        return other.revolution ==
					                       original.revolution &&
					               other.mfm_track;
				        });
				if (same_turn != track.sectors.end()) {
					raw = same_turn->mfm_track;
				}
			}
			if (raw && track.cells_changed) {
				auto [entry,
				      inserted] = decoded.try_emplace(raw.get());
				if (inserted) {
					entry->second = DecodeMfmTrack(raw->cells,
					                               raw->bit_count,
					                               raw->index,
					                               false,
					                               raw);
				}
				const auto found =
				        std::ranges::find(entry->second,
				                          original.id_bit,
				                          &FloppySector::id_bit);
				if (found == entry->second.end()) {
					continue;
				}
				sector                = *found;
				sector.revolution     = original.revolution;
				sector.cycle_fraction = original.cycle_fraction;
			}
			sector.overlay_slot = i + 1;
			track.visible_sectors.push_back(std::move(sector));
		}
		track.dirty = false;
	}
	return track.visible_sectors;
}

double FloppyTrackImage::GetCycleMs(const uint32_t cylinder, const uint32_t head) const
{
	return cylinder < cylinders && head < heads
	             ? tracks[cylinder * heads + head].cycle_ms
	             : 200.0;
}

void FloppyTrackImage::UpdateLogicalGeometry(const uint32_t cylinder,
                                             const uint32_t head,
                                             const std::span<const FloppySector> sectors)
{
	// Build the logical CHS geometry from conventional 512-byte IDs.
	std::array<bool, 256> logical_ids = {};
	for (const auto& sector : sectors) {
		if (sector.cylinder == cylinder && sector.head == head &&
		    sector.data.size() == 512 && !sector.id_crc_error) {
			logical_ids[sector.id] = true;
		}
	}
	const auto first_missing = std::find(logical_ids.begin() + 1,
	                                     logical_ids.end(),
	                                     false);
	logical_sectors          = std::max(logical_sectors,
                                   static_cast<uint32_t>(first_missing -
                                                         logical_ids.begin() - 1));
}
