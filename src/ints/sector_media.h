// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_SECTOR_MEDIA_H
#define DOSBOX_SECTOR_MEDIA_H

#include <cstdint>

// Logical sector access independent of the controller and track encoding.
// Buffers hold one logical sector. Operations return BIOS disk status.
class SectorMedia {
public:
	virtual ~SectorMedia() = default;

	virtual uint8_t Read_Sector(uint32_t head, uint32_t cylinder,
	                            uint32_t sector, void* data)          = 0;
	virtual uint8_t Write_Sector(uint32_t head, uint32_t cylinder,
	                             uint32_t sector, void* data)         = 0;
	virtual uint8_t Read_AbsoluteSector(uint32_t sector, void* data)  = 0;
	virtual uint8_t Write_AbsoluteSector(uint32_t sector, void* data) = 0;
};

#endif
