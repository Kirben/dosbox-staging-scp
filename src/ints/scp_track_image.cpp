// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <utility>

#include "ints/scp_capture.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

struct FluxTurn {
	uint64_t duration_ns                 = 0;
	std::vector<uint64_t> transitions_ns = {};
};

std::vector<FluxTurn> split_at_index(const ScpCapture::Header& header,
                                     const ScpCapture::Track& track)
{
	std::vector<FluxTurn> turns = {};
	uint64_t cycle_ns           = 0;
	for (const auto& revolution : track.revolutions) {
		const auto duration_ns = header.Indexed()
		                               ? revolution.IndexDurationNs()
		                               : (header.Rpm() == 360 ? 166666667u
		                                                      : 200000000u);
		if (duration_ns < 50'000'000 || duration_ns > 1'000'000'000) {
			return {};
		}
		turns.push_back({duration_ns, {}});
		cycle_ns += duration_ns;
	}

	const auto unit_ns        = header.FluxUnitNs();
	const auto overflow_ticks = uint64_t{1} << header.cell_width;
	uint64_t elapsed_ns       = 0;
	uint64_t start_ns         = 0;
	uint64_t overflow         = 0;
	size_t turn               = 0;
	// Intervals remain continuous across the stored revolution descriptors.
	for (const auto& revolution : track.revolutions) {
		for (const auto entry : revolution.entries) {
			if (entry == 0) {
				overflow += overflow_ticks;
				continue;
			}
			const auto ticks = overflow + entry;
			overflow         = 0;
			if (ticks >= (cycle_ns - elapsed_ns + unit_ns - 1) / unit_ns) {
				return turns;
			}
			elapsed_ns += ticks * unit_ns;
			while (elapsed_ns >= start_ns + turns[turn].duration_ns) {
				start_ns += turns[turn++].duration_ns;
			}
			turns[turn].transitions_ns.push_back(elapsed_ns - start_ns);
		}
	}
	return turns;
}

std::vector<uint8_t> recover_bitstream(const FluxTurn& turn,
                                       const double nominal_cell_ns)
{
	constexpr size_t MaxCells      = 2'100'000;
	std::vector<uint8_t> bitstream = {};
	double clock_ns                = nominal_cell_ns;
	double phase_ns                = 0;
	uint64_t previous_ns           = 0;
	size_t cells_written           = 0;
	for (const auto time_ns : turn.transitions_ns) {
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
	        static_cast<double>(turn.duration_ns - previous_ns) / clock_ns));
	if (tail > MaxCells - cells_written) {
		return {};
	}
	bitstream.resize((cells_written + tail + 7) / 8);
	return bitstream;
}

} // namespace

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::LoadScp(FILE* file,
                                                            std::string& error)
{
	ScpCapture capture = {};
	if (!capture.Load(file, error)) {
		return {};
	}
	auto image       = std::make_unique<FloppyTrackImage>();
	image->cylinders = capture.header.end_track / 2u + 1;
	image->heads     = 1;
	for (size_t index = 1; index < capture.tracks.size(); index += 2) {
		if (capture.tracks[index]) {
			image->heads = 2;
			break;
		}
	}
	image->tracks.resize(image->cylinders * image->heads);

	constexpr std::array<double, 4> CellTimesNs = {2000, 1666.667, 1000, 500};
	bool found_ids = false;
	for (size_t index = 0; index < capture.tracks.size(); ++index) {
		if (!capture.tracks[index]) {
			continue;
		}
		const auto turns = split_at_index(capture.header,
		                                  *capture.tracks[index]);
		if (turns.empty()) {
			continue;
		}
		auto& track = image->tracks[(index / 2) * image->heads + index % 2];
		track.cycle_ms = 0;
		for (const auto& turn : turns) {
			track.cycle_ms += static_cast<double>(turn.duration_ns) /
			                  1'000'000;
		}

		double elapsed_ms = 0;
		for (const auto& turn : turns) {
			const auto turn_ms = static_cast<double>(turn.duration_ns) /
			                     1'000'000;
			if (turn.transitions_ns.empty()) {
				elapsed_ms += turn_ms;
				continue;
			}
			std::vector<FloppySector> decoded = {};
			int best_score                    = 0;
			for (const auto cell_ns : CellTimesNs) {
				const auto bitstream = recover_bitstream(turn, cell_ns);
				if (bitstream.size() < 16) {
					continue;
				}
				auto candidate = DecodeMfmTrack(bitstream);
				int score      = 0;
				for (const auto& sector : candidate) {
					if (!sector.id_crc_error) {
						score += sector.data.empty() ||
						                         sector.data_crc_error
						               ? 1
						               : 4;
					}
				}
				if (score > best_score ||
				    (decoded.empty() && !candidate.empty())) {
					decoded    = std::move(candidate);
					best_score = score;
				}
			}

			image->UpdateLogicalGeometry(static_cast<uint32_t>(index / 2),
			                             static_cast<uint32_t>(index % 2),
			                             decoded);
			found_ids |= !decoded.empty();
			for (auto& sector : decoded) {
				sector.cycle_fraction = (elapsed_ms +
				                         sector.cycle_fraction *
				                                 turn_ms) /
				                        track.cycle_ms;
			}
			track.sectors.insert(track.sectors.end(),
			                     std::make_move_iterator(decoded.begin()),
			                     std::make_move_iterator(decoded.end()));
			elapsed_ms += turn_ms;
		}
	}
	if (!found_ids) {
		error = "no IBM MFM sector IDs found";
		return {};
	}
	return image;
}
