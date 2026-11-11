// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_TRACK_IMAGE_H
#define DOSBOX_FLOPPY_TRACK_IMAGE_H

#include <cstdint>
#include <cstdio>
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
	std::vector<uint8_t> data = {};
};

class FloppyTrackImage {
public:
	static std::unique_ptr<FloppyTrackImage> LoadHxcMfm(FILE* file);
	static std::unique_ptr<FloppyTrackImage> LoadScp(FILE* file,
	                                                 std::string& error);

	double GetCycleMs(const uint32_t cylinder, const uint32_t head) const;

	std::span<const FloppySector> GetTrack(const uint32_t cylinder,
	                                       const uint32_t head) const;

	uint32_t cylinders       = 0;
	uint32_t heads           = 0;
	uint32_t logical_sectors = 0;

private:
	struct Track {
		std::vector<FloppySector> sectors = {};
		double cycle_ms                   = 200.0;
	};
	static std::vector<FloppySector> DecodeMfmTrack(
	        const std::span<const uint8_t> bitstream);
	void UpdateLogicalGeometry(const uint32_t cylinder, const uint32_t head,
	                           const std::span<const FloppySector> sectors);
	std::vector<Track> tracks = {};
};

#endif
