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

std::shared_ptr<FloppyMfmTrack> recover_bitstream(const FluxTurn& turn,
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
	const auto bit_count = cells_written + tail;
	bitstream.resize((bit_count + 7) / 8);
	auto raw       = std::make_shared<FloppyMfmTrack>();
	raw->cells     = std::move(bitstream);
	raw->bit_count = bit_count;
	return raw;
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
		for (const auto cell_ns : CellTimesNs) {
			std::vector<std::shared_ptr<FloppyMfmTrack>> revolutions = {};
			for (const auto& turn : turns) {
				auto raw = recover_bitstream(turn, cell_ns);
				if (!raw || !raw->bit_count) {
					break;
				}
				revolutions.push_back(std::move(raw));
			}
			if (revolutions.size() != turns.size()) {
				continue;
			}
			// Index does not restart the data separator. A field
			// can continue into the following captured turn.
			auto candidate = DecodeMfmRevolutions(revolutions,
			                                      track.revolution_ends);
			int score      = 0;
			for (const auto& sector : candidate) {
				if (!sector.id_crc_error) {
					score += sector.data.empty() || sector.data_crc_error
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
		track.sectors = std::move(decoded);
	}
	if (!found_ids) {
		error = "no IBM MFM sector IDs found";
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

bool FloppyTrackImage::CanOverlaySector(const OverlayKey& key) const
{
	const auto [cylinder, head, id, slot, size_code] = key;
	if (slot) {
		if (cylinder >= cylinders || head >= heads) {
			return false;
		}
		const auto& track = tracks[cylinder * heads + head].sectors;
		return slot <= track.size() &&
		       track[slot - 1].size_code == size_code &&
		       !track[slot - 1].id_crc_error;
	}
	const auto track                      = GetTrack(cylinder, head);
	std::optional<size_t> last_revolution = {};
	for (const auto& sector : track) {
		if (sector.cylinder != cylinder || sector.head != head ||
		    sector.id != id || sector.size_code != size_code ||
		    sector.id_crc_error) {
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
	const auto [cylinder, head, id, slot, size_code] = key;
	auto& sectors = tracks[cylinder * heads + head].sectors;
	tracks[cylinder * heads + head].dirty = true;
	const auto apply                      = [&](FloppySector& sector) {
                const auto create_data_mark = sector.data.empty();
                tracks[cylinder * heads + head].cells_changed |= bool(sector.mfm_track);
                sector.data           = data.bytes;
                sector.data_crc_error = false;
                sector.deleted        = data.deleted;
                sector.weak_mask.clear();
                sector.extended_data.clear();
                sector.UpdateTrackData(create_data_mark);
	};
	if (slot) {
		apply(sectors[slot - 1]);
		return;
	}
	for (auto& sector : sectors) {
		if (sector.cylinder == cylinder && sector.head == head &&
		    sector.id == id && sector.size_code == size_code &&
		    !sector.id_crc_error) {
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
	     header != overlay_header(capture_fingerprint, DeletedMarkOverlayVersion))) {
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
			if (count <= 0 || fill == EOF || requested < count ||
			    (rate_kbps && !IsFloppyDataRate(rate_kbps)) ||
			    (size_code != 0xff && (size_code < 0 || size_code > 7)) ||
			    (size_code != 0xff &&
			     (size_t{128} << size_code) * count > 64 * 1024) ||
			    (partial && requested == count) ||
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
			if (!IsFloppyOverlaySizeCode(size_code)) {
				return false;
			}
			address.back() = check_cast<uint32_t>(size_code);
			if (!CanOverlaySector(address)) {
				return false;
			}
			// Version 9 stores a normal/deleted mark flag before
			// the payload.
			const auto mark = version >= DeletedMarkOverlayVersion
			                        ? fgetc(input.get())
			                        : 0;
			if (mark != 0 && mark != 1) {
				return false;
			}
			const auto size = size_t{128} << size_code;
			OverlaySector data = {std::vector<uint8_t>(size), mark == 1};
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
	const auto version = deleted        ? DeletedMarkOverlayVersion
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
			           fputc(data.deleted ? 1 : 0, file) != EOF) &&
			          fwrite(data.bytes.data(), 1, data.bytes.size(), file) ==
			                  data.bytes.size();
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
			success                 = success &&
			          fwrite(record.data(), 1, record.size(), file) ==
			                  record.size() &&
			          (!partial || fputc(format.requested, file) != EOF) &&
			          (version < FormatSizeOverlayVersion ||
			           fputc(format.size_code, file) != EOF) &&
			          (version < FormatRateOverlayVersion ||
			           (fputc(format.rate_kbps & 0xff, file) != EOF &&
			            fputc(format.rate_kbps >> 8, file) != EOF)) &&
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
	for (const auto& sector : sectors) {
		if (!sector.mfm_track) {
			continue;
		}
		const auto bits     = sector.mfm_track->bit_count;
		const auto start    = (sector.data_bit + bits - 16) % bits;
		const auto length   = (sector.data.size() + 3) * 16 + 1;
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
			     overlaps((other.data_bit + bits - 16) % bits,
			              (other.data.size() + 3) * 16 + 1))) {
				return true;
			}
		}
	}
	return false;
}

bool FloppyTrackImage::WriteSector(const uint32_t cylinder, const uint32_t head,
                                   const uint32_t sector,
                                   const std::span<const uint8_t> data,
                                   const FloppySector* selected, const bool deleted)
{
	if (IsWriteProtected() || cylinder >= cylinders || head >= heads ||
	    sector > UINT8_MAX || data.size() < 256 || data.size() > 1024 ||
	    !std::has_single_bit(data.size())) {
		return false;
	}
	const auto size_code = check_cast<uint32_t>(std::countr_zero(data.size()) - 7);
	const OverlayKey logical = {cylinder, head, sector, 0, size_code};
	std::vector<OverlayKey> targets = {};
	if ((!selected || (selected->cylinder == cylinder && selected->head == head)) &&
	    CanOverlaySector(logical)) {
		targets.push_back(logical);
	} else if (selected) {
		const auto track = GetTrack(cylinder, head);
		const auto& ends = tracks[cylinder * heads + head].revolution_ends;
		std::vector<std::vector<const FloppySector*>> copies(ends.size());
		uint32_t selected_slot = 0;
		for (size_t i = 0; i < track.size(); ++i) {
			const auto& candidate = track[i];
			if (candidate.cylinder == selected->cylinder &&
			    candidate.head == selected->head &&
			    candidate.id == selected->id &&
			    candidate.size_code == size_code) {
				const auto slot = check_cast<uint32_t>(
				        candidate.overlay_slot);
				copies[candidate.revolution].push_back(&candidate);
				if (&candidate == selected) {
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
				if (CanOverlaySector(key)) {
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
	                               deleted};
	auto entries       = overlay;
	const auto ordered = HasOverlappingFields(cylinder, head);
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
	const auto partial = count < format.requested;
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
		revolutions.push_back(FormatMfmTrack(
		        previous, revolution, duration * track.cycle_ms, format));
	}
	auto sectors = DecodeMfmRevolutions(revolutions, track.revolution_ends);
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
			const auto cutoff = start +
			                    duration * static_cast<double>(count) /
			                            format.requested;
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
	track.sectors       = std::move(sectors);
	track.dirty         = true;
	track.cells_changed = false;
}

FloppyTrackImage::FormatResult FloppyTrackImage::FormatTrack(
        const uint8_t cylinder, const uint8_t head,
        const std::span<const uint8_t> ids, const uint8_t fill,
        const uint8_t requested, const uint8_t size_code, const uint16_t rate_kbps)
{
	if (IsWriteProtected()) {
		return FormatResult::WriteError;
	}
	const TrackAddress address = {cylinder, head};
	const auto count           = ids.size() / ChrnSize;
	if (count > requested || size_code > 7 || !IsFloppyDataRate(rate_kbps) ||
	    (size_t{128} << size_code) * count > 64 * 1024 ||
	    !IsValidTrackFormat(address, ids)) {
		return FormatResult::InvalidLayout;
	}
	auto entries  = overlay;
	auto prior    = TakeTrackWrites(entries, address);
	auto formats  = track_formats;
	auto& history = formats[address];
	if (count == requested) {
		history.clear();
		prior.clear();
	}
	history.push_back({
	        {ids.begin(), ids.end()},
                fill, requested, std::move(prior), size_code, rate_kbps
        });
	if (!SaveOverlay(entries, formats)) {
		return FormatResult::WriteError;
	}
	ApplyTrackFormat(address, history.back());
	overlay.swap(entries);
	track_formats.swap(formats);
	return FormatResult::Success;
}
