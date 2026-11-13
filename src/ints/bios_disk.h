// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_BIOS_DISK_H
#define DOSBOX_BIOS_DISK_H

#include "dosbox.h"

#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dos/dos.h"
#include "hardware/memory.h"
#include "ints/bios.h"
#include "ints/sector_media.h"

/* The Section handling Bios Disk Access */
#define BIOS_MAX_DISK 10

#define MAX_SWAPPABLE_DISKS 20
struct DiskGeometry {
	uint32_t ksize;     /* Size in kilobytes */
	uint16_t secttrack; /* Sectors per track */
	uint16_t headscyl;  /* Heads per cylinder */
	uint16_t cylcount;  /* Cylinders per side */
	uint16_t biosval;   /* Type to return from BIOS */
};

const std::vector<DiskGeometry>& BIOS_GetDiskGeometryList();

class FloppyMedia;

class imageDisk final : public SectorMedia {
public:
	MediaResult Read_Sector(const uint32_t head, const uint32_t cylinder,
	                        const uint32_t sector,
	                        const std::span<uint8_t> data) override;
	MediaResult Write_Sector(const uint32_t head, const uint32_t cylinder,
	                         const uint32_t sector,
	                         const std::span<const uint8_t> data) override;
	MediaResult Read_AbsoluteSector(const uint32_t sectnum,
	                                const std::span<uint8_t> data) override;
	MediaResult Write_AbsoluteSector(const uint32_t sectnum,
	                                 const std::span<const uint8_t> data) override;

	// The controller schedules its own transfer and requires a full sector.
	bool ReadSectorForController(const uint32_t head, const uint32_t cylinder,
	                             const uint32_t sector,
	                             const std::span<uint8_t> data);

	bool VerifyRawSector(const uint32_t sector, const std::span<uint8_t> scratch);

	bool IsWriteProtected() const;
	bool WriteSectorForController(const uint32_t head, const uint32_t cylinder,
	                              const uint32_t sector,
	                              const std::span<const uint8_t> data);

	void Set_Geometry(uint32_t setHeads, uint32_t setCyl, uint32_t setSect,
	                  uint32_t setSectSize);
	void Get_Geometry(uint32_t* getHeads, uint32_t* getCyl,
	                  uint32_t* getSect, uint32_t* getSectSize);
	uint8_t GetBiosType(void);
	uint32_t getSectSize(void);

	imageDisk(FILE* img_file, const char* img_name, uint32_t img_size_k,
	          bool is_hdd, bool read_only = false);
	imageDisk(std::shared_ptr<FloppyMedia> media, const std::string& name,
	          const uint32_t head_count, const uint32_t cylinder_count,
	          const uint32_t sectors_per_track);
	std::shared_ptr<FloppyMedia> GetFloppyMedia() const;
	imageDisk(const imageDisk&)            = delete; // prevent copy
	imageDisk& operator=(const imageDisk&) = delete; // prevent assignment

	~imageDisk() override
	{
		if (diskimg != nullptr) {
			fclose(diskimg);
		}
	}

	bool hardDrive;
	bool active;
	FILE* diskimg;
	char diskname[512];
	uint8_t floppytype;

	uint32_t sector_size;
	uint32_t heads, cylinders, sectors;

private:
	MediaResult write_absolute_sector(const uint32_t sector,
	                                  const std::span<const uint8_t> data,
	                                  const bool controller_write);
	bool read_only                            = false;
	std::shared_ptr<FloppyMedia> floppy_media = {};
	enum class RawReadMode { Normal, Controller, Verify };
	MediaResult read_absolute_sector(const uint32_t sector,
	                                 const std::span<uint8_t> data,
	                                 const RawReadMode mode);
	cross_off_t current_fpos;
	enum { NONE, READ, WRITE } last_action;
};

uint8_t BIOS_GetFloppyCmosType(const size_t slot);

#define MAX_HDD_IMAGES 2

#define MAX_DISK_IMAGES (2 + MAX_HDD_IMAGES)

using BiosBootDisks = std::array<std::shared_ptr<imageDisk>, MAX_SWAPPABLE_DISKS>;

std::shared_ptr<imageDisk> BIOS_GetDisk(const size_t slot);
std::shared_ptr<FloppyMedia> BIOS_GetFloppyMedia(const size_t slot);
void BIOS_SetDisk(const size_t slot, std::shared_ptr<imageDisk> disk);
void BIOS_SetBootDisks(BiosBootDisks disks);
bool BIOS_ReadFloppyBootSector(const std::shared_ptr<imageDisk>& disk,
                               const PhysPt address);

extern uint16_t imgDTASeg; /* Real memory location of temporary DTA pointer for
                              fat image disk access */
extern RealPt imgDTAPtr;   /* Real memory location of temporary DTA pointer for
                              fat image disk access */
extern DOS_DTA* imgDTA;

bool getSwapRequest(void);

#endif
