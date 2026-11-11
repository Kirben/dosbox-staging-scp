// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_TRACK_IMAGE_H
#define DOSBOX_FLOPPY_TRACK_IMAGE_H

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct FloppySector {
	uint8_t cylinder          = 0;
	uint8_t head              = 0;
	uint8_t id                = 0;
	uint8_t size_code         = 0;
	double cycle_fraction     = 0;
	bool id_crc_error         = false;
	bool data_crc_error       = false;
	bool deleted              = false;
	size_t revolution         = 0;
	std::vector<uint8_t> data = {};
};

class FloppyTrackImage {
public:
	static std::unique_ptr<FloppyTrackImage> LoadHxcMfm(FILE* file);
	static std::unique_ptr<FloppyTrackImage> LoadScp(FILE* file,
	                                                 const std::string& path,
	                                                 const bool read_only,
	                                                 std::string& error);

	static std::unique_ptr<FloppyTrackImage> Load86f(FILE* file,
	                                                 std::string& error);

	double GetCycleMs(const uint32_t cylinder, const uint32_t head) const;
	double GetFormatCycleMs(const uint32_t cylinder, const uint32_t head) const;

	std::span<const FloppySector> GetTrack(const uint32_t cylinder,
	                                       const uint32_t head) const;
	bool IsWriteProtected() const;
	bool WriteScpSector(const uint32_t cylinder, const uint32_t head,
	                    const uint32_t sector,
	                    const std::span<const uint8_t> data,
	                    const FloppySector* selected = nullptr);
	enum class FormatResult { Success, InvalidLayout, WriteError };
	FormatResult FormatScpTrack(const uint8_t cylinder, const uint8_t head,
	                            const std::span<const uint8_t> ids,
	                            const uint8_t fill, const uint8_t requested);

	uint32_t cylinders       = 0;
	uint32_t heads           = 0;
	uint32_t logical_sectors = 0;

private:
	// Cylinder, head, logical ID, and one-based physical slot (zero for CHS).
	using OverlayKey    = std::array<uint32_t, 4>;
	using OverlaySector = std::array<uint8_t, 512>;
	using Overlay       = std::map<OverlayKey, OverlaySector>;
	using TrackAddress  = std::array<uint8_t, 2>;
	struct TrackFormat {
		std::vector<uint8_t> ids = {};
		uint8_t fill             = 0;
		uint8_t requested        = 0;
		Overlay prior_writes     = {};
	};
	using TrackFormats = std::map<TrackAddress, std::vector<TrackFormat>>;
	bool LoadScpOverlay();
	bool SaveScpOverlay(const Overlay& overlay, const TrackFormats& formats) const;
	bool IsValidTrackFormat(const TrackAddress& address,
	                        const std::span<const uint8_t> ids) const;
	void ApplyTrackFormat(const TrackAddress& address, const TrackFormat& format);
	static Overlay TakeTrackWrites(Overlay& entries, const TrackAddress& address);
	bool CanOverlaySector(const OverlayKey& key) const;
	void ApplyOverlaySector(const OverlayKey& key, const OverlaySector& data);
	std::string overlay_path     = {};
	uint64_t capture_fingerprint = 0;
	bool overlay_writable        = false;
	Overlay overlay              = {};
	TrackFormats track_formats   = {};

	struct Track {
		std::vector<FloppySector> sectors = {};
		double cycle_ms                   = 200.0;
		double format_cycle_ms            = 200.0;
		std::vector<double> revolution_ends = {1.0};
	};
	static std::vector<FloppySector> DecodeMfmTrack(
	        const std::span<const uint8_t> bitstream,
	        const size_t bit_count = 0, const size_t index = 0);
	void UpdateLogicalGeometry(const uint32_t cylinder, const uint32_t head,
	                           const std::span<const FloppySector> sectors);
	std::vector<Track> tracks = {};
};

#endif
