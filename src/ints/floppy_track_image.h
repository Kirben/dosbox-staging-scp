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

#include "ints/floppy_media.h"

class FloppyTrackImage final : public FloppyMedia {
public:
	static std::unique_ptr<FloppyTrackImage> LoadHxcMfm(FILE* file,
	                                                    const std::string& path,
	                                                    const bool read_only,
	                                                    std::string& error);
	static std::unique_ptr<FloppyTrackImage> LoadScp(FILE* file,
	                                                 const std::string& path,
	                                                 const bool read_only,
	                                                 std::string& error);

	static std::unique_ptr<FloppyTrackImage> Load86f(FILE* file,
	                                                 const std::string& path,
	                                                 const bool read_only,
	                                                 std::string& error);

	double GetCycleMs(const uint32_t cylinder, const uint32_t head) const override;
	std::span<const double> GetRevolutionEnds(uint32_t cylinder,
	                                          uint32_t head) const override;
	double GetFormatCycleMs(const uint32_t cylinder,
	                        const uint32_t head) const override;

	std::span<const FloppySector> GetTrack(const uint32_t cylinder,
	                                       const uint32_t head) const override;
	bool SupportsTrackWrites() const override;
	bool IsWriteProtected() const override;
	bool WriteSector(const uint32_t cylinder, const uint32_t head,
	                 const uint32_t sector, const std::span<const uint8_t> data,
	                 const FloppySector* selected) override;
	FormatResult FormatTrack(const uint8_t cylinder, const uint8_t head,
	                         const std::span<const uint8_t> ids,
	                         const uint8_t fill, const uint8_t requested) override;

	uint32_t cylinders       = 0;
	uint32_t heads           = 0;
	uint32_t logical_sectors = 0;

private:
	// Cylinder, head, logical ID, physical slot (zero for CHS), and size code.
	using OverlayKey    = std::array<uint32_t, 5>;
	using OverlaySector = std::vector<uint8_t>;
	using Overlay       = std::map<OverlayKey, OverlaySector>;
	using TrackAddress  = std::array<uint8_t, 2>;
	struct TrackFormat {
		std::vector<uint8_t> ids = {};
		uint8_t fill             = 0;
		uint8_t requested        = 0;
		Overlay prior_writes     = {};
	};
	using TrackFormats = std::map<TrackAddress, std::vector<TrackFormat>>;
	bool OpenOverlay(const std::string& path, const bool read_only,
	                 const uint64_t fingerprint, std::string& error);
	bool LoadOverlay();
	bool SaveOverlay(const Overlay& overlay, const TrackFormats& formats) const;
	bool IsValidTrackFormat(const TrackAddress& address,
	                        const std::span<const uint8_t> ids) const;
	void ApplyTrackFormat(const TrackAddress& address, const TrackFormat& format);
	static Overlay TakeTrackWrites(Overlay& entries, const TrackAddress& address);
	bool CanOverlaySector(const OverlayKey& key) const;
	void ApplyOverlaySector(const OverlayKey& key, const OverlaySector& data);
	std::string overlay_path     = {};
	uint64_t capture_fingerprint = 0;
	bool overlay_writable        = false;
	std::unique_ptr<FILE, decltype([](FILE* file) { fclose(file); })> overlay_lock = {};
	Overlay overlay              = {};
	TrackFormats track_formats   = {};

	struct Track {
		std::vector<FloppySector> sectors   = {};
		double cycle_ms                     = 200.0;
		double format_cycle_ms              = 200.0;
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
