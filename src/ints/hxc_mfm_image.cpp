// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "ints/bitstream_capture.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

struct MfmBitstream {
	std::span<const uint8_t> bytes = {};
	size_t bit_count               = 0;
};

std::optional<uint64_t> round_nonnegative(const double value)
{
	// llround returns a signed value. Check its domain before rounding,
	// then use the standard range check for the unsigned destination.
	if (!std::isfinite(value) || value < 0 ||
	    value >= static_cast<double>(std::numeric_limits<long long>::max())) {
		return {};
	}
	const auto rounded = std::llround(value);
	if (!std::in_range<uint64_t>(rounded)) {
		return {};
	}
	return static_cast<uint64_t>(rounded);
}

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

// Overlapping IDs can each advertise a whole track's worth of data. Input
// byte limits alone therefore cannot bound the decoded representation.
constexpr size_t MaxDecodedTrackBytes = 8 * 1024 * 1024;
constexpr size_t MaxDecodedImageBytes = 128 * 1024 * 1024;

bool account_decoded_sector(const FloppySector& sector, size_t& used,
                            const size_t limit)
{
	for (const auto size : {sector.data.size(),
	                        sector.extended_data.size(),
	                        sector.weak_mask.size()}) {
		if (size > limit - used) {
			return false;
		}
		used += size;
	}
	return true;
}

std::vector<bool> find_weak_cells(const MfmBitstream track)
{
	// A recovered MFM stream permits three zero cells between transitions.
	// Preserve that leading interval; only the excess cells are ambiguous.
	constexpr size_t StableZeroCells = 3;
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
		if (bit - start > StableZeroCells) {
			for (auto cell = start + StableZeroCells; cell < bit; ++cell) {
				weak[cell % track.bit_count] = true;
			}
		}
		start = bit + 1;
	}
	return weak;
}

} // namespace

namespace {

constexpr size_t MaxFluxCellsPerRevolution = 2'100'000;

std::shared_ptr<FloppyMfmTrack> recover_flux(
        const std::span<const uint64_t> transitions_ns, const uint64_t duration_ns,
        const double nominal_cell_ns, const bool preserve_timing,
        const std::span<const uint64_t> index_ends_ns = {})
{
	if (!duration_ns || !std::isfinite(nominal_cell_ns) ||
	    nominal_cell_ns <= 0 || index_ends_ns.size() > 256) {
		return {};
	}
	const auto max_cells = MaxFluxCellsPerRevolution *
	                       std::max(size_t{1}, index_ends_ns.size());
	std::vector<uint8_t> bitstream = {};
	double clock_ns                = nominal_cell_ns;
	double phase_ns                = 0;
	uint64_t previous_ns           = 0;
	size_t cells_written           = 0;
	std::vector<double> cell_ends  = {};
	uint64_t emitted_ns            = 0;
	std::vector<size_t> index_ends = {};
	const auto append_times        = [&](const size_t count,
	                                     const uint64_t end_ns,
	                                     const size_t first_cell) {
		const auto endpoint = [&](const size_t cell) {
			return static_cast<double>(emitted_ns) +
			       static_cast<double>(end_ns - emitted_ns) *
			               static_cast<double>(cell) /
			               static_cast<double>(count);
		};
		while (index_ends.size() < index_ends_ns.size() &&
		       index_ends_ns[index_ends.size()] <= end_ns) {
			// Assign cells by endpoint even when index bisects an
			// interval. Binary search avoids rounding the two
			// interval halves separately.
			const auto boundary_ns = index_ends_ns[index_ends.size()];
			size_t low = 0;
			auto high  = count;
			while (low < high) {
				const auto middle = low + (high - low + 1) / 2;
				if (endpoint(middle) <=
				    static_cast<double>(boundary_ns)) {
					low = middle;
				} else {
					high = middle - 1;
				}
			}
			index_ends.push_back(first_cell + low);
		}
		if (preserve_timing) {
			for (size_t cell = 1; cell <= count; ++cell) {
				cell_ends.push_back(endpoint(cell) /
				                    static_cast<double>(duration_ns));
			}
		}
		emitted_ns = end_ns;
	};
	for (const auto time_ns : transitions_ns) {
		if (time_ns < previous_ns || time_ns > duration_ns) {
			return {};
		}
		phase_ns += static_cast<double>(time_ns - previous_ns);
		previous_ns      = time_ns;
		const auto count = std::max(0.0,
		                            std::floor(phase_ns / clock_ns + 0.5));
		if (!std::isfinite(count) ||
		    count > static_cast<double>(max_cells - cells_written)) {
			return {};
		}
		const auto cells = static_cast<size_t>(count);
		if (cells == 0) {
			continue;
		}
		append_times(cells, time_ns, cells_written);
		cells_written += cells;
		bitstream.resize((cells_written + 7) / 8);
		const auto bit = cells_written - 1;
		bitstream[bit / 8] |= static_cast<uint8_t>(0x80u >> (bit % 8));
		phase_ns -= static_cast<double>(cells) * clock_ns;
		// Normal FM/MFM spacings can train the oscillator. A longer
		// interval provides no reliable frequency measurement: coast at
		// the acquired clock instead of pulling it towards nominal.
		if (cells <= 4) {
			clock_ns = std::clamp(clock_ns + phase_ns * 0.05,
			                      nominal_cell_ns * 0.9,
			                      nominal_cell_ns * 1.1);
		}
		phase_ns *= 0.4;
	}

	const auto remaining_ns = static_cast<double>(duration_ns - previous_ns);
	const auto tail_count = index_ends_ns.empty()
	                              ? std::round(remaining_ns / clock_ns)
	                              : std::floor(std::max(0.0,
	                                                    (remaining_ns + phase_ns) /
	                                                            clock_ns));
	if (!std::isfinite(tail_count) ||
	    tail_count > static_cast<double>(max_cells - cells_written)) {
		return {};
	}
	const auto tail      = static_cast<size_t>(tail_count);
	const auto bit_count = cells_written + tail;
	if (index_ends_ns.empty()) {
		append_times(tail, duration_ns, cells_written);
	} else {
		// Do not round an unfinished cell into the end of the capture.
		// The next loop's first cell may straddle the physical index.
		if (tail) {
			const auto end_ns = round_nonnegative(
			        static_cast<double>(previous_ns) +
			        static_cast<double>(tail) * clock_ns - phase_ns);
			if (!end_ns) {
				return {};
			}
			append_times(tail, std::min(*end_ns, duration_ns), cells_written);
		}
		append_times(0, duration_ns, bit_count);
	}
	bitstream.resize((bit_count + 7) / 8);
	auto raw       = std::make_shared<FloppyMfmTrack>();
	raw->cells     = std::move(bitstream);
	raw->bit_count = bit_count;
	raw->cell_ends = std::move(cell_ends);
	raw->revolution_ends_bits = std::move(index_ends);
	return raw;
}

} // namespace

std::shared_ptr<FloppyMfmTrack> FloppyMfmTrack::RecoverFlux(
        const std::span<const uint64_t> transitions_ns, const uint64_t duration_ns,
        const double nominal_cell_ns, const bool preserve_timing)
{
	return recover_flux(transitions_ns, duration_ns, nominal_cell_ns, preserve_timing);
}

std::shared_ptr<FloppyMfmTrack> FloppyMfmTrack::RecoverFluxRevolutions(
        const std::span<const uint64_t> transitions_ns,
        const std::span<const uint64_t> index_ends_ns, const double nominal_cell_ns)
{
	if (index_ends_ns.empty() || index_ends_ns.size() > 255 ||
	    !index_ends_ns.front() ||
	    std::adjacent_find(index_ends_ns.begin(),
	                       index_ends_ns.end(),
	                       [](const auto a, const auto b) { return a >= b; }) !=
	            index_ends_ns.end()) {
		return {};
	}
	// Eight encoded bytes provide the 64 data-bit acquisition interval.
	// Use actual recorded history, including any no-flux area, not fake sync.
	const auto acquisition_ns = round_nonnegative(nominal_cell_ns * 128);
	if (!acquisition_ns || !*acquisition_ns) {
		return {};
	}
	const auto duration_ns = index_ends_ns.back();
	const auto last_start  = index_ends_ns.size() > 1
	                               ? index_ends_ns[index_ends_ns.size() - 2]
	                               : 0;
	const auto history_ns = std::min(*acquisition_ns, duration_ns - last_start);
	if (history_ns > UINT64_MAX - duration_ns) {
		return {};
	}
	const auto history_start          = duration_ns - history_ns;
	std::vector<uint64_t> transitions = {};
	for (const auto time_ns : transitions_ns) {
		if (time_ns > history_start && time_ns <= duration_ns) {
			transitions.push_back(time_ns - history_start);
		}
	}
	for (const auto time_ns : transitions_ns) {
		if (time_ns > duration_ns) {
			return {};
		}
		transitions.push_back(time_ns + history_ns);
	}
	std::vector<uint64_t> boundaries = {history_ns};
	for (const auto end_ns : index_ends_ns) {
		boundaries.push_back(end_ns + history_ns);
	}
	auto raw = recover_flux(transitions,
	                        duration_ns + history_ns,
	                        nominal_cell_ns,
	                        false,
	                        boundaries);
	if (!raw || raw->revolution_ends_bits.size() != boundaries.size()) {
		return {};
	}
	const auto discard = raw->revolution_ends_bits.front();
	const auto count   = raw->bit_count - discard;
	std::vector<uint8_t> cells((count + 7) / 8);
	for (size_t bit = 0; bit < count; ++bit) {
		if (bit_at({raw->cells, raw->bit_count}, bit + discard)) {
			cells[bit / 8] |= check_cast<uint8_t>(0x80u >> (bit % 8));
		}
	}
	raw->cells     = std::move(cells);
	raw->bit_count = count;
	raw->revolution_ends_bits.erase(raw->revolution_ends_bits.begin());
	size_t previous = 0;
	for (auto& end : raw->revolution_ends_bits) {
		end -= discard;
		if (end <= previous || end - previous > MaxFluxCellsPerRevolution) {
			return {};
		}
		previous = end;
	}
	return raw;
}

std::shared_ptr<FloppyMfmTrack> FloppyMfmTrack::Sample(const uint64_t cycle,
                                                       const double duration_ms,
                                                       const uint16_t rate_kbps) const
{
	auto sampled = std::make_shared<FloppyMfmTrack>(*this);
	const auto cell_ns = bit_count ? duration_ms * 1'000'000 /
	                                         static_cast<double>(bit_count)
	                               : 0;
	const auto selected_cell_ns = rate_kbps ? 500'000.0 / rate_kbps : 0;
	const auto recover_clock = rate_kbps && bit_count && duration_ms > 0 &&
	                           !(cell_ns >= selected_cell_ns * 0.9 &&
	                             cell_ns <= selected_cell_ns * 1.1);
	// Do not mistake an oversampled legal interval for missing flux. Infer
	// TC ambiguity on the selected grid; explicit surface masks stay on the
	// original grid and retain their existing resampling behavior.
	if (infer_weak_cells && recover_clock) {
		sampled->weak_cells.clear();
	}
	sampled->infer_weak_cells = false;
	for (size_t bit = 0; bit < sampled->weak_cells.size(); ++bit) {
		if (!sampled->weak_cells[bit]) {
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
	const auto duration_ns = round_nonnegative(duration_ms * 1'000'000);
	if (!duration_ns || !*duration_ns) {
		return {};
	}
	// Bitstream captures already contain recovered cells. Preserve their
	// exact positions when the selected separator can lock to that clock.
	if (cell_ns >= selected_cell_ns * 0.9 && cell_ns <= selected_cell_ns * 1.1) {
		return sampled;
	}
	std::vector<uint64_t> transitions = {};
	for (size_t bit = 0; bit < bit_count; ++bit) {
		if (bit_at({sampled->cells, bit_count}, bit)) {
			const auto time_ns = round_nonnegative(
			        cell_ends.empty()
			                ? static_cast<double>(bit + 1) * cell_ns
			                : cell_ends[bit] * static_cast<double>(
			                                           *duration_ns));
			if (!time_ns) {
				return {};
			}
			transitions.push_back(*time_ns);
		}
	}
	auto decoded = RecoverFlux(transitions, *duration_ns, selected_cell_ns, true);
	if (!decoded || !decoded->bit_count) {
		return {};
	}
	const auto position = [&](const size_t bit) -> std::optional<size_t> {
		if (bit > bit_count) {
			return {};
		}
		// Separator acquisition can produce uneven cells. Preserve the
		// physical timestamp of index and each revolution boundary.
		const auto time  = bit == 0 ? 0.0
		                 : cell_ends.empty()
		                         ? static_cast<double>(bit) /
                                                  static_cast<double>(bit_count)
		                         : cell_ends[bit - 1];
		const auto upper = std::lower_bound(decoded->cell_ends.begin(),
		                                    decoded->cell_ends.end(),
		                                    time);
		if (upper == decoded->cell_ends.end()) {
			return decoded->bit_count;
		}
		// lower_bound returns an iterator in this vector, so its
		// nonnegative distance from begin fits size_t.
		const auto lower_index = static_cast<size_t>(
		        upper - decoded->cell_ends.begin());
		const auto lower_time = lower_index
		                              ? decoded->cell_ends[lower_index - 1]
		                              : 0.0;
		return time - lower_time < *upper - time ? lower_index
		                                         : lower_index + 1;
	};
	const auto decoded_index = position(index);
	if (!decoded_index) {
		return {};
	}
	decoded->index = *decoded_index % decoded->bit_count;
	for (const auto end : revolution_ends_bits) {
		const auto decoded_end = position(end);
		if (!decoded_end) {
			return {};
		}
		decoded->revolution_ends_bits.push_back(*decoded_end);
	}
	if (infer_weak_cells) {
		decoded->weak_cells = find_weak_cells(
		        {decoded->cells, decoded->bit_count});
		return decoded->Sample(cycle);
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

void FloppySector::UpdateTrackData(const bool create_data_mark,
                                   const std::optional<uint32_t> write_bits)
{
	if (!mfm_track) {
		return;
	}
	auto& track   = *mfm_track;
	auto position = (data_bit + track.bit_count - 16) % track.bit_count;
	auto remaining = write_bits
	                       ? size_t{*write_bits} +
	                                 (create_data_mark ? (fm ? 6u : 15u) * 16 : 0)
	                       : std::numeric_limits<size_t>::max();
	const auto write_bit = [&](const bool value) {
		if (!remaining) {
			return;
		}
		--remaining;
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
			write_bit(fm || !(previous || value));
			write_bit(value);
			previous = value;
		}
	};
	if (create_data_mark) {
		// Write Data creates the sync field and DAM even if the old
		// track contained only an ID field.
		const auto sync_bits = (fm ? 112u : 256u) % track.bit_count;
		position = (data_bit + track.bit_count - sync_bits) % track.bit_count;
		previous = false;
		for (auto byte = 0; byte < (fm ? 6 : 12); ++byte) {
			append(0);
		}
		for (auto byte = 0; byte < (fm ? 0 : 3); ++byte) {
			for (auto bit = 15; bit >= 0; --bit) {
				write_bit(((0x4489u >> bit) & 1u) != 0);
			}
		}
		previous = true;
	}
	const auto mark = deleted ? uint8_t{0xf8} : uint8_t{0xfb};
	auto crc        = update_crc(fm ? uint16_t{0xffff} : SyncCrc, mark);
	if (fm) {
		// FM address marks carry the missing-clock C7 pattern.
		for (auto bit = 7; bit >= 0; --bit) {
			write_bit(((0xc7u >> bit) & 1u) != 0);
			write_bit(((mark >> bit) & 1u) != 0);
		}
	} else {
		append(mark);
	}
	for (const auto byte : data) {
		append(byte);
		crc = update_crc(crc, byte);
	}
	append(check_cast<uint8_t>(crc >> 8));
	append(static_cast<uint8_t>(crc));
	const MfmBitstream cells = {track.cells, track.bit_count};
	if (!fm && !write_bits) {
		write_bit(!(previous || bit_at(cells, position + 1)));
	}
}

std::vector<FloppySector> FloppyTrackImage::DecodeTrack(
        const std::span<const uint8_t> bytes, const size_t bit_count,
        const size_t index, const bool detect_weak,
        std::shared_ptr<FloppyMfmTrack> captured, const bool fm, bool* exceeded_budget)
{
	if (exceeded_budget) {
		*exceeded_budget = false;
	}
	const auto resource_limit = [&]() -> std::vector<FloppySector> {
		if (exceeded_budget) {
			*exceeded_budget = true;
		}
		return {};
	};
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
	size_t decoded_bytes              = 0;
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
		if (data.type != IdMark) {
			// Read Track uses the command's N, even when the ID's N
			// cannot describe a supported logical sector size.
			const auto field       = data.bit + mark_bits;
			sector.has_data_mark   = true;
			sector.data_bit        = field % track_bits;
			sector.data_delay_bits = (field + track_bits - id.bit) %
			                         track_bits;
			sector.weak_seed = data.bit;
			sector.deleted   = data.type == DeletedMark;
		}
		if (sector.has_data_mark && sector.size_code <= 7) {
			const auto size = size_t{128} << sector.size_code;
			if (size > MaxDecodedTrackBytes - decoded_bytes) {
				return resource_limit();
			}
			// Once a data mark is found, the controller consumes
			// the entire field, including embedded address marks
			// and index crossings.
			const auto field       = data.bit + mark_bits;
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
					if (size > MaxDecodedTrackBytes - decoded_bytes -
					                   sector.data.size()) {
						return resource_limit();
					}
					sector.extended_data.resize(size);
					for (size_t j = 0; j < size; ++j) {
						sector.extended_data[j] = read_byte(
						        track, field + j * ByteBits);
					}
					break;
				}
			}
		}
		if (!account_decoded_sector(sector, decoded_bytes, MaxDecodedTrackBytes)) {
			return resource_limit();
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
	const auto raw = join_revolutions(revolutions);
	auto sectors = DecodeTrack(raw->cells, raw->bit_count, raw->index, false, raw, fm);
	if (revolutions.size() > 1) {
		PositionSectors(sectors, ends);
	}
	return sectors;
}

std::shared_ptr<FloppyMfmTrack> FloppyTrackImage::join_revolutions(
        const std::span<const std::shared_ptr<FloppyMfmTrack>> revolutions)
{
	assert(!revolutions.empty());
	if (revolutions.size() == 1) {
		return revolutions.front();
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
	return raw;
}

void FloppyTrackImage::PositionSectors(std::vector<FloppySector>& sectors,
                                       const std::span<const double> ends)
{
	for (auto& sector : sectors) {
		const auto next = std::upper_bound(
		        sector.mfm_track->revolution_ends_bits.begin(),
		        sector.mfm_track->revolution_ends_bits.end(),
		        sector.id_bit);
		const auto revolution = next -
		                        sector.mfm_track->revolution_ends_bits.begin();
		assert(std::in_range<size_t>(revolution));
		sector.revolution = static_cast<size_t>(revolution);
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

std::shared_ptr<FloppyMfmTrack> FloppyTrackImage::format_track_cells(
        const std::span<const FloppySector> previous, const size_t revolution,
        const double duration_ms, const TrackFormat& format,
        std::shared_ptr<FloppyMfmTrack> source)
{
	if (!source) {
		const auto found = std::ranges::find_if(previous, [&](const auto& sector) {
			return sector.mfm_track &&
			       (sector.revolution == revolution ||
			        sector.mfm_track->revolution_ends_bits.size() >
			                revolution);
		});
		source = found != previous.end() ? found->mfm_track : nullptr;
	}
	const auto source_bits = source ? source->GetRevolutionBitCount(revolution)
	                                : 0;
	const auto source_start = source && !source->revolution_ends_bits.empty() &&
	                                          revolution
	                                ? source->revolution_ends_bits[revolution - 1]
	                                : 0;
	const auto rate = format.rate_kbps ? format.rate_kbps : uint16_t{250};
	const auto rounded_bits = round_nonnegative(duration_ms *
	                                            (format.fm ? 1 : 2) * rate);
	if (!rounded_bits || !std::in_range<size_t>(*rounded_bits)) {
		return {};
	}
	auto bits = static_cast<size_t>(*rounded_bits);
	if (!format.rate_kbps && source) {
		bits = source_bits;
	}
	const auto encoding_bits = bits;
	const auto count         = format.ids.size() / 4;
	const auto partial = count < format.requested || format.written_cells;
	if (partial) {
		// Retain enough resolution for faster fields in the surviving
		// suffix.
		bits = std::max(bits, source_bits);
	}
	if (!bits || !encoding_bits) {
		return {};
	}
	auto captured       = std::make_shared<FloppyMfmTrack>();
	captured->bit_count = bits;
	captured->cells.resize((bits + 7) / 8);
	const auto written_bytes =
	        format.gap
	                ? floppy_format::lead_bytes(format.fm) +
	                          count * floppy_format::sector_bytes(format.size_code,
	                                                              *format.gap,
	                                                              format.fm,
	                                                              format.extended_gap)
	                : 0;
	const auto cutoff = format.gap
	                          ? static_cast<size_t>(std::min<uint64_t>(
	                                    bits,
	                                    uint64_t{format.written_cells.value_or(
	                                            check_cast<uint32_t>(
	                                                    written_bytes * 16))} *
	                                            bits / encoding_bits))
	                          : bits * count / format.requested;
	if (partial && source && !source->weak_cells.empty()) {
		captured->weak_cells.resize(bits);
	}
	// Keep the original suffix on an interrupted format, normalized to index.
	for (size_t bit = 0; bit < bits; ++bit) {
		const auto preserve = partial && source && bit >= cutoff;
		auto value = ((format.fm ? 0xffffu : 0x9254u) >> (15 - bit % 16)) & 1u;
		if (preserve) {
			value = 0;
			// Keep suffix transition times when changing the cell
			// clock.
			const auto begin_bit = uint64_t{bit} * source_bits / bits;
			const auto end_bit = (uint64_t{bit} + 1) * source_bits / bits;
			if (!std::in_range<size_t>(begin_bit) ||
			    !std::in_range<size_t>(end_bit)) {
				return {};
			}
			const auto begin = static_cast<size_t>(begin_bit);
			const auto end   = static_cast<size_t>(end_bit);
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
	const auto scaled_cell = [&](const uint64_t offset) {
		const auto cell = offset * bits / encoding_bits;
		assert(std::in_range<size_t>(cell));
		return static_cast<size_t>(cell);
	};
	const auto write_bit = [&](const bool value) {
		const auto begin = scaled_cell(position);
		const auto end   = scaled_cell(uint64_t{position} + 1);
		for (auto cell = begin;
		     cell < end && cell < bits && (!partial || cell < cutoff);
		     ++cell) {
			const auto mask = check_cast<uint8_t>(0x80u >> (cell % 8));
			auto& byte = captured->cells[cell / 8];
			byte       = check_cast<uint8_t>(
			        (value && cell + 1 == end)
			                ? byte | mask
			                : byte & static_cast<uint8_t>(~mask));
			if (!captured->weak_cells.empty()) {
				captured->weak_cells[cell] = false;
			}
		}
		// A partial field must not overwrite the surviving suffix or
		// wrap back into the committed prefix.
		position = partial || format.gap ? position + 1
		                                 : (position + 1) % encoding_bits;
	};
	const auto append = [&](const uint8_t byte) {
		for (int bit = 7; bit >= 0; --bit) {
			const auto data = ((byte >> bit) & 1) != 0;
			write_bit(format.fm || !(last_data || data));
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
	const auto mark = [&](const uint8_t value) {
		if (format.fm) {
			for (auto byte = 0; byte < 6; ++byte) {
				append(0);
			}
			for (auto bit = 7; bit >= 0; --bit) {
				write_bit(((0xc7u >> bit) & 1u) != 0);
				write_bit(((value >> bit) & 1u) != 0);
			}
		} else {
			sync();
			append(value);
		}
	};
	if (format.gap) {
		for (auto byte = 0; byte < (format.fm ? 40 : 80); ++byte) {
			append(format.fm ? 0xff : 0x4e);
		}
		for (auto byte = 0; byte < (format.fm ? 6 : 12); ++byte) {
			append(0);
		}
		if (format.fm) {
			for (auto bit = 7; bit >= 0; --bit) {
				write_bit(((0xd7u >> bit) & 1u) != 0);
				write_bit(((0xfcu >> bit) & 1u) != 0);
			}
		} else {
			for (auto byte = 0; byte < 3; ++byte) {
				for (auto bit = 15; bit >= 0; --bit) {
					write_bit(((0x5224u >> bit) & 1u) != 0);
				}
			}
			last_data = false;
			append(0xfc);
		}
		for (auto byte = 0; byte < (format.fm ? 26 : 50); ++byte) {
			append(format.fm ? 0xff : 0x4e);
		}
	}
	for (size_t i = 0; i < count; ++i) {
		if (!format.gap) {
			position = i * encoding_bits / format.requested;
		} else if (!format.fm) {
			for (auto byte = 0; byte < 12; ++byte) {
				append(0);
			}
		}
		mark(0xfe);
		const auto seed = format.fm ? uint16_t{0xffff} : SyncCrc;
		auto crc        = update_crc(seed, 0xfe);
		const auto id = std::span(format.ids).subspan(i * 4, 4);
		for (const auto byte : id) {
			append(byte);
			crc = update_crc(crc, byte);
		}
		append_crc(crc);
		for (auto gap = 0; gap < (format.fm             ? 11
		                          : format.extended_gap ? 41
		                                                : 22);
		     ++gap) {
			append(format.fm ? 0xff : 0x4e);
		}
		for (auto gap = 0; gap < (format.fm ? 0 : 12); ++gap) {
			append(0);
		}
		mark(0xfb);
		crc = update_crc(seed, 0xfb);
		// Command N controls the data field; ID N is just an ID byte.
		const auto size_code = format.size_code == 0xff ? id[3]
		                                                : format.size_code;
		for (size_t byte = 0; byte < (size_t{128} << size_code); ++byte) {
			append(format.fill);
			crc = update_crc(crc, format.fill);
		}
		append_crc(crc);
		if (format.gap) {
			for (auto byte = 0; byte < *format.gap; ++byte) {
				append(format.fm ? 0xff : 0x4e);
			}
		}
	}
	if (format.gap && (!partial || format.written_cells)) {
		// Gap 4b runs to index, retaining the preceding data bit for
		// MFM clock generation. Nothing wraps into the first field.
		while (position < encoding_bits) {
			append(format.fm ? 0xff : 0x4e);
		}
	}
	return captured;
}

bool FloppyTrackImage::within_decoded_budget(const std::span<const FloppySector> additional) const
{
	size_t used = 0;
	for (const auto& track : tracks) {
		for (const auto& sector : track.sectors) {
			if (!account_decoded_sector(sector, used, MaxDecodedImageBytes)) {
				return false;
			}
		}
	}
	for (const auto& sector : additional) {
		if (!account_decoded_sector(sector, used, MaxDecodedImageBytes)) {
			return false;
		}
	}
	return true;
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
		raw->infer_weak_cells = captured.detect_weak;
		raw->weak_cells = captured.detect_weak
		                        ? find_weak_cells({raw->cells, raw->bit_count})
		                        : captured.weak_cells;
		track.raw_tracks.push_back(raw);
		bool exceeded_budget      = false;
		const auto resource_limit = [&]() {
			error = "floppy decoded data exceeds resource limits";
		};
		track.sectors = DecodeTrack(raw->cells,
		                            raw->bit_count,
		                            raw->index,
		                            false,
		                            raw,
		                            false,
		                            &exceeded_budget);
		if (exceeded_budget) {
			resource_limit();
			return {};
		}
		if (track.sectors.empty()) {
			track.sectors = DecodeTrack(raw->cells,
			                            raw->bit_count,
			                            raw->index,
			                            false,
			                            raw,
			                            true,
			                            &exceeded_budget);
			if (exceeded_budget) {
				resource_limit();
				return {};
			}
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
					                            fm,
					                            &exceeded_budget);
					if (exceeded_budget) {
						resource_limit();
						return {};
					}
					if (!track.sectors.empty()) {
						break;
					}
				}
				if (!track.sectors.empty()) {
					break;
				}
			}
		}
		for (auto& sector : track.sectors) {
			const auto mark = std::ranges::find(captured.address_marks,
			                                    sector.id_bit,
			                                    &BitstreamCaptureMark::bit);
			if (mark != captured.address_marks.end()) {
				sector.cycle_fraction = mark->cycle_fraction;
				track.recorded_address_times = true;
			}
		}
		// Keep backing-field order stable for existing physical overlay
		// slots.
		if (!image->within_decoded_budget({})) {
			resource_limit();
			return {};
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
		std::map<std::pair<const FloppyMfmTrack*, bool>, std::vector<FloppySector>>
		        decoded = {};
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
				auto [entry, inserted] = decoded.try_emplace(
				        std::pair{raw.get(), original.fm});
				if (inserted) {
					entry->second = DecodeTrack(raw->cells,
					                            raw->bit_count,
					                            raw->index,
					                            false,
					                            raw,
					                            original.fm);
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
			if (track.recorded_address_times) {
				sector.cycle_fraction = original->cycle_fraction;
			}
		}
	}
	if (track.recorded_address_times) {
		std::stable_sort(sectors.begin(),
		                 sectors.end(),
		                 [](const auto& left, const auto& right) {
			                 return left.cycle_fraction <
			                        right.cycle_fraction;
		                 });
	}
	return sectors;
}

double FloppyTrackImage::GetDataRateKbps(const uint32_t cylinder,
                                         const uint32_t head) const
{
	if (cylinder >= cylinders || head >= heads) {
		return 0;
	}
	const auto& track = tracks[cylinder * heads + head];
	for (const auto& sector : GetTrack(cylinder, head)) {
		// A damaged ID CRC does not change the recovered physical clock.
		if (sector.fm || !sector.mfm_track ||
		    sector.revolution >= track.revolution_ends.size()) {
			continue;
		}
		const auto start = sector.revolution
		                         ? track.revolution_ends[sector.revolution - 1]
		                         : 0;
		const auto duration = (track.revolution_ends[sector.revolution] -
		                       start) *
		                      track.cycle_ms;
		if (duration > 0) {
			return static_cast<double>(sector.mfm_track->GetRevolutionBitCount(
			               sector.revolution)) /
			       (2 * duration);
		}
	}
	return 0;
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
