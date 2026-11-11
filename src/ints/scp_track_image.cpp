// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <optional>
#include <utility>

#include "ints/scp_capture.h"
#include "misc/support.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

struct FluxTurn {
	uint64_t duration_ns                 = 0;
	std::vector<uint64_t> transitions_ns = {};
};

constexpr std::array<uint8_t, 8> OverlayMagic = {'S', 'C', 'P', 'O', 'V', 'L', 1, 0};
constexpr uint8_t TrackFormatMarker           = 0xff;
constexpr uint8_t PartialFormatMarker         = 0xfe;
constexpr size_t ChrnSize                     = 4;

auto overlay_header(const uint64_t fingerprint, const uint8_t version)
{
	std::array<uint8_t, 16> header = {};
	std::copy(OverlayMagic.begin(), OverlayMagic.end(), header.begin());
	header[6] = version;
	for (size_t i = 0; i < sizeof(fingerprint); ++i) {
		header[OverlayMagic.size() + i] = static_cast<uint8_t>(
		        fingerprint >> (8 * i));
	}
	return header;
}

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
                                                            const std::string& path,
                                                            const bool read_only,
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
		track.format_cycle_ms = static_cast<double>(turns.front().duration_ns) /
		                        1'000'000;
		track.cycle_ms = 0;
		track.revolution_ends.clear();
		for (const auto& turn : turns) {
			track.cycle_ms += static_cast<double>(turn.duration_ns) /
			                  1'000'000;
		}

		double elapsed_ms = 0;
		for (size_t revolution = 0; revolution < turns.size(); ++revolution) {
			const auto& turn = turns[revolution];
			const auto turn_ms = static_cast<double>(turn.duration_ns) /
			                     1'000'000;
			track.revolution_ends.push_back((elapsed_ms + turn_ms) /
			                                track.cycle_ms);
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
				sector.revolution     = revolution;
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
	image->overlay_path        = path + ".overlay";
	image->capture_fingerprint = capture.fingerprint;
	image->overlay_writable    = !read_only;
	if (!image->LoadScpOverlay()) {
		error = "invalid SCP overlay or overlay belongs to another capture";
		return {};
	}
	return image;
}

bool FloppyTrackImage::IsWriteProtected() const
{
	return !overlay_writable;
}

bool FloppyTrackImage::CanOverlaySector(const OverlayKey& key) const
{
	const auto [cylinder, head, id]       = key;
	std::optional<size_t> last_revolution = {};
	for (const auto& sector : GetTrack(cylinder, head)) {
		if (sector.cylinder != cylinder || sector.head != head ||
		    sector.id != id || sector.size_code != 2 || sector.id_crc_error) {
			continue;
		}
		// A CHS write cannot distinguish duplicate IDs within a
		// revolution.
		if (last_revolution == sector.revolution) {
			return false;
		}
		last_revolution = sector.revolution;
	}
	return last_revolution.has_value();
}

void FloppyTrackImage::ApplyOverlaySector(const OverlayKey& key,
                                          const OverlaySector& data)
{
	const auto [cylinder, head, id] = key;
	for (auto& sector : tracks[cylinder * heads + head].sectors) {
		if (sector.cylinder == cylinder && sector.head == head &&
		    sector.id == id && sector.size_code == 2 &&
		    !sector.id_crc_error) {
			sector.data.assign(data.begin(), data.end());
			sector.data_crc_error = false;
			sector.deleted        = false;
		}
	}
}

bool FloppyTrackImage::LoadScpOverlay()
{
	std::error_code error = {};
	if (!std::filesystem::exists(overlay_path, error)) {
		return !error;
	}
	const auto close_file = [](FILE* file) { fclose(file); };
	const std::unique_ptr<FILE, decltype(close_file)> input(
	        fopen(overlay_path.c_str(), "rb"), close_file);
	std::array<uint8_t, 16> header = {};
	if (!input ||
	    fread(header.data(), 1, header.size(), input.get()) != header.size() ||
	    (header != overlay_header(capture_fingerprint, 1) &&
	     header != overlay_header(capture_fingerprint, 2) &&
	     header != overlay_header(capture_fingerprint, 3))) {
		return false;
	}
	OverlayKey key  = {};
	auto bytes_read = fread(key.data(), 1, key.size(), input.get());
	while (bytes_read == key.size()) {
		const auto partial = header[6] == 3 && key[1] == PartialFormatMarker;
		if (partial || (header[6] >= 2 && key[1] == TrackFormatMarker)) {
			const TrackAddress address = {key[0], key[2]};
			const auto count           = fgetc(input.get());
			const auto fill            = fgetc(input.get());
			const auto requested = partial ? fgetc(input.get()) : count;
			if (count <= 0 || fill == EOF || requested < count ||
			    (partial && requested == count) ||
			    (header[6] == 2 && !overlay.empty())) {
				return false;
			}
			TrackFormat format = {std::vector<uint8_t>(
			                              static_cast<size_t>(count) *
			                              ChrnSize),
			                      check_cast<uint8_t>(fill),
			                      check_cast<uint8_t>(requested)};
			if (fread(format.ids.data(), 1, format.ids.size(), input.get()) !=
			            format.ids.size() ||
			    !IsValidTrackFormat(address, format.ids)) {
				return false;
			}
			auto& history = track_formats[address];
			if (!partial && !history.empty()) {
				return false;
			}
			auto prior = TakeTrackWrites(overlay, address);
			if (partial) {
				format.prior_writes = std::move(prior);
			}
			ApplyTrackFormat(address, format);
			history.push_back(std::move(format));
		} else {
			OverlaySector data = {};
			if (!CanOverlaySector(key) ||
			    fread(data.data(), 1, data.size(), input.get()) !=
			            data.size() ||
			    !overlay.emplace(key, data).second) {
				return false;
			}
			ApplyOverlaySector(key, data);
		}
		bytes_read = fread(key.data(), 1, key.size(), input.get());
	}
	if (ferror(input.get()) || !feof(input.get()) || bytes_read != 0) {
		return false;
	}
	return true;
}

bool FloppyTrackImage::SaveScpOverlay(const Overlay& entries,
                                      const TrackFormats& formats) const
{
	const auto temporary = overlay_path + ".tmp";
	auto* file           = fopen(temporary.c_str(), "wbx");
	if (!file) {
		return false;
	}
	const auto has_partial =
	        std::any_of(formats.begin(), formats.end(), [](const auto& entry) {
		        return std::any_of(entry.second.begin(),
		                           entry.second.end(),
		                           [](const auto& format) {
			                           return format.ids.size() / ChrnSize <
			                                  format.requested;
		                           });
	        });
	const auto header = overlay_header(capture_fingerprint,
	                                   has_partial ? 3
	                                               : (formats.empty() ? 1 : 2));
	auto success = fwrite(header.data(), 1, header.size(), file) ==
	               header.size();
	const auto write_sectors = [&](const Overlay& writes) {
		for (const auto& [key, data] : writes) {
			success = success &&
			          fwrite(key.data(), 1, key.size(), file) ==
			                  key.size() &&
			          fwrite(data.data(), 1, data.size(), file) ==
			                  data.size();
		}
	};
	for (const auto& [address, history] : formats) {
		for (const auto& format : history) {
			write_sectors(format.prior_writes);
			const auto count = check_cast<uint8_t>(
			        format.ids.size() / ChrnSize);
			const auto partial      = count < format.requested;
			const std::array record = {address[0],
			                           partial ? PartialFormatMarker
			                                   : TrackFormatMarker,
			                           address[1],
			                           count,
			                           format.fill};
			success = success &&
			          fwrite(record.data(), 1, record.size(), file) ==
			                  record.size() &&
			          (!partial || fputc(format.requested, file) != EOF) &&
			          fwrite(format.ids.data(), 1, format.ids.size(), file) ==
			                  format.ids.size();
		}
	}
	write_sectors(entries);
	success               = fclose(file) == 0 && success;
	std::error_code error = {};
	if (success) {
		std::filesystem::rename(temporary, overlay_path, error);
		if (!error) {
			return true;
		}
	}
	std::filesystem::remove(temporary, error);
	return false;
}

bool FloppyTrackImage::WriteScpSector(const uint32_t cylinder,
                                      const uint32_t head, const uint32_t sector,
                                      const std::span<const uint8_t> data)
{
	if (IsWriteProtected() || cylinder >= cylinders || head >= heads ||
	    sector > UINT8_MAX || data.size() != OverlaySector{}.size()) {
		return false;
	}
	const OverlayKey key = {check_cast<uint8_t>(cylinder),
	                        check_cast<uint8_t>(head),
	                        check_cast<uint8_t>(sector)};
	if (!CanOverlaySector(key)) {
		return false;
	}
	auto entries  = overlay;
	auto& updated = entries[key];
	std::copy(data.begin(), data.end(), updated.begin());
	if (!SaveScpOverlay(entries, track_formats)) {
		return false;
	}
	ApplyOverlaySector(key, updated);
	overlay.swap(entries);
	return true;
}

bool FloppyTrackImage::IsValidTrackFormat(const TrackAddress& address,
                                          const std::span<const uint8_t> ids) const
{
	if (address[0] >= cylinders || address[1] >= heads || ids.empty() ||
	    ids.size() % ChrnSize != 0 || ids.size() / ChrnSize > UINT8_MAX) {
		return false;
	}
	// Bound the decoded data that a single formatted track can allocate.
	constexpr size_t MaxTrackDataBytes = 64 * 1024;
	size_t data_bytes                  = 0;
	for (size_t offset = 0; offset < ids.size(); offset += ChrnSize) {
		const auto size_code = ids[offset + 3];
		if (size_code > 7) {
			return false;
		}
		data_bytes += size_t{128} << size_code;
	}
	return data_bytes <= MaxTrackDataBytes;
}

FloppyTrackImage::Overlay FloppyTrackImage::TakeTrackWrites(Overlay& entries,
                                                            const TrackAddress& address)
{
	Overlay writes = {};
	auto it        = entries.lower_bound({address[0], address[1], 0});
	while (it != entries.end() && it->first[0] == address[0] &&
	       it->first[1] == address[1]) {
		writes.insert(entries.extract(it++));
	}
	return writes;
}

void FloppyTrackImage::ApplyTrackFormat(const TrackAddress& address,
                                        const TrackFormat& format)
{
	auto& track = tracks[address[0] * heads + address[1]];
	const auto count   = format.ids.size() / ChrnSize;
	const auto partial = count < format.requested;
	if (!partial) {
		track.sectors.clear();
		track.cycle_ms        = track.format_cycle_ms;
		track.revolution_ends = {1.0};
	}

	// Use the requested sector count to place the written prefix. Keep the
	// original index timing and sector positions past the cutoff on each turn.
	const auto written_fraction = static_cast<double>(count) / format.requested;
	double start = 0;
	for (size_t revolution = 0; revolution < track.revolution_ends.size();
	     ++revolution) {
		const auto end      = track.revolution_ends[revolution];
		const auto duration = end - start;
		if (partial) {
			const auto cutoff = start + duration * written_fraction;
			std::erase_if(track.sectors, [&](const auto& sector) {
				return sector.revolution == revolution &&
				       sector.cycle_fraction < cutoff;
			});
		}
		for (size_t i = 0; i < count; ++i) {
			const auto offset   = i * ChrnSize;
			FloppySector sector = {};
			sector.cylinder     = format.ids[offset];
			sector.head         = format.ids[offset + 1];
			sector.id           = format.ids[offset + 2];
			sector.size_code    = format.ids[offset + 3];
			sector.revolution   = revolution;
			sector.cycle_fraction = start +
			                        duration * static_cast<double>(i) /
			                                format.requested;
			sector.data.assign(size_t{128} << sector.size_code,
			                   format.fill);
			track.sectors.push_back(std::move(sector));
		}
		start = end;
	}
	std::stable_sort(track.sectors.begin(),
	                 track.sectors.end(),
	                 [](const auto& a, const auto& b) {
		                 return a.cycle_fraction < b.cycle_fraction;
	                 });
}

FloppyTrackImage::FormatResult FloppyTrackImage::FormatScpTrack(
        const uint8_t cylinder, const uint8_t head,
        const std::span<const uint8_t> ids, const uint8_t fill,
        const uint8_t requested)
{
	if (IsWriteProtected()) {
		return FormatResult::WriteError;
	}
	const TrackAddress address = {cylinder, head};
	const auto count           = ids.size() / ChrnSize;
	if (count > requested || !IsValidTrackFormat(address, ids)) {
		return FormatResult::InvalidLayout;
	}
	auto entries = overlay;
	auto prior    = TakeTrackWrites(entries, address);
	auto formats  = track_formats;
	auto& history = formats[address];
	if (count == requested) {
		history.clear();
		prior.clear();
	}
	history.push_back({
	        {ids.begin(), ids.end()},
                fill, requested, std::move(prior)
        });
	if (!SaveScpOverlay(entries, formats)) {
		return FormatResult::WriteError;
	}
	ApplyTrackFormat(address, history.back());
	overlay.swap(entries);
	track_formats.swap(formats);
	return FormatResult::Success;
}
