// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <map>

#include "ints/bitstream_capture.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

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

std::shared_ptr<FloppyMfmTrack> FloppyMfmTrack::RecoverFlux(
        const std::span<const uint64_t> transitions_ns,
        const uint64_t duration_ns, const double nominal_cell_ns)
{
	constexpr size_t MaxCells      = 2'100'000;
	std::vector<uint8_t> bitstream = {};
	double clock_ns                = nominal_cell_ns;
	double phase_ns                = 0;
	uint64_t previous_ns           = 0;
	size_t cells_written           = 0;
	for (const auto time_ns : transitions_ns) {
		phase_ns += static_cast<double>(time_ns - previous_ns);
		previous_ns      = time_ns;
		const auto cells = static_cast<size_t>(
		        std::max(0.0, std::floor(phase_ns / clock_ns + 0.5)));
		if (cells == 0) {
			continue;
		}
		if (cells > MaxCells - cells_written) {
			return {};
		}
		cells_written += cells;
		bitstream.resize((cells_written + 7) / 8);
		const auto bit = cells_written - 1;
		bitstream[bit / 8] |= static_cast<uint8_t>(0x80u >> (bit % 8));
		phase_ns -= static_cast<double>(cells) * clock_ns;
		clock_ns += (cells <= 4 ? phase_ns : nominal_cell_ns - clock_ns) * 0.05;
		clock_ns = std::clamp(clock_ns,
		                      nominal_cell_ns * 0.9,
		                      nominal_cell_ns * 1.1);
		phase_ns *= 0.4;
	}

	const auto tail = static_cast<size_t>(std::round(
	        static_cast<double>(duration_ns - previous_ns) / clock_ns));
	if (tail > MaxCells - cells_written) {
		return {};
	}
	const auto bit_count = cells_written + tail;
	bitstream.resize((bit_count + 7) / 8);
	auto raw       = std::make_shared<FloppyMfmTrack>();
	raw->cells     = std::move(bitstream);
	raw->bit_count = bit_count;
	return raw;
}

std::shared_ptr<FloppyMfmTrack> FloppyMfmTrack::Sample(const uint64_t cycle,
                                                       const double duration_ms,
                                                       const uint16_t rate_kbps) const
{
	auto sampled = std::make_shared<FloppyMfmTrack>(*this);
	for (size_t bit = 0; bit < weak_cells.size(); ++bit) {
		if (!weak_cells[bit]) {
			continue;
		}
		// The same physical cell has one value during a turn, including
		// when separate commands encounter it. The next turn resamples it.
		auto noise = (cycle + 1) * 0x9e3779b97f4a7c15ull + bit;
		noise      = (noise ^ (noise >> 30)) * 0xbf58476d1ce4e5b9ull;
		noise      = (noise ^ (noise >> 27)) * 0x94d049bb133111ebull;
		noise ^= noise >> 31;
		const auto mask = check_cast<uint8_t>(0x80u >> (bit % 8));
		if (noise & 1) {
			sampled->cells[bit / 8] |= mask;
		} else {
			sampled->cells[bit / 8] &= static_cast<uint8_t>(~mask);
		}
	}
	// This snapshot is already sampled; data reads must not sample it twice.
	sampled->weak_cells.clear();
	if (!rate_kbps || !bit_count || duration_ms <= 0) {
		return sampled;
	}
	const auto cell_ns = duration_ms * 1'000'000 / static_cast<double>(bit_count);
	const auto selected_cell_ns = 500'000.0 / rate_kbps;
	// Bitstream captures already contain recovered cells. Preserve their
	// exact positions when the selected separator can lock to that clock.
	if (cell_ns >= selected_cell_ns * 0.9 && cell_ns <= selected_cell_ns * 1.1) {
		return sampled;
	}
	std::vector<uint64_t> transitions = {};
	for (size_t bit = 0; bit < bit_count; ++bit) {
		if (bit_at({sampled->cells, bit_count}, bit)) {
			transitions.push_back(check_cast<uint64_t>(std::llround(
			        static_cast<double>(bit + 1) * cell_ns)));
		}
	}
	auto decoded = RecoverFlux(transitions,
	                           check_cast<uint64_t>(
	                                   std::llround(duration_ms * 1'000'000)),
	                           selected_cell_ns);
	if (!decoded || !decoded->bit_count) {
		return {};
	}
	const auto position = [&](const size_t bit) {
		return check_cast<size_t>(
		        std::llround(static_cast<double>(bit) *
		                     static_cast<double>(decoded->bit_count) /
		                     static_cast<double>(bit_count)));
	};
	decoded->index = position(index) % decoded->bit_count;
	for (const auto end : revolution_ends_bits) {
		decoded->revolution_ends_bits.push_back(position(end));
	}
	return decoded;
}

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
	auto crc = update_crc(fm ? uint16_t{0xffff} : SyncCrc, deleted ? 0xf8 : 0xfb);
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
	field.fm        = fm;
	field.weak_seed = weak_seed;
	if (!mfm_track->weak_cells.empty()) {
		field.weak_mask.resize(size + 2);
	}
	auto crc = update_crc(fm ? uint16_t{0xffff} : SyncCrc, deleted ? 0xf8 : 0xfb);
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

void FloppySector::UpdateTrackData(const bool create_data_mark)
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
	if (create_data_mark) {
		// Write Data creates the sync field and DAM even if the old
		// track contained only an ID field.
		position = (data_bit + track.bit_count - (256 % track.bit_count)) %
		           track.bit_count;
		previous = false;
		for (auto byte = 0; byte < 12; ++byte) {
			append(0);
		}
		for (auto byte = 0; byte < 3; ++byte) {
			for (auto bit = 15; bit >= 0; --bit) {
				write_bit(((0x4489u >> bit) & 1u) != 0);
			}
		}
		previous = true;
	}
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

std::vector<FloppySector> FloppyTrackImage::DecodeTrack(
        const std::span<const uint8_t> bytes, const size_t bit_count,
        const size_t index, const bool detect_weak,
        std::shared_ptr<FloppyMfmTrack> captured, const bool fm)
{
	constexpr auto SyncWord    = 0x4489;
	constexpr auto IdMark      = 0xfe;
	constexpr auto DataMark    = 0xfb;
	constexpr auto DeletedMark = 0xf8;
	const auto mark_bits       = fm ? 16u : 64u;
	const auto crc_seed        = fm ? uint16_t{0xffff} : SyncCrc;
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
		if (fm && (word == 0xf57e || word == 0xf56f || word == 0xf56a)) {
			marks.push_back({bit, read_byte(track, bit)});
		} else if (!fm && word == SyncWord &&
		           read_word(track, bit + 16) == SyncWord &&
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
		const auto id_field         = id.bit + mark_bits;
		std::array<uint8_t, 4> chrn = {};
		auto crc                    = update_crc(crc_seed, IdMark);
		for (size_t j = 0; j < chrn.size(); ++j) {
			chrn[j] = read_byte(track, id_field + j * ByteBits);
			crc     = update_crc(crc, chrn[j]);
		}
		const auto id_crc = (read_byte(track, id_field + 4 * ByteBits) << 8) |
		                    read_byte(track, id_field + 5 * ByteBits);
		FloppySector sector   = {};
		sector.fm             = fm;
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
		sector.mfm_track    = captured;
		// A write to an ID-only sector creates the standard Gap 2
		// sync and data field following its ID and CRC.
		sector.data_delay_bits = (fm ? 25 : 48) * ByteBits;
		sector.data_bit = (id.bit + sector.data_delay_bits) % track_bits;

		const auto data = marks[(i + 1) % marks.size()];
		if (data.type != IdMark && sector.size_code <= 7) {
			const auto size = size_t{128} << sector.size_code;
			// Once a data mark is found, the controller consumes
			// the entire field, including embedded address marks
			// and index crossings.
			const auto field       = data.bit + mark_bits;
			sector.mfm_track       = captured;
			sector.data_bit        = field % track_bits;
			sector.data_delay_bits = (field + track_bits - id.bit) %
			                         track_bits;
			sector.weak_seed = data.bit;
			sector.deleted   = data.type == DeletedMark;
			sector.data.resize(size);
			crc = update_crc(crc_seed, data.type);
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
			const auto field = data.bit + mark_bits;
			auto field_crc   = update_crc(crc_seed, data.type);
			for (auto size = size_t{128};
			     size <= 16384 && gap >= mark_bits + (size + 2) * ByteBits;
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

std::vector<FloppySector> FloppyTrackImage::DecodeRevolutions(
        const std::span<const std::shared_ptr<FloppyMfmTrack>> revolutions,
        const std::span<const double> ends, const bool fm)
{
	assert(!revolutions.empty() && revolutions.size() == ends.size());
	if (revolutions.size() == 1) {
		const auto& raw = revolutions.front();
		return DecodeTrack(raw->cells, raw->bit_count, raw->index, false, raw, fm);
	}
	auto raw = std::make_shared<FloppyMfmTrack>();
	for (const auto& turn : revolutions) {
		raw->bit_count += turn->bit_count;
		raw->revolution_ends_bits.push_back(raw->bit_count);
	}
	raw->cells.resize((raw->bit_count + 7) / 8);
	if (std::ranges::any_of(revolutions, [](const auto& turn) {
		    return !turn->weak_cells.empty();
	    })) {
		raw->weak_cells.resize(raw->bit_count);
	}
	size_t position = 0;
	for (const auto& turn : revolutions) {
		for (size_t bit = 0; bit < turn->bit_count; ++bit, ++position) {
			const auto source = (bit + turn->index) % turn->bit_count;
			if (bit_at({turn->cells, turn->bit_count}, source)) {
				raw->cells[position / 8] |= check_cast<uint8_t>(
				        0x80u >> (position % 8));
			}
			if (!turn->weak_cells.empty()) {
				raw->weak_cells[position] = turn->weak_cells[source];
			}
		}
	}
	auto sectors = DecodeTrack(raw->cells, raw->bit_count, 0, false, raw, fm);
	PositionSectors(sectors, ends);
	return sectors;
}

void FloppyTrackImage::PositionSectors(std::vector<FloppySector>& sectors,
                                       const std::span<const double> ends)
{
	for (auto& sector : sectors) {
		const auto next = std::upper_bound(
		        sector.mfm_track->revolution_ends_bits.begin(),
		        sector.mfm_track->revolution_ends_bits.end(),
		        sector.id_bit);
		sector.revolution = check_cast<size_t>(
		        next - sector.mfm_track->revolution_ends_bits.begin());
		const auto start_bit =
		        sector.revolution
		                ? sector.mfm_track->revolution_ends_bits[sector.revolution - 1]
		                : 0;
		const auto start_time = sector.revolution
		                              ? ends[sector.revolution - 1]
		                              : 0;
		const auto duration   = ends[sector.revolution] - start_time;
		sector.cycle_fraction =
		        start_time +
		        duration * static_cast<double>(sector.id_bit - start_bit) /
		                static_cast<double>(sector.mfm_track->GetRevolutionBitCount(
		                        sector.revolution));
	}
}

std::shared_ptr<FloppyMfmTrack> FloppyTrackImage::FormatMfmTrack(
        const std::span<const FloppySector> previous, const size_t revolution,
        const double duration_ms, const TrackFormat& format)
{
	const auto found = std::ranges::find_if(previous, [&](const auto& sector) {
		return sector.mfm_track &&
		       (sector.revolution == revolution ||
		        sector.mfm_track->revolution_ends_bits.size() > revolution);
	});
	const auto source = found != previous.end() ? found->mfm_track : nullptr;
	const auto source_bits = source ? source->GetRevolutionBitCount(revolution)
	                                : 0;
	const auto source_start = source && !source->revolution_ends_bits.empty() &&
	                                          revolution
	                                ? source->revolution_ends_bits[revolution - 1]
	                                : 0;
	const auto rate = format.rate_kbps ? format.rate_kbps : uint16_t{250};
	auto bits = check_cast<size_t>(std::lround(duration_ms * 2 * rate));
	if (!format.rate_kbps && source) {
		bits = source_bits;
	}
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
		const auto preserve = partial && source && bit >= cutoff;
		auto value          = (0x9254u >> (15 - bit % 16)) & 1u;
		if (preserve) {
			value = 0;
			// Keep suffix transition times when changing the cell
			// clock.
			const auto begin = check_cast<size_t>(
			        uint64_t{bit} * source_bits / bits);
			const auto end = check_cast<size_t>((uint64_t{bit} + 1) *
			                                    source_bits / bits);
			for (auto old = begin; old < end; ++old) {
				const auto index = (old + source_start +
				                    source->index) %
				                   source->bit_count;
				value |= bit_at({source->cells, source->bit_count},
				                index);
				if (!captured->weak_cells.empty()) {
					captured->weak_cells[bit] =
					        captured->weak_cells[bit] ||
					        source->weak_cells[index];
				}
			}
		}
		captured->cells[bit / 8] |= check_cast<uint8_t>(value
		                                                << (7 - bit % 8));
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
	return captured;
}

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::FromBitstreamCapture(
        const BitstreamCapture& capture, const std::string& path,
        const bool read_only, const uint64_t fingerprint, std::string& error)
{
	auto image       = std::make_unique<FloppyTrackImage>();
	image->cylinders = capture.cylinders;
	image->heads     = capture.heads;
	image->tracks.resize(capture.tracks.size());
	bool found_ids = false;
	for (size_t i = 0; i < capture.tracks.size(); ++i) {
		const auto& captured  = capture.tracks[i];
		auto& track           = image->tracks[i];
		track.cycle_ms        = captured.cycle_ms;
		track.format_cycle_ms = track.cycle_ms;
		if (captured.cells.empty()) {
			continue;
		}
		auto raw        = std::make_shared<FloppyMfmTrack>();
		raw->cells      = captured.cells;
		raw->bit_count  = captured.bit_count ? captured.bit_count
		                                     : captured.cells.size() * 8;
		raw->index      = captured.index;
		raw->weak_cells = captured.detect_weak
		                        ? find_weak_cells({raw->cells, raw->bit_count})
		                        : captured.weak_cells;
		track.raw_tracks.push_back(raw);
		track.sectors = DecodeTrack(
		        raw->cells, raw->bit_count, raw->index, false, raw);
		if (track.sectors.empty()) {
			track.sectors = DecodeTrack(raw->cells,
			                            raw->bit_count,
			                            raw->index,
			                            false,
			                            raw,
			                            true);
		}
		if (track.sectors.empty()) {
			// A container's sampling clock can differ from the
			// field's encoding clock, including 300-kbps DD media
			// captured in a 360-RPM drive. Probe the same rates the
			// FDC can select.
			for (const auto fm : {false, true}) {
				for (const auto rate : {250, 300, 500, 1000}) {
					auto sampled = raw->Sample(
					        0,
					        track.cycle_ms,
					        check_cast<uint16_t>(fm ? rate / 2
					                                : rate));
					if (!sampled) {
						continue;
					}
					track.sectors = DecodeTrack(sampled->cells,
					                            sampled->bit_count,
					                            sampled->index,
					                            false,
					                            sampled,
					                            fm);
					if (!track.sectors.empty()) {
						break;
					}
				}
				if (!track.sectors.empty()) {
					break;
				}
			}
		}
		image->UpdateLogicalGeometry(check_cast<uint32_t>(i / image->heads),
		                             check_cast<uint32_t>(i % image->heads),
		                             track.sectors);
		found_ids |= !track.sectors.empty();
	}
	if (!found_ids) {
		error = "no IBM FM/MFM sector IDs found";
		return {};
	}
	if (!image->OpenOverlay(path, read_only || capture.read_only, fingerprint, error)) {
		return {};
	}
	return image;
}

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::LoadHxcMfm(
        FILE* file, const std::string& path, const bool read_only, std::string& error)
{
	error.clear();
	if (fseek(file, 0, SEEK_END)) {
		error = "cannot seek HxC MFM image";
		return {};
	}
	const auto size = ftell(file);
	if (size < 19 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
		error = "invalid HxC MFM image size";
		return {};
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		error = "cannot read HxC MFM image";
		return {};
	}
	const auto capture = ParseHxcMfmCapture(bytes, error);
	if (!capture) {
		return {};
	}
	return FromBitstreamCapture(
	        *capture, path, read_only, FloppyImageFingerprint(bytes), error);
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
					entry->second = DecodeTrack(raw->cells,
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

std::vector<FloppySector> FloppyTrackImage::SampleTrack(const uint32_t cylinder,
                                                        const uint32_t head,
                                                        const uint64_t cycle,
                                                        const uint16_t rate_kbps,
                                                        const bool fm) const
{
	if (cylinder >= cylinders || head >= heads) {
		return {};
	}
	const auto& track                 = tracks[cylinder * heads + head];
	const auto logical                = GetTrack(cylinder, head);
	std::vector<FloppySector> sectors = {};
	if (track.flux && !track.cells_changed) {
		sectors = DecodeFluxTrack(*track.flux,
		                          track.revolution_ends,
		                          rate_kbps,
		                          fm);
	} else {
		auto raws = track.raw_tracks;
		if (raws.empty()) {
			for (const auto& sector : track.sectors) {
				if (sector.mfm_track &&
				    std::ranges::find(raws, sector.mfm_track) ==
				            raws.end()) {
					raws.push_back(sector.mfm_track);
				}
			}
		}
		if (raws.empty()) {
			return {logical.begin(), logical.end()};
		}
		for (const auto& raw : raws) {
			auto sampled = raw->Sample(cycle,
			                           track.cycle_ms,
			                           fm ? rate_kbps / 2 : rate_kbps);
			if (!sampled || !sampled->bit_count) {
				continue;
			}
			auto fields = DecodeTrack(sampled->cells,
			                          sampled->bit_count,
			                          sampled->index,
			                          false,
			                          sampled,
			                          fm);
			if (!sampled->revolution_ends_bits.empty()) {
				PositionSectors(fields, track.revolution_ends);
			}
			sectors.insert(sectors.end(), fields.begin(), fields.end());
		}
	}
	for (auto& sector : sectors) {
		const auto original = std::ranges::find_if(logical, [&](const auto& other) {
			return other.fm == sector.fm &&
			       other.id_bit == sector.id_bit &&
			       other.revolution == sector.revolution;
		});
		if (original != logical.end()) {
			sector.overlay_slot = original->overlay_slot;
		}
	}
	return sectors;
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
