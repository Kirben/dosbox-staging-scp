// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_SECTOR_MEDIA_H
#define DOSBOX_SECTOR_MEDIA_H

#include <cstdint>
#include <span>

// Media results are independent of BIOS, DOS, and controller status codes.
enum class MediaResult {
	Ok = 0,
	CrcError,
	DeletedMark,
	NoSector,
	InvalidBuffer,
	WriteProtected,
	ReadError,
	WriteError,
	SeekError,
};

// Logical sector access independent of the controller and track encoding.
// Each buffer must hold exactly one logical sector. Ok means a full transfer.
class SectorMedia {
public:
	virtual ~SectorMedia() = default;

	virtual MediaResult Read_Sector(const uint32_t head, const uint32_t cylinder,
	                                const uint32_t sector,
	                                const std::span<uint8_t> data) = 0;
	virtual MediaResult Write_Sector(const uint32_t head, const uint32_t cylinder,
	                                 const uint32_t sector,
	                                 const std::span<const uint8_t> data) = 0;
	virtual MediaResult Read_AbsoluteSector(const uint32_t sector,
	                                        const std::span<uint8_t> data) = 0;
	virtual MediaResult Write_AbsoluteSector(
	        const uint32_t sector, const std::span<const uint8_t> data) = 0;
};

#endif
