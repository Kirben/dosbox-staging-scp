// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <utility>

#ifdef _WIN32
#include <share.h>
#else
#include <sys/file.h>
#endif

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
constexpr uint8_t TrackFormatMarker         = 0xff;
constexpr uint8_t PartialFormatMarker       = 0xfe;
constexpr uint8_t PhysicalWriteMarker       = 0xfd;
constexpr uint8_t LegacyOverlayVersion      = 4;
constexpr uint8_t SizedOverlayVersion       = 5;
constexpr uint8_t OrderedOverlayVersion     = 6;
constexpr uint8_t FormatSizeOverlayVersion  = 7;
constexpr uint8_t FormatRateOverlayVersion  = 8;
constexpr uint8_t DeletedMarkOverlayVersion = 9;
constexpr uint8_t EncodingOverlayVersion      = 10;
constexpr uint8_t GapOverlayVersion           = 11;
constexpr uint8_t PartialWriteOverlayVersion  = 12;
constexpr uint8_t FormatCutoffOverlayVersion  = 13;
constexpr uint32_t LegacySizeCode           = 2;
constexpr size_t ChrnSize                   = 4;

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
			if (ticks > (cycle_ns - elapsed_ns) / unit_ns) {
				return turns;
			}
			elapsed_ns += ticks * unit_ns;
			// A transition at index closes the preceding cell. Keep it
			// at that turn's end rather than dropping a zero-time pulse.
			while (elapsed_ns > start_ns + turns[turn].duration_ns) {
				start_ns += turns[turn++].duration_ns;
			}
			turns[turn].transitions_ns.push_back(elapsed_ns - start_ns);
		}
	}
	return turns;
}

} // namespace

struct FloppyTrackImage::FluxTrack {
	std::vector<uint64_t> transitions_ns = {};
	std::vector<uint64_t> index_ends_ns  = {};
	double cell_ns              = 2000;
};

std::vector<FloppySector> FloppyTrackImage::DecodeFluxTrack(
        const FluxTrack& flux, const std::span<const double> ends,
        const uint16_t rate_kbps, const bool fm)
{
	const auto raw = FloppyMfmTrack::RecoverFluxRevolutions(
	        flux.transitions_ns,
	        flux.index_ends_ns,
	        rate_kbps ? (fm ? 1'000'000.0 : 500'000.0) / rate_kbps : flux.cell_ns);
	if (!raw || !raw->bit_count) {
		return {};
	}
	auto sectors = DecodeTrack(raw->cells, raw->bit_count, 0, false, raw, fm);
	if (ends.size() > 1) {
		PositionSectors(sectors, ends);
	}
	return sectors;
}

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

	constexpr std::array<double, 6> CellTimesNs = {
	        2000, 1666.667, 1000, 500, 4000, 3333.333};
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
		auto flux          = std::make_shared<FluxTrack>();
		uint64_t offset_ns = 0;
		for (const auto& turn : turns) {
			for (const auto time_ns : turn.transitions_ns) {
				flux->transitions_ns.push_back(offset_ns + time_ns);
			}
			offset_ns += turn.duration_ns;
			flux->index_ends_ns.push_back(offset_ns);
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
		for (const auto& turn : turns) {
			elapsed_ms += static_cast<double>(turn.duration_ns) / 1'000'000;
			track.revolution_ends.push_back(elapsed_ms / track.cycle_ms);
		}
		if (std::ranges::all_of(turns, [](const auto& turn) {
			    return turn.transitions_ns.empty();
		    })) {
			continue;
		}
		std::vector<FloppySector> decoded = {};
		int best_score                    = 0;
		double best_cell_ns               = CellTimesNs.front();
		for (const auto cell_ns : CellTimesNs) {
			// Preserve a valid MFM track as the logical sector view.
			if (cell_ns > 2000 && best_score > 0) {
				continue;
			}
			const auto raw = FloppyMfmTrack::RecoverFluxRevolutions(
			        flux->transitions_ns, flux->index_ends_ns, cell_ns);
			if (!raw || !raw->bit_count) {
				continue;
			}
			// Index does not restart the data separator. A field
			// can continue into the following captured turn.
			for (const auto fm : {false, true}) {
				if (fm && best_score > 0) {
					continue;
				}
				bool exceeded_budget = false;
				auto candidate       = DecodeTrack(raw->cells,
                                                             raw->bit_count,
                                                             0,
                                                             false,
                                                             raw,
                                                             fm,
                                                             &exceeded_budget);
				if (exceeded_budget) {
					error = "SCP decoded track exceeds resource limits";
					return {};
				}
				if (turns.size() > 1) {
					PositionSectors(candidate,
					                track.revolution_ends);
				}
				int score = 0;
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
					decoded      = std::move(candidate);
					best_score   = score;
					best_cell_ns = cell_ns;
				}
			}
		}
		if (!image->within_decoded_budget(decoded)) {
			error = "SCP decoded image exceeds resource limits";
			return {};
		}
		image->UpdateLogicalGeometry(static_cast<uint32_t>(index / 2),
		                             static_cast<uint32_t>(index % 2),
		                             decoded);
		found_ids |= !decoded.empty();
		flux->cell_ns = best_cell_ns;
		track.flux    = std::move(flux);
		track.sectors = std::move(decoded);
	}
	if (!found_ids) {
		error = "no IBM FM/MFM sector IDs found";
		return {};
	}
	if (!image->OpenOverlay(path, read_only, capture.fingerprint, error)) {
		return {};
	}
	return image;
}

bool FloppyTrackImage::OpenOverlay(const std::string& path, const bool read_only,
                                   const uint64_t fingerprint, std::string& error)
{
	overlay_path        = path + ".overlay";
	capture_fingerprint = fingerprint;
	overlay_writable    = !read_only;
	if (overlay_writable) {
		// Keep the lock file so every writer locks the same file, even
		// while the overlay itself is replaced.
		const auto lock_path = overlay_path + ".lock";
#ifdef _WIN32
		overlay_lock.reset(_fsopen(lock_path.c_str(), "ab+", _SH_DENYWR));
		const auto locked = bool(overlay_lock);
#else
		overlay_lock.reset(fopen(lock_path.c_str(), "ab+"));
		const auto locked = overlay_lock &&
		                    flock(fileno(overlay_lock.get()),
		                          LOCK_EX | LOCK_NB) == 0;
#endif
		if (!locked) {
			error = "cannot lock floppy overlay for writing";
			return false;
		}
	}
	if (!LoadOverlay()) {
		error = "invalid floppy overlay or overlay belongs to another image";
		return false;
	}
	return true;
}

std::span<const double> FloppyTrackImage::GetRevolutionEnds(const uint32_t cylinder,
                                                            const uint32_t head) const
{
	return tracks.at(cylinder * heads + head).revolution_ends;
}

double FloppyTrackImage::GetFormatCycleMs(const uint32_t cylinder,
                                          const uint32_t head) const
{
	return tracks.at(cylinder * heads + head).format_cycle_ms;
}

bool FloppyTrackImage::SupportsTrackWrites() const
{
	return !overlay_path.empty();
}

bool FloppyTrackImage::IsWriteProtected() const
{
	return !overlay_writable;
}

void FloppyTrackImage::ensure_encoding_fields(const uint32_t cylinder,
                                              const uint32_t head, const bool fm)
{
	if (cylinder >= cylinders || head >= heads) {
		return;
	}
	auto& track = tracks[cylinder * heads + head];
	if (track.encoding_fields_ready[fm]) {
		return;
	}
	// Preserve the original waveform as the controller's sampling source.
	// Additional separator clocks supply stable addresses, not extra disks.
	if (track.raw_tracks.empty()) {
		if (track.flux) {
			// A logical view at a slower clock can omit faster
			// fields. Keep transitions on the finest supported
			// clock grid before an overlay replaces flux-based
			// command sampling.
			const auto& flux = *track.flux;
			auto raw = FloppyMfmTrack::RecoverFluxRevolutions(
			        flux.transitions_ns, flux.index_ends_ns, 500);
			if (!raw || !raw->bit_count) {
				return;
			}
			track.raw_tracks.push_back(std::move(raw));
		} else {
			for (const auto& field : track.sectors) {
				if (field.mfm_track &&
				    std::ranges::find(track.raw_tracks,
				                      field.mfm_track) ==
				            track.raw_tracks.end()) {
					track.raw_tracks.push_back(field.mfm_track);
				}
			}
		}
	}
	std::vector<FloppySector> added = {};
	for (const auto rate : {250, 300, 500, 1000}) {
		std::vector<FloppySector> fields = {};
		for (const auto& raw : track.raw_tracks) {
			const auto sampled = raw->Sample(
			        0,
			        track.cycle_ms,
			        check_cast<uint16_t>(fm ? rate / 2 : rate));
			if (!sampled || !sampled->bit_count) {
				continue;
			}
			auto decoded = DecodeTrack(sampled->cells,
			                           sampled->bit_count,
			                           sampled->index,
			                           false,
			                           sampled,
			                           fm);
			if (!sampled->revolution_ends_bits.empty()) {
				PositionSectors(decoded, track.revolution_ends);
			}
			fields.insert(fields.end(), decoded.begin(), decoded.end());
		}
		for (auto& field : fields) {
			if (field.id_crc_error) {
				continue;
			}
			const auto same = [&](const auto& other) {
				return other.fm == fm &&
				       other.cylinder == field.cylinder &&
				       other.head == field.head &&
				       other.id == field.id &&
				       other.size_code == field.size_code &&
				       other.revolution == field.revolution &&
				       std::abs(other.cycle_fraction -
				                field.cycle_fraction) < 0.00002;
			};
			if (!std::ranges::any_of(track.sectors, same) &&
			    !std::ranges::any_of(added, same)) {
				added.push_back(std::move(field));
			}
		}
	}
	if (!within_decoded_budget(added)) {
		return;
	}
	track.sectors.insert(track.sectors.end(), added.begin(), added.end());
	track.encoding_fields_ready[fm] = true;
	track.dirty                     = true;
}

void FloppyTrackImage::copy_track_write(const uint32_t cylinder, const uint32_t head,
                                        const FloppySector& sector,
                                        const bool create_data_mark,
                                        const std::optional<uint32_t> write_bits)
{
	const auto source = sector.mfm_track;
	if (!source || !source->bit_count) {
		return;
	}
	auto& track = tracks[cylinder * heads + head];
	auto copies = track.raw_tracks;
	for (const auto& field : track.sectors) {
		if (field.mfm_track &&
		    std::ranges::find(copies, field.mfm_track) == copies.end()) {
			copies.push_back(field.mfm_track);
		}
	}
	const auto sync_bytes = create_data_mark ? (sector.fm ? 6u : 15u) : 0u;
	const auto bits       = source->bit_count;
	const auto start = (sector.data_bit + bits -
	                    ((sync_bytes + 1) * 16 % bits) + bits - source->index) %
	                   bits;
	const auto length = write_bits
	                          ? size_t{*write_bits} + sync_bytes * 16
	                          : (sector.data.size() + sync_bytes + 3) * 16 +
	                                    (sector.fm ? 0 : 1);
	const auto boundary = [](const FloppyMfmTrack& raw, const size_t cell) {
		return !cell ? 0.0
		     : raw.cell_ends.empty()
		             ? static_cast<double>(cell) /
		                       static_cast<double>(raw.bit_count)
		             : raw.cell_ends[cell - 1];
	};
	const auto phase = [&](const FloppyMfmTrack& raw, const size_t cell) {
		const auto position = boundary(raw, cell) - boundary(raw, raw.index);
		return position < 0 ? position + 1 : position;
	};
	const auto first       = (start + source->index) % bits;
	const auto start_phase = phase(*source, first);
	const auto end_phase   = phase(*source, (first + length) % bits);
	const auto duration    = end_phase >= start_phase
	                               ? end_phase - start_phase
	                               : end_phase + 1 - start_phase;
	for (const auto& destination : copies) {
		if (destination == source || !destination->bit_count) {
			continue;
		}
		// Copy only the written arc between clock grids. Faster fields
		// in a mixed-encoding track must retain their original
		// transitions.
		for (size_t cell = 0; cell < destination->bit_count; ++cell) {
			const auto position = phase(*destination, cell + 1);
			const auto offset = position >= start_phase
			                          ? position - start_phase
			                          : position + 1 - start_phase;
			if (length < bits && (offset <= 0 || offset > duration)) {
				continue;
			}
			const auto mask = check_cast<uint8_t>(0x80u >> (cell % 8));
			auto& byte = destination->cells[cell / 8];
			byte &= static_cast<uint8_t>(~mask);
			if (!destination->weak_cells.empty()) {
				destination->weak_cells[cell] = false;
			}
		}
		for (size_t offset = 0; offset < std::min(length, bits); ++offset) {
			const auto cell = (first + offset) % bits;
			if (!(source->cells[cell / 8] & (0x80u >> (cell % 8)))) {
				continue;
			}
			auto position = phase(*source, cell + 1) +
			                boundary(*destination, destination->index);
			if (position > 1) {
				position -= 1;
			}
			size_t found = 0;
			if (destination->cell_ends.empty()) {
				const auto last = destination->bit_count - 1;
				// Keep an exact cell boundary in the preceding
				// cell, allowing for floating-point roundoff.
				const auto end =
				        std::ceil(position * static_cast<double>(
				                                     destination->bit_count) -
				                  1e-8) -
				        1;
				found = static_cast<size_t>(std::clamp(
				        end, 0.0, static_cast<double>(last)));
			} else {
				const auto next = std::ranges::lower_bound(
				        destination->cell_ends, position - 1e-12);
				found = std::min(destination->bit_count - 1,
				                 static_cast<size_t>(
				                         next -
				                         destination->cell_ends.begin()));
			}
			destination->cells[found / 8] |= check_cast<uint8_t>(
			        0x80u >> (found % 8));
		}
	}
}

bool FloppyTrackImage::CanOverlaySector(const OverlayKey& key, const bool fm) const
{
	const auto [cylinder, head, id, slot, size_code] = key;
	if (slot) {
		if (cylinder >= cylinders || head >= heads) {
			return false;
		}
		const auto& track = tracks[cylinder * heads + head].sectors;
		// A physical record can replay a write selected on another
		// captured turn, where the same ID had a good CRC.
		return slot <= track.size() &&
		       track[slot - 1].size_code == size_code &&
		       track[slot - 1].fm == fm;
	}
	const auto track                      = GetTrack(cylinder, head);
	std::optional<size_t> last_revolution = {};
	auto has_good_id                      = false;
	for (const auto& sector : track) {
		if (sector.cylinder != cylinder || sector.head != head ||
		    sector.id != id || sector.size_code != size_code ||
		    sector.fm != fm) {
			continue;
		}
		// A CHS write cannot distinguish duplicate IDs within a
		// revolution.
		if (last_revolution == sector.revolution) {
			return false;
		}
		last_revolution = sector.revolution;
		has_good_id |= !sector.id_crc_error;
	}
	return has_good_id;
}

void FloppyTrackImage::ApplyOverlaySector(const OverlayKey& key,
                                          const OverlaySector& data)
{
	const auto [cylinder, head, id, slot, size_code] = key;
	auto& sectors = tracks[cylinder * heads + head].sectors;
	tracks[cylinder * heads + head].dirty = true;
	const auto apply                      = [&](FloppySector& sector) {
		// Write Data emits the sync and address mark as well as the
		// payload. Recreate their phase when copying between clock grids.
		const auto create_data_mark = true;
                tracks[cylinder * heads + head].cells_changed |= bool(sector.mfm_track);
		if (data.write_bits) {
			auto written    = sector;
			written.data    = data.bytes;
			written.deleted = data.deleted;
			written.UpdateTrackData(create_data_mark, data.write_bits);
			copy_track_write(cylinder,
			                 head,
			                 written,
			                 create_data_mark,
			                 data.write_bits);
			return;
		}
		sector.data           = data.bytes;
		sector.data_crc_error = false;
		sector.deleted        = data.deleted;
                sector.weak_mask.clear();
                sector.extended_data.clear();
                sector.UpdateTrackData(create_data_mark);
		copy_track_write(cylinder, head, sector, create_data_mark);
	};
	if (slot) {
		apply(sectors[slot - 1]);
		return;
	}
	for (auto& sector : sectors) {
		if (sector.cylinder == cylinder && sector.head == head &&
		    sector.id == id && sector.size_code == size_code &&
		    sector.fm == data.fm) {
			// A successful write changes the same physical data arc
			// on every turn, even when another turn has a weak ID
			// CRC.
			apply(sector);
		}
	}
}

bool FloppyTrackImage::LoadOverlay()
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
	     header != overlay_header(capture_fingerprint, 3) &&
	     header != overlay_header(capture_fingerprint, LegacyOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, SizedOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, OrderedOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, FormatSizeOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, FormatRateOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, DeletedMarkOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, EncodingOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, GapOverlayVersion) &&
	     header != overlay_header(capture_fingerprint, PartialWriteOverlayVersion) &&
	     header != overlay_header(capture_fingerprint,
	                              FormatCutoffOverlayVersion))) {
		return false;
	}
	const auto version         = header[6];
	std::array<uint8_t, 3> key = {};
	auto bytes_read = fread(key.data(), 1, key.size(), input.get());
	while (bytes_read == key.size()) {
		const auto partial = version >= 3 && key[1] == PartialFormatMarker;
		if (partial || (version >= 2 && key[1] == TrackFormatMarker)) {
			const TrackAddress address = {key[0], key[2]};
			const auto count           = fgetc(input.get());
			const auto fill            = fgetc(input.get());
			const auto requested = partial ? fgetc(input.get()) : count;
			const auto size_code = version >= FormatSizeOverlayVersion
			                             ? fgetc(input.get())
			                             : 0xff;
			const auto rate_low = version >= FormatRateOverlayVersion
			                            ? fgetc(input.get())
			                            : 0;
			const auto rate_high = version >= FormatRateOverlayVersion
			                             ? fgetc(input.get())
			                             : 0;
			if (rate_low == EOF || rate_high == EOF) {
				return false;
			}
			const auto rate_kbps = check_cast<uint16_t>(
			        rate_low | (rate_high << 8));
			const auto encoding = version >= EncodingOverlayVersion
			                            ? fgetc(input.get())
			                            : 0;
			const auto gap      = version >= GapOverlayVersion
			                            ? fgetc(input.get())
			                            : 0;
			uint32_t written_cells = 0;
			if (version >= FormatCutoffOverlayVersion) {
				for (auto byte = 0; byte < 4; ++byte) {
					const auto value = fgetc(input.get());
					if (value == EOF) {
						return false;
					}
					written_cells |= check_cast<uint32_t>(value)
					              << (byte * 8);
				}
			}
			const auto has_gap  = version >= GapOverlayVersion &&
			                      (encoding & 2);
			if (count <= 0 || fill == EOF || requested < count ||
			    encoding < 0 ||
			    encoding > (version >= GapOverlayVersion ? 3 : 1) ||
			    gap == EOF || (!has_gap && gap != 0) ||
			    (has_gap && (size_code == 0xff || !rate_kbps)) ||
			    (encoding && !rate_kbps) ||
			    (rate_kbps && !IsFloppyDataRate(rate_kbps)) ||
			    (size_code != 0xff && (size_code < 0 || size_code > 7)) ||
			    (size_code != 0xff &&
			     (size_t{128} << size_code) * count > 64 * 1024) ||
			    (partial && requested == count && !written_cells) ||
			    (written_cells && (!partial || !has_gap)) ||
			    (version == 2 && !overlay.empty())) {
				return false;
			}
			TrackFormat format = {std::vector<uint8_t>(
			                              static_cast<size_t>(count) *
			                              ChrnSize),
			                      check_cast<uint8_t>(fill),
			                      check_cast<uint8_t>(requested)};
			format.size_code   = check_cast<uint8_t>(size_code);
			format.rate_kbps   = rate_kbps;
			format.fm          = (encoding & 1) != 0;
			if (written_cells) {
				format.written_cells = written_cells;
			}
			if (has_gap) {
				format.gap = check_cast<uint8_t>(gap);
			}
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
			OverlayKey address = {key[0], key[1], key[2], 0};
			if (version >= LegacyOverlayVersion &&
			    key[1] == PhysicalWriteMarker) {
				std::array<uint8_t, 4> slot_bytes = {};
				if (fread(slot_bytes.data(),
				          1,
				          slot_bytes.size(),
				          input.get()) != slot_bytes.size()) {
					return false;
				}
				uint32_t slot = 0;
				for (size_t i = 0; i < slot_bytes.size(); ++i) {
					slot |= uint32_t{slot_bytes[i]} << (i * 8);
				}
				if (!slot) {
					return false;
				}
				address = {key[0], key[2], 0, slot};
			}
			// Version 5 adds a size code after the logical or
			// physical address.
			const auto size_code = version >= SizedOverlayVersion
			                             ? fgetc(input.get())
			                             : check_cast<int>(LegacySizeCode);
			if (!IsFloppyOverlaySizeCode(size_code) &&
			    !(size_code == 0 && version >= EncodingOverlayVersion)) {
				return false;
			}
			address.back() = check_cast<uint32_t>(size_code);
			// Version 9 stores a normal/deleted mark flag before
			// the payload.
			const auto mark = version >= DeletedMarkOverlayVersion
			                        ? fgetc(input.get())
			                        : 0;
			if (mark < 0 ||
			    mark > (version >= PartialWriteOverlayVersion ? 7
			            : version >= EncodingOverlayVersion   ? 3
			                                                : 1)) {
				return false;
			}
			const auto fm = (mark & 2) != 0;
			ensure_encoding_fields(address[0], address[1], fm);
			if ((!size_code && !fm) || !CanOverlaySector(address, fm)) {
				return false;
			}
			const auto size = size_t{128} << size_code;
			OverlaySector data  = {std::vector<uint8_t>(size),
			                       (mark & 1) != 0,
			                       fm};
			if (mark & 4) {
				std::array<uint8_t, 4> cells = {};
				if (fread(cells.data(), 1, cells.size(), input.get()) !=
				    cells.size()) {
					return false;
				}
				uint32_t count = 0;
				for (size_t i = 0; i < cells.size(); ++i) {
					count |= uint32_t{cells[i]} << (i * 8);
				}
				if (!count || count > (size + 3) * 16) {
					return false;
				}
				data.write_bits = count;
			}
			const auto repeated = std::ranges::any_of(
			        overlay, [&](const auto& entry) {
				        return entry.first == address;
			        });
			if (fread(data.bytes.data(), 1, data.bytes.size(), input.get()) !=
			            data.bytes.size() ||
			    (version < OrderedOverlayVersion && repeated)) {
				return false;
			}
			overlay.emplace_back(address, data);
			ApplyOverlaySector(address, data);
		}
		bytes_read = fread(key.data(), 1, key.size(), input.get());
	}
	if (ferror(input.get()) || !feof(input.get()) || bytes_read != 0) {
		return false;
	}
	return true;
}

bool FloppyTrackImage::SaveOverlay(const Overlay& entries,
                                   const TrackFormats& formats) const
{
	const auto temporary = overlay_path + ".tmp";
	// The writer lock covers the entire mount, so any temporary file
	// left here belongs to an interrupted save.
	std::error_code error = {};
	if (std::filesystem::is_regular_file(temporary, error)) {
		std::filesystem::remove(temporary, error);
		if (error) {
			return false;
		}
	}
	auto* file = fopen(temporary.c_str(), "wbx");
	if (!file) {
		return false;
	}
	// Keep writing version 4 when all records fit its 512-byte layout.
	const auto has_sized_writes = [](const Overlay& writes) {
		return std::ranges::any_of(writes, [](const auto& entry) {
			return entry.first.back() != LegacySizeCode;
		});
	};
	const auto sized =
	        has_sized_writes(entries) ||
	        std::ranges::any_of(formats, [&](const auto& track) {
		        return std::ranges::any_of(track.second, [&](const auto& format) {
			        return has_sized_writes(format.prior_writes);
		        });
	        });
	const auto repeated = [](const Overlay& writes) {
		std::set<OverlayKey> keys = {};
		return std::ranges::any_of(writes, [&](const auto& entry) {
			return !keys.insert(entry.first).second;
		});
	};
	const auto ordered =
	        repeated(entries) ||
	        std::ranges::any_of(formats, [&](const auto& track) {
		        return std::ranges::any_of(track.second, [&](const auto& format) {
			        return repeated(format.prior_writes);
		        });
	        });
	const auto formatted_size = std::ranges::any_of(formats, [](const auto& track) {
		return std::ranges::any_of(track.second, [](const auto& format) {
			if (format.size_code == 0xff) {
				return false;
			}
			for (size_t i = 3; i < format.ids.size(); i += ChrnSize) {
				if (format.ids[i] != format.size_code) {
					return true;
				}
			}
			return false;
		});
	});
	const auto formatted_rate = std::ranges::any_of(formats, [](const auto& track) {
		return std::ranges::any_of(track.second, [](const auto& format) {
			return format.rate_kbps != 0;
		});
	});
	const auto has_deleted = [](const Overlay& writes) {
		return std::ranges::any_of(writes, [](const auto& entry) {
			return entry.second.deleted;
		});
	};
	const auto deleted =
	        has_deleted(entries) ||
	        std::ranges::any_of(formats, [&](const auto& track) {
		        return std::ranges::any_of(track.second, [&](const auto& format) {
			        return has_deleted(format.prior_writes);
		        });
	        });
	const auto has_fm = [](const Overlay& writes) {
		return std::ranges::any_of(writes, [](const auto& entry) {
			return entry.second.fm;
		});
	};
	const auto fm =
	        has_fm(entries) ||
	        std::ranges::any_of(formats, [&](const auto& track) {
		        return std::ranges::any_of(track.second, [&](const auto& format) {
			        return format.fm || has_fm(format.prior_writes);
		        });
	        });
	const auto gapped = std::ranges::any_of(formats, [](const auto& track) {
		return std::ranges::any_of(track.second, [](const auto& format) {
			return format.gap.has_value();
		});
	});
	const auto has_partial = [](const Overlay& writes) {
		return std::ranges::any_of(writes, [](const auto& entry) {
			return entry.second.write_bits.has_value();
		});
	};
	const auto partial =
	        has_partial(entries) ||
	        std::ranges::any_of(formats, [&](const auto& track) {
		        return std::ranges::any_of(track.second, [&](const auto& format) {
			        return has_partial(format.prior_writes);
		        });
	        });
	const auto cutoff = std::ranges::any_of(formats, [](const auto& track) {
		return std::ranges::any_of(track.second, [](const auto& format) {
			return format.written_cells.has_value();
		});
	});
	const auto version = cutoff         ? FormatCutoffOverlayVersion
	                   : partial        ? PartialWriteOverlayVersion
	                   : gapped         ? GapOverlayVersion
	                   : fm             ? EncodingOverlayVersion
	                   : deleted        ? DeletedMarkOverlayVersion
	                   : formatted_rate ? FormatRateOverlayVersion
	                   : formatted_size ? FormatSizeOverlayVersion
	                   : ordered        ? OrderedOverlayVersion
	                   : sized          ? SizedOverlayVersion
	                                    : LegacyOverlayVersion;
	const auto header  = overlay_header(capture_fingerprint, version);
	auto success       = fwrite(header.data(), 1, header.size(), file) ==
	               header.size();
	const auto write_sectors = [&](const Overlay& writes) {
		for (const auto& [key, data] : writes) {
			const auto [cylinder, head, id, slot, size_code] = key;
			const std::array record = {check_cast<uint8_t>(cylinder),
			                           slot ? PhysicalWriteMarker
			                                : check_cast<uint8_t>(head),
			                           check_cast<uint8_t>(
			                                   slot ? head : id)};
			success = success &&
			          fwrite(record.data(), 1, record.size(), file) ==
			                  record.size();
			if (slot) {
				std::array<uint8_t, 4> slot_bytes = {};
				for (size_t i = 0; i < slot_bytes.size(); ++i) {
					slot_bytes[i] = static_cast<uint8_t>(
					        slot >> (i * 8));
				}
				success = success &&
				          fwrite(slot_bytes.data(),
				                 1,
				                 slot_bytes.size(),
				                 file) == slot_bytes.size();
			}
			success = success &&
			          (version < SizedOverlayVersion ||
			           fputc(check_cast<int>(size_code), file) != EOF) &&
			          (version < DeletedMarkOverlayVersion ||
			           fputc((data.deleted ? 1 : 0) | (data.fm ? 2 : 0) |
			                         (data.write_bits ? 4 : 0),
			                 file) != EOF);
			if (data.write_bits) {
				std::array<uint8_t, 4> cells = {};
				for (size_t i = 0; i < cells.size(); ++i) {
					cells[i] = static_cast<uint8_t>(
					        *data.write_bits >> (i * 8));
				}
				success = success &&
				          fwrite(cells.data(), 1, cells.size(), file) ==
				                  cells.size();
			}
			success = success && fwrite(data.bytes.data(),
			                            1,
			                            data.bytes.size(),
			                            file) == data.bytes.size();
		}
	};
	const auto write_cells = [&](const uint32_t count) {
		std::array<uint8_t, 4> bytes = {};
		for (size_t i = 0; i < bytes.size(); ++i) {
			bytes[i] = static_cast<uint8_t>(count >> (i * 8));
		}
		return fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	};
	for (const auto& [address, history] : formats) {
		for (const auto& format : history) {
			write_sectors(format.prior_writes);
			const auto count = check_cast<uint8_t>(
			        format.ids.size() / ChrnSize);
			const auto partial      = count < format.requested ||
			                          format.written_cells;
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
			          (version < FormatSizeOverlayVersion ||
			           fputc(format.size_code, file) != EOF) &&
			          (version < FormatRateOverlayVersion ||
			           (fputc(format.rate_kbps & 0xff, file) != EOF &&
			            fputc(format.rate_kbps >> 8, file) != EOF)) &&
			          (version < EncodingOverlayVersion ||
			           fputc((format.fm ? 1 : 0) |
			                         (version >= GapOverlayVersion && format.gap
			                                  ? 2
			                                  : 0),
			                 file) != EOF) &&
			          (version < GapOverlayVersion ||
			           fputc(format.gap.value_or(0), file) != EOF) &&
			          (version < FormatCutoffOverlayVersion ||
			           write_cells(format.written_cells.value_or(0))) &&
			          fwrite(format.ids.data(), 1, format.ids.size(), file) ==
			                  format.ids.size();
		}
	}
	write_sectors(entries);
	success = fclose(file) == 0 && success;
	if (success) {
		std::filesystem::rename(temporary, overlay_path, error);
		if (!error) {
			return true;
		}
	}
	std::filesystem::remove(temporary, error);
	return false;
}

bool FloppyTrackImage::HasOverlappingFields(const uint32_t cylinder,
                                            const uint32_t head) const
{
	const auto& sectors = tracks[cylinder * heads + head].sectors;
	// Different separator clocks can expose overlapping fields. Their bit
	// offsets cannot be compared without translating between clock grids.
	if (std::ranges::any_of(sectors, [&](const auto& sector) {
		    return sector.fm ||
		           std::ranges::any_of(sectors, [&](const auto& other) {
			           return other.revolution == sector.revolution &&
			                  other.mfm_track != sector.mfm_track;
		           });
	    })) {
		return true;
	}
	for (const auto& sector : sectors) {
		if (!sector.mfm_track) {
			continue;
		}
		const auto bits = sector.mfm_track->bit_count;
		const auto start = (sector.data_bit + bits - (256 % bits)) % bits;
		const auto length   = (sector.data.size() + 18) * 16 + 1;
		const auto overlaps = [&](const size_t other_start,
		                          const size_t other_length) {
			return length >= bits || other_length >= bits ||
			       (other_start + bits - start) % bits < length ||
			       (start + bits - other_start) % bits < other_length;
		};
		for (const auto& other : sectors) {
			if (&other == &sector ||
			    (other.revolution != sector.revolution &&
			     other.mfm_track != sector.mfm_track)) {
				continue;
			}
			// A write can erase another ID or change an enclosing
			// field's CRC.
			if (overlaps(other.id_bit, 160) ||
			    (other.mfm_track == sector.mfm_track &&
			     overlaps((other.data_bit + bits - (256 % bits)) % bits,
			              (other.data.size() + 18) * 16 + 1))) {
				return true;
			}
		}
	}
	return false;
}

bool FloppyTrackImage::WriteSector(const uint32_t cylinder, const uint32_t head,
                                   const uint32_t sector,
                                   const std::span<const uint8_t> data,
                                   const FloppySector* selected, const bool deleted,
                                   const std::optional<uint32_t> write_bits)
{
	if (IsWriteProtected() || cylinder >= cylinders || head >= heads ||
	    sector > UINT8_MAX || data.size() < 128 ||
	    (data.size() == 128 && (!selected || !selected->fm)) ||
	    data.size() > 1024 || !std::has_single_bit(data.size()) ||
	    (write_bits && (!*write_bits || *write_bits > (data.size() + 3) * 16))) {
		return false;
	}
	std::optional<FloppySector> stable_selected = {};
	if (selected) {
		if (selected->id_crc_error) {
			return false;
		}
		ensure_encoding_fields(cylinder, head, selected->fm);
		const auto fields = GetTrack(cylinder, head);
		const auto found = std::ranges::find_if(fields, [&](const auto& field) {
			return field.fm == selected->fm &&
			       field.cylinder == selected->cylinder &&
			       field.head == selected->head &&
			       field.id == selected->id &&
			       field.size_code == selected->size_code &&
			       field.revolution == selected->revolution &&
			       std::abs(field.cycle_fraction -
			                selected->cycle_fraction) < 0.00002;
		});
		if (found == fields.end()) {
			return false;
		}
		stable_selected = *found;
		selected        = &*stable_selected;
	}
	const auto fm = selected && selected->fm;
	const auto size_code = check_cast<uint32_t>(std::countr_zero(data.size()) - 7);
	const OverlayKey logical = {cylinder, head, sector, 0, size_code};
	std::vector<OverlayKey> targets = {};
	if ((!selected || (selected->cylinder == cylinder && selected->head == head)) &&
	    CanOverlaySector(logical, fm)) {
		targets.push_back(logical);
	} else if (selected) {
		const auto track = GetTrack(cylinder, head);
		const auto& ends = tracks[cylinder * heads + head].revolution_ends;
		std::vector<std::vector<const FloppySector*>> copies(ends.size());
		uint32_t selected_slot = 0;
		for (size_t i = 0; i < track.size(); ++i) {
			const auto& candidate = track[i];
			if (candidate.fm == fm &&
			    candidate.cylinder == selected->cylinder &&
			    candidate.head == selected->head &&
			    candidate.id == selected->id &&
			    candidate.size_code == size_code) {
				const auto slot = check_cast<uint32_t>(
				        candidate.overlay_slot);
				copies[candidate.revolution].push_back(&candidate);
				if (candidate.overlay_slot == selected->overlay_slot &&
				    candidate.id_bit == selected->id_bit &&
				    candidate.revolution == selected->revolution) {
					selected_slot = slot;
				}
			}
		}
		if (!selected_slot || selected->id_crc_error) {
			return false;
		}
		const auto& turn = copies[selected->revolution];
		const auto phase = [&](const FloppySector& field) {
			const auto start = field.revolution
			                         ? ends[field.revolution - 1]
			                         : 0;
			return (field.cycle_fraction - start) /
			       (ends[field.revolution] - start);
		};
		const auto selected_phase = phase(*selected);
		const auto distance       = [&](const FloppySector* field) {
                        const auto difference = std::abs(phase(*field) -
                                                         selected_phase);
                        return std::min(difference, 1.0 - difference);
		};
		// A missing ID makes the occurrence ambiguous on the other turns.
		if (std::all_of(copies.begin(), copies.end(), [&](const auto& other) {
			    return other.size() == turn.size();
		    })) {
			for (const auto& other : copies) {
				// Index jitter can move an ID from the start of
				// one recorded turn to its end. Match angular
				// position, not the ID's ordinal in the sorted
				// track table.
				const auto closest = std::ranges::min_element(
				        other, {}, distance);
				const OverlayKey key = {cylinder,
				                        head,
				                        0,
				                        check_cast<uint32_t>(
				                                (*closest)->overlay_slot),
				                        size_code};
				if (CanOverlaySector(key, fm)) {
					targets.push_back(key);
				}
			}
		} else {
			targets.push_back(
			        {cylinder, head, 0, selected_slot, size_code});
		}
	} else {
		return false;
	}
	const OverlaySector updated = {std::vector<uint8_t>(data.begin(), data.end()),
	                               deleted,
	                               fm,
	                               write_bits};
	auto entries       = overlay;
	const auto ordered = write_bits.has_value() ||
	                     HasOverlappingFields(cylinder, head) ||
	                     std::ranges::any_of(entries, [&](const auto& entry) {
		                     return entry.first[0] == cylinder &&
		                            entry.first[1] == head &&
		                            entry.second.write_bits.has_value();
	                     });
	for (const auto& key : targets) {
		if (!ordered) {
			// Independent fields only need their latest contents.
			// Keep the ordered history on overlapping tracks where
			// writes interact.
			std::erase_if(entries, [&](const auto& entry) {
				return entry.first == key;
			});
		}
		entries.emplace_back(key, updated);
	}
	if (!SaveOverlay(entries, track_formats)) {
		return false;
	}
	for (const auto& key : targets) {
		ApplyOverlaySector(key, updated);
	}
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
	std::erase_if(entries, [&](auto& entry) {
		if (entry.first[0] != address[0] || entry.first[1] != address[1]) {
			return false;
		}
		writes.push_back(std::move(entry));
		return true;
	});
	return writes;
}

void FloppyTrackImage::ApplyTrackFormat(const TrackAddress& address,
                                        const TrackFormat& format)
{
	auto& track        = tracks[address[0] * heads + address[1]];
	const auto visible = GetTrack(address[0], address[1]);
	const std::vector<FloppySector> previous(visible.begin(), visible.end());
	const auto count   = format.ids.size() / ChrnSize;
	const auto partial = count < format.requested || format.written_cells;
	if (!partial) {
		track.cycle_ms        = track.format_cycle_ms;
		track.revolution_ends = {1.0};
	}
	std::vector<std::shared_ptr<FloppyMfmTrack>> revolutions = {};
	for (size_t revolution = 0; revolution < track.revolution_ends.size();
	     ++revolution) {
		const auto start    = revolution
		                            ? track.revolution_ends[revolution - 1]
		                            : 0;
		const auto duration = track.revolution_ends[revolution] - start;
		revolutions.push_back(
		        format_track_cells(previous,
		                           revolution,
		                           duration * track.cycle_ms,
		                           format,
		                           track.raw_tracks.empty()
		                                   ? nullptr
		                                   : track.raw_tracks.front()));
	}
	const auto canonical = join_revolutions(revolutions);
	auto sectors = DecodeRevolutions(revolutions, track.revolution_ends);
	if (format.fm) {
		auto sampled    = canonical->Sample(0,
		                                    track.cycle_ms,
		                                    format.rate_kbps / 2);
		auto fm_sectors = DecodeTrack(sampled->cells,
		                              sampled->bit_count,
		                              sampled->index,
		                              false,
		                              sampled,
		                              true);
		if (!sampled->revolution_ends_bits.empty()) {
			PositionSectors(fm_sectors, track.revolution_ends);
		}
		sectors.insert(sectors.end(), fm_sectors.begin(), fm_sectors.end());
	}
	if (partial) {
		for (size_t revolution = 0;
		     revolution < track.revolution_ends.size();
		     ++revolution) {
			if (std::ranges::any_of(previous, [&](const auto& sector) {
				    return sector.revolution == revolution &&
				           sector.mfm_track;
			    })) {
				continue;
			}
			const auto start = revolution
			                         ? track.revolution_ends[revolution - 1]
			                         : 0;
			const auto duration = track.revolution_ends[revolution] - start;
			const auto fraction = format.written_cells
			                            ? *format.written_cells /
			                                      (duration * track.cycle_ms *
			                                       (format.fm ? 1 : 2) *
			                                       format.rate_kbps)
			                            : static_cast<double>(count) /
			                                      format.requested;
			const auto cutoff = start + duration * fraction;
			for (const auto& sector : previous) {
				if (sector.revolution == revolution &&
				    sector.cycle_fraction >= cutoff) {
					sectors.push_back(sector);
				}
			}
		}
		std::stable_sort(sectors.begin(),
		                 sectors.end(),
		                 [](const auto& left, const auto& right) {
			                 return left.cycle_fraction <
			                        right.cycle_fraction;
		                 });
	}
	track.sectors               = std::move(sectors);
	track.dirty                 = true;
	track.cells_changed         = false;
	track.encoding_fields_ready = {};
	track.flux.reset();
	track.raw_tracks = {canonical};
}

FloppyTrackImage::FormatResult FloppyTrackImage::FormatTrack(
        const uint8_t cylinder, const uint8_t head,
        const std::span<const uint8_t> ids, const uint8_t fill,
        const uint8_t requested, const uint8_t size_code,
        const uint16_t rate_kbps, const bool fm, const std::optional<uint8_t> gap,
        const std::optional<uint32_t> written_cells)
{
	if (IsWriteProtected()) {
		return FormatResult::WriteError;
	}
	const TrackAddress address = {cylinder, head};
	const auto count           = ids.size() / ChrnSize;
	if ((written_cells && (!*written_cells || !gap)) || count > requested ||
	    size_code > 7 || !IsFloppyDataRate(rate_kbps) ||
	    (size_t{128} << size_code) * count > 64 * 1024 ||
	    !IsValidTrackFormat(address, ids)) {
		return FormatResult::InvalidLayout;
	}
	auto entries  = overlay;
	auto prior    = TakeTrackWrites(entries, address);
	auto formats  = track_formats;
	auto& history = formats[address];
	if (count == requested && !written_cells) {
		history.clear();
		prior.clear();
	}
	history.push_back({
	        {ids.begin(), ids.end()},
	        fill,
	        requested,
	        std::move(prior),
	        size_code,
	        rate_kbps,
	        fm,
	        gap,
	        written_cells
        });
	if (!SaveOverlay(entries, formats)) {
		return FormatResult::WriteError;
	}
	ApplyTrackFormat(address, history.back());
	overlay.swap(entries);
	track_formats.swap(formats);
	return FormatResult::Success;
}
