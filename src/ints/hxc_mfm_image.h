// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_HXC_MFM_IMAGE_H
#define DOSBOX_HXC_MFM_IMAGE_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

struct FloppySector {
	uint8_t cylinder          = 0;
	uint8_t head              = 0;
	uint8_t id                = 0;
	uint8_t size_code         = 0;
	double revolution_offset  = 0;
	bool id_crc_error         = false;
	bool data_crc_error       = false;
	bool deleted              = false;
	std::vector<uint8_t> data = {};
};

class HxcMfmImage {
public:
	static std::unique_ptr<HxcMfmImage> Load(FILE* file);

	std::span<const FloppySector> GetTrack(const uint32_t cylinder,
	                                       const uint32_t head) const;

	uint32_t cylinders       = 0;
	uint32_t heads           = 0;
	uint32_t logical_sectors = 0;
	double revolution_ms     = 200.0;

private:
	std::vector<std::vector<FloppySector>> tracks = {};
};

#endif
