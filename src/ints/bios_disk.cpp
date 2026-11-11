// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bios_disk.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include "cpu/callback.h"
#include "cpu/registers.h"
#include "dos/dos.h" /* for Drives[] */
#include "dos/drives.h"
#include "gui/mapper.h"
#include "hardware/floppy_controller.h"
#include "hardware/memory.h"
#include "utils/string_utils.h"

static const std::vector<DiskGeometry> disk_geometry_list = {
        { 160,  8, 1, 40, 0}, // SS/DD 5.25"
        { 180,  9, 1, 40, 0}, // SS/DD 5.25"
        { 200, 10, 1, 40, 0}, // SS/DD 5.25" (booters)
        { 320,  8, 2, 40, 1}, // DS/DD 5.25"
        { 360,  9, 2, 40, 1}, // DS/DD 5.25"
        { 400, 10, 2, 40, 1}, // DS/DD 5.25" (booters)
        { 720,  9, 2, 80, 3}, // DS/DD 3.5"
        {1200, 15, 2, 80, 2}, // DS/HD 5.25"
        {1440, 18, 2, 80, 4}, // DS/HD 3.5"
        {1520, 19, 2, 80, 2}, // DS/HD 5.25" (XDF)
        {1680, 21, 2, 80, 4}, // DS/HD 3.5"  (DMF)
        {1720, 21, 2, 82, 4}, // DS/HD 3.5"  (DMF)
        {1840, 23, 2, 80, 4}, // DS/HD 3.5"  (XDF)
        {2880, 36, 2, 80, 6}, // DS/ED 3.5"
};

const std::vector<DiskGeometry>& BIOS_GetDiskGeometryList()
{
	return disk_geometry_list;
}

callback_number_t call_int13 = 0;
Bitu diskparm0, diskparm1;
static uint8_t last_status;
static uint8_t last_drive;
uint16_t imgDTASeg;
RealPt imgDTAPtr;
DOS_DTA* imgDTA;
bool killRead;
static bool swapping_requested;

void BIOS_SetEquipment(uint16_t equipment);

/* 2 floppys and 2 harddrives, max */
static std::array<std::shared_ptr<imageDisk>, MAX_DISK_IMAGES> disk_slots = {};
static BiosBootDisks boot_disks                                           = {};

static size_t swap_position = 0;

static void update_disk_parameter_tables()
{
	uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
	if (disk_slots[2]) {
		PhysPt dp0physaddr = CALLBACK_PhysPointer(diskparm0);
		disk_slots[2]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp0physaddr, (uint16_t)tmpcyl);
		phys_writeb(dp0physaddr + 0x2, (uint8_t)tmpheads);
		phys_writew(dp0physaddr + 0x3, 0);
		phys_writew(dp0physaddr + 0x5, (uint16_t)-1);
		phys_writeb(dp0physaddr + 0x7, 0);
		phys_writeb(dp0physaddr + 0x8,
		            (0xc0 | (((disk_slots[2]->heads) > 8) << 3)));
		phys_writeb(dp0physaddr + 0x9, 0);
		phys_writeb(dp0physaddr + 0xa, 0);
		phys_writeb(dp0physaddr + 0xb, 0);
		phys_writew(dp0physaddr + 0xc, (uint16_t)tmpcyl);
		phys_writeb(dp0physaddr + 0xe, (uint8_t)tmpsect);
	}
	if (disk_slots[3]) {
		PhysPt dp1physaddr = CALLBACK_PhysPointer(diskparm1);
		disk_slots[3]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp1physaddr, (uint16_t)tmpcyl);
		phys_writeb(dp1physaddr + 0x2, (uint8_t)tmpheads);
		phys_writeb(dp1physaddr + 0xe, (uint8_t)tmpsect);
	}
}

std::shared_ptr<imageDisk> BIOS_GetDisk(const size_t slot)
{
	return slot < disk_slots.size() ? disk_slots[slot] : nullptr;
}

static void set_disk_slot(const size_t slot, std::shared_ptr<imageDisk> disk)
{
	disk_slots[slot] = std::move(disk);
	if (slot >= 2) {
		if (!disk_slots[slot]) {
			const auto table = CALLBACK_PhysPointer(
			        slot == 2 ? diskparm0 : diskparm1);
			constexpr auto table_size = 16u;
			for (auto offset = 0u; offset < table_size; ++offset) {
				phys_writeb(table + offset, 0);
			}
		}
		update_disk_parameter_tables();
	}
}

void BIOS_SetDisk(const size_t slot, std::shared_ptr<imageDisk> disk)
{
	assert(slot < disk_slots.size());
	if (slot < 2 && disk_slots[slot] != disk) {
		// Prevent a later BOOT swap from restoring an ejected image.
		boot_disks.fill(nullptr);
		swap_position = 0;
	}
	set_disk_slot(slot, std::move(disk));
}

void incrementFDD(void)
{
	uint16_t equipment = mem_readw(BIOS_CONFIGURATION);
	if (equipment & 1) {
		Bitu numofdisks = (equipment >> 6) & 3;
		numofdisks++;
		if (numofdisks > 1) {
			numofdisks = 1; // max 2 floppies at the moment
		}
		equipment &= ~0x00C0;
		equipment |= (numofdisks << 6);
	} else {
		equipment |= 1;
	}
	BIOS_SetEquipment(equipment);
}

template <typename T, size_t N>
static size_t disk_array_prefix_size(const std::array<T, N>& images)
{
	size_t num = 0;
	for (const auto& disk : images) {
		if (!disk) {
			break;
		}
		num++;
	}
	return num;
}

static void load_boot_disks(const size_t first_disk)
{
	const size_t boot_disks_num = disk_array_prefix_size(boot_disks);
	if (boot_disks_num == 0) {
		return;
	}

	assert(first_disk < boot_disks_num);
	const auto pos_1 = first_disk;
	const auto pos_2 = (first_disk + 1) % boot_disks_num;

	set_disk_slot(0, boot_disks[pos_1]);
	LOG_MSG("Loaded disk A from swaplist position %zu - \"%s\"",
	        pos_1,
	        boot_disks[pos_1]->diskname);

	set_disk_slot(1, boot_disks[pos_2]);
	LOG_MSG("Loaded disk B from swaplist position %zu - \"%s\"",
	        pos_2,
	        boot_disks[pos_2]->diskname);
}

void BIOS_SetBootDisks(BiosBootDisks disks)
{
	boot_disks    = std::move(disks);
	swap_position = 0;
	load_boot_disks(swap_position);
}

bool getSwapRequest(void)
{
	bool sreq          = swapping_requested;
	swapping_requested = false;
	return sreq;
}

static void swap_disks(bool pressed)
{
	if (!pressed) {
		return;
	}
	DriveManager::CycleAllDisks();
	/* Hack/feature: rescan all disks as well */
	LOG_MSG("Diskcaching reset for normal mounted drives.");
	for (Bitu i = 0; i < DOS_DRIVES; i++) {
		if (Drives[i]) {
			Drives[i]->EmptyCache();
		}
	}
	const auto count = disk_array_prefix_size(boot_disks);
	if (count) {
		swap_position = (swap_position + 1) % count;
		load_boot_disks(swap_position);
	}
	swapping_requested = true;
}

uint8_t imageDisk::Read_Sector(uint32_t head, uint32_t cylinder,
                               uint32_t sector, void* data)
{
	if (track_image) {
		uint8_t status = 0x04;
		for (const auto& candidate : track_image->GetTrack(cylinder, head)) {
			if (candidate.cylinder != cylinder ||
			    candidate.head != head || candidate.id != sector ||
			    candidate.data.size() != sector_size ||
			    candidate.deleted) {
				continue;
			}
			if (candidate.id_crc_error || candidate.data_crc_error) {
				status = 0x10;
				continue;
			}
			std::copy(candidate.data.begin(),
			          candidate.data.end(),
			          static_cast<uint8_t*>(data));
			return 0;
		}
		return status;
	}

	uint32_t sectnum;

	sectnum = ((cylinder * heads + head) * sectors) + sector - 1L;

	return Read_AbsoluteSector(sectnum, data);
}

uint8_t imageDisk::Read_AbsoluteSector(uint32_t sectnum, void* data)
{
	if (track_image) {
		if (!sectors || sectnum >= cylinders * heads * sectors) {
			return 0x04;
		}
		const auto track = sectnum / sectors;
		return Read_Sector(track % heads,
		                   track / heads,
		                   sectnum % sectors + 1,
		                   data);
	}
	return read_absolute_sector(sectnum, data, false);
}

bool imageDisk::ReadSectorForController(const uint32_t head, const uint32_t cylinder,
                                        const uint32_t sector,
                                        const std::span<uint8_t> data)
{
	if (data.size() != sector_size) {
		return false;
	}
	const auto absolute_sector = (cylinder * heads + head) * sectors + sector - 1;
	return read_absolute_sector(absolute_sector, data.data(), true) == 0;
}

uint8_t imageDisk::read_absolute_sector(const uint32_t sectnum, void* data,
                                        const bool controller_read)
{
	const auto bytenum = check_cast<cross_off_t>(sectnum) * sector_size;

	if (last_action == WRITE || bytenum != current_fpos) {
		if (cross_fseeko(diskimg, bytenum, SEEK_SET) != 0) {
			LOG_ERR("BIOSDISK: Could not seek to sector %u in file '%s': %s",
			        sectnum,
			        diskname,
			        strerror(errno));
			return 0xff;
		}
	}

	// Only perform delay if we booted from a disk image
	// Otherwise this would result in delay duplication in the int21 handler
	if (!controller_read && DOS_IsGuestOsBooted()) {
		DiskType type = hardDrive ? DiskType::HardDisk : DiskType::Floppy;
		DOS_PerformDiskIoDelay(sector_size, type);
	}

	size_t ret   = fread(data, 1, sector_size, diskimg);
	current_fpos = bytenum + ret;
	last_action  = READ;

	return controller_read && ret != sector_size ? 0xff : 0;
}

uint8_t imageDisk::Write_Sector(uint32_t head, uint32_t cylinder,
                                uint32_t sector, void* data)
{
	if (track_image) {
		if (IsWriteProtected()) {
			return 0x03;
		}
		const std::span bytes(static_cast<const uint8_t*>(data), sector_size);
		return track_image->WriteScpSector(cylinder, head, sector, bytes)
		             ? 0
		             : 0x05;
	}
	uint32_t sectnum;

	sectnum = ((cylinder * heads + head) * sectors) + sector - 1L;

	return Write_AbsoluteSector(sectnum, data);
}

uint8_t imageDisk::Write_AbsoluteSector(uint32_t sectnum, void* data)
{
	if (track_image) {
		if (!sectors || sectnum >= cylinders * heads * sectors) {
			return 0x04;
		}
		const auto track = sectnum / sectors;
		return Write_Sector(track % heads,
		                    track / heads,
		                    sectnum % sectors + 1,
		                    data);
	}
	return write_absolute_sector(sectnum, data, false);
}

bool imageDisk::IsWriteProtected() const
{
	return read_only || (track_image && track_image->IsWriteProtected());
}

bool imageDisk::WriteSectorForController(const uint32_t head, const uint32_t cylinder,
                                         const uint32_t sector,
                                         const std::span<const uint8_t> data)
{
	if (data.size() != sector_size) {
		return false;
	}
	if (track_image) {
		return track_image->WriteScpSector(cylinder, head, sector, data);
	}
	const auto absolute_sector = (cylinder * heads + head) * sectors + sector - 1;
	return write_absolute_sector(absolute_sector, data.data(), true) == 0;
}

uint8_t imageDisk::write_absolute_sector(const uint32_t sectnum, const void* data,
                                         const bool controller_write)
{
	if (IsWriteProtected()) {
		return 0x03;
	}

	const auto bytenum = check_cast<cross_off_t>(sectnum) * sector_size;

	// LOG_MSG("Writing sectors to %ld at bytenum %d", sectnum, bytenum);

	if (last_action == READ || bytenum != current_fpos) {
		if (cross_fseeko(diskimg, bytenum, SEEK_SET) != 0) {
			LOG_ERR("BIOSDISK: Could not seek to byte %lld in file '%s': %s",
			        static_cast<long long int>(bytenum),
			        diskname,
			        strerror(errno));
			return 0xff;
		}
	}

	// Only perform delay if we booted from a disk image
	// Otherwise this would result in delay duplication in the int21 handler
	if (!controller_write && DOS_IsGuestOsBooted()) {
		DiskType type = hardDrive ? DiskType::HardDisk : DiskType::Floppy;
		DOS_PerformDiskIoDelay(sector_size, type);
	}

	size_t ret   = fwrite(data, 1, sector_size, diskimg);
	current_fpos = bytenum + ret;
	last_action  = WRITE;

	if (controller_write) {
		return ret == sector_size && fflush(diskimg) == 0 ? 0 : 0x05;
	}
	return ((ret > 0) ? 0x00 : 0x05);
}

imageDisk::imageDisk(FILE* img_file, const char* img_name, uint32_t img_size_k,
                     bool is_hdd, bool is_read_only)
        : hardDrive(is_hdd),
          active(false),
          diskimg(img_file),
          floppytype(0),
          sector_size(512),
          heads(0),
          cylinders(0),
          sectors(0),
          read_only(is_read_only),
          current_fpos(0),
          last_action(NONE)
{
	memset(diskname, 0, 512);
	safe_strcpy(diskname, img_name);
	if (!diskimg) {
		return;
	}
	fseek(diskimg, 0, SEEK_SET);
	if (!is_hdd) {
		bool founddisk = false;
		for (uint8_t i = 0; i < disk_geometry_list.size(); ++i) {
			const auto& geo = disk_geometry_list[i];
			if (geo.ksize == img_size_k || geo.ksize + 1 == img_size_k) {
				if (geo.ksize != img_size_k) {
					LOG_MSG("ImageLoader: image file with additional data, might not load!");
				}
				founddisk  = true;
				active     = true;
				floppytype = i;
				heads      = geo.headscyl;
				cylinders  = geo.cylcount;
				sectors    = geo.secttrack;
				break;
			}
		}
		if (!founddisk) {
			active = false;
		} else {
			incrementFDD();
		}
	}
}

imageDisk::imageDisk(std::unique_ptr<FloppyTrackImage> image, const std::string& name)
        : imageDisk(nullptr, name.c_str(), 0, false)
{
	heads       = image->heads;
	cylinders   = image->cylinders;
	sectors     = image->logical_sectors;
	track_image = std::move(image);
	active      = true;
	for (size_t i = 0; i < disk_geometry_list.size(); ++i) {
		const auto& geometry = disk_geometry_list[i];
		if (geometry.cylcount == cylinders &&
		    geometry.headscyl == heads && geometry.secttrack == sectors) {
			floppytype = check_cast<uint8_t>(i);
			break;
		}
	}
	incrementFDD();
}

const FloppyTrackImage* imageDisk::GetTrackImage() const
{
	return track_image.get();
}

void imageDisk::Set_Geometry(uint32_t setHeads, uint32_t setCyl,
                             uint32_t setSect, uint32_t setSectSize)
{
	heads       = setHeads;
	cylinders   = setCyl;
	sectors     = setSect;
	sector_size = setSectSize;
	active      = true;
}

void imageDisk::Get_Geometry(uint32_t* getHeads, uint32_t* getCyl,
                             uint32_t* getSect, uint32_t* getSectSize)
{
	*getHeads    = heads;
	*getCyl      = cylinders;
	*getSect     = sectors;
	*getSectSize = sector_size;
}

uint8_t imageDisk::GetBiosType(void)
{
	if (!hardDrive) {
		return static_cast<uint8_t>(disk_geometry_list[floppytype].biosval);
	} else {
		return 0;
	}
}

uint32_t imageDisk::getSectSize(void)
{
	return sector_size;
}

static uint8_t GetDosDriveNumber(uint8_t biosNum)
{
	switch (biosNum) {
	case 0x0: return 0x0;
	case 0x1: return 0x1;
	case 0x80: return 0x2;
	case 0x81: return 0x3;
	case 0x82: return 0x4;
	case 0x83: return 0x5;
	default: return 0x7f;
	}
}

static bool driveInactive(uint8_t driveNum)
{
	if (driveNum >= MAX_DISK_IMAGES) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d non-existent", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	if (!disk_slots[driveNum]) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d not active", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	if (!disk_slots[driveNum]->active) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d not active", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	return false;
}

template <typename T, size_t N>
static bool has_image(const std::array<T, N>& arr)
{
	auto to_bool = [](const T& x) { return bool(x); };
	return std::any_of(std::begin(arr), std::end(arr), to_bool);
}

static Bitu INT13_DiskHandler(void)
{
	uint16_t segat, bufptr;
	uint8_t sectbuf[512];
	uint8_t drivenum;
	Bitu t;
	last_drive            = reg_dl;
	drivenum              = GetDosDriveNumber(reg_dl);
	const bool any_images = has_image(disk_slots);

	// unconditionally enable the interrupt flag
	CALLBACK_SIF(true);

	// drivenum = 0;
	// LOG_MSG("INT13: Function %x called on drive %x (dos drive %d)",
	// reg_ah,  reg_dl, drivenum);

	// NOTE: the 0xff error code returned in some cases is questionable;
	// 0x01 seems more correct
	switch (reg_ah) {
	case 0x0: /* Reset disk */
	{
		/* if there aren't any diskimages (so only localdrives and
		 * virtual drives) always succeed on reset disk. If there are
		 * diskimages then and only then do real checks
		 */
		if (any_images && driveInactive(drivenum)) {
			/* driveInactive sets carry flag if the specified drive
			 * is not available */
			if (is_machine_cga() || is_machine_pcjr()) {
				/* those bioses call floppy drive reset for
				 * invalid drive values */
				if (((disk_slots[0]) && (disk_slots[0]->active)) ||
				    ((disk_slots[1]) && (disk_slots[1]->active))) {
					if (!is_machine_pcjr() && reg_dl < 0x80) {
						reg_ip++;
					}
					last_status = 0x00;
					CALLBACK_SCF(false);
				}
			}
			return CBRET_NONE;
		}
		if (!is_machine_pcjr() && reg_dl < 0x80) {
			reg_ip++;
		}
		last_status = 0x00;
		CALLBACK_SCF(false);
	} break;
	case 0x1: /* Get status of last operation */

		if (last_status != 0x00) {
			reg_ah = last_status;
			CALLBACK_SCF(true);
		} else {
			reg_ah = 0x00;
			CALLBACK_SCF(false);
		}
		break;
	case 0x2: /* Read sectors */
		if (reg_al == 0) {
			reg_ah = 0x01;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		if (drivenum >= MAX_DISK_IMAGES || !disk_slots[drivenum]) {
			if (drivenum >= DOS_DRIVES || !Drives[drivenum] ||
			    Drives[drivenum]->IsRemovable()) {
				reg_ah = 0x01;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			// Inherit the Earth cdrom and Amberstar use it as a
			// disk test
			if (((reg_dl & 0x80) == 0x80) && (reg_dh == 0) &&
			    ((reg_cl & 0x3f) == 1)) {
				if (reg_ch == 0) {
					// write some MBR data into buffer for
					// Amberstar installer
					real_writeb(SegValue(es),
					            reg_bx + 0x1be,
					            0x80); // first partition is
					                   // active
					real_writeb(SegValue(es),
					            reg_bx + 0x1c2,
					            0x06); // first partition is
					                   // FAT16B
				}
				reg_ah = 0;
				CALLBACK_SCF(false);
				return CBRET_NONE;
			}
		}
		if (driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}

		segat  = SegValue(es);
		bufptr = reg_bx;
		for (Bitu i = 0; i < reg_al; i++) {
			last_status = disk_slots[drivenum]->Read_Sector(
			        (uint32_t)reg_dh,
			        (uint32_t)(reg_ch | ((reg_cl & 0xc0) << 2)),
			        (uint32_t)((reg_cl & 63) + i),
			        sectbuf);
			if ((last_status != 0x00) || killRead) {
				LOG_MSG("Error in disk read");
				killRead = false;
				reg_ah   = 0x04;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			for (t = 0; t < 512; t++) {
				real_writeb(segat, bufptr, sectbuf[t]);
				bufptr++;
			}
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x3: /* Write sectors */
		if (driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		bufptr = reg_bx;
		for (Bitu i = 0; i < reg_al; i++) {
			for (t = 0; t < disk_slots[drivenum]->getSectSize(); t++) {
				sectbuf[t] = real_readb(SegValue(es), bufptr);
				bufptr++;
			}
			last_status = disk_slots[drivenum]->Write_Sector(
			        (uint32_t)reg_dh,
			        (uint32_t)(reg_ch | ((reg_cl & 0xc0) << 2)),
			        (uint32_t)((reg_cl & 63) + i),
			        &sectbuf[0]);
			if (last_status != 0x00) {
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x04: /* Verify sectors */
		if (reg_al == 0) {
			reg_ah = 0x01;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		if (driveInactive(drivenum)) {
			reg_ah = last_status;
			return CBRET_NONE;
		}

		if (disk_slots[drivenum]->GetTrackImage()) {
			const auto cylinder = static_cast<uint32_t>(
			        reg_ch | ((reg_cl & 0xc0) << 2));
			const auto first_sector = static_cast<uint32_t>(reg_cl & 0x3f);
			for (uint8_t verified = 0; verified < reg_al; ++verified) {
				last_status = disk_slots[drivenum]->Read_Sector(
				        reg_dh, cylinder, first_sector + verified, sectbuf);
				if (last_status) {
					reg_ah = last_status;
					reg_al = verified;
					CALLBACK_SCF(true);
					return CBRET_NONE;
				}
			}
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x05: /* Format track */
		if (driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x08: /* Get drive parameters */
		if (driveInactive(drivenum)) {
			last_status = 0x07;
			reg_ah      = last_status;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		reg_ax = 0x00;
		reg_bl = disk_slots[drivenum]->GetBiosType();
		uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
		disk_slots[drivenum]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		if (tmpcyl == 0) {
			LOG(LOG_BIOS,
			    LOG_ERROR)("INT13 DrivParm: cylinder count zero!");
		} else {
			tmpcyl--; // cylinder count -> max cylinder
		}
		if (tmpheads == 0) {
			LOG(LOG_BIOS, LOG_ERROR)("INT13 DrivParm: head count zero!");
		} else {
			tmpheads--; // head count -> max head
		}
		reg_ch = (uint8_t)(tmpcyl & 0xff);
		reg_cl = (uint8_t)(((tmpcyl >> 2) & 0xc0) | (tmpsect & 0x3f));
		reg_dh = (uint8_t)tmpheads;
		last_status = 0x00;
		if (reg_dl & 0x80) { // harddisks
			reg_dl = 0;
			if (disk_slots[2]) {
				reg_dl++;
			}
			if (disk_slots[3]) {
				reg_dl++;
			}
		} else { // floppy disks
			reg_dl = 0;
			if (disk_slots[0]) {
				reg_dl++;
			}
			if (disk_slots[1]) {
				reg_dl++;
			}
		}
		CALLBACK_SCF(false);
		break;
	case 0x11: /* Recalibrate drive */
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x15: /* Get disk type */
		/* Korean Powerdolls uses this to detect harddrives */
		LOG(LOG_BIOS, LOG_WARN)("INT13: Get disktype used!");
		if (any_images) {
			if (driveInactive(drivenum)) {
				last_status = 0x07;
				reg_ah      = last_status;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}

			uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
			disk_slots[drivenum]->Get_Geometry(&tmpheads,
			                                   &tmpcyl,
			                                   &tmpsect,
			                                   &tmpsize);
			// Store intermediate calculations in 64-bit to avoid
			// accidental integer overflow on temporary value:
			uint64_t largesize = tmpheads;
			largesize *= tmpcyl;
			largesize *= tmpsect;
			largesize *= tmpsize;
			const auto ts = static_cast<uint32_t>(largesize / 512);

			reg_ah = (drivenum < 2) ? 1 : 3; // With 2 for floppy
			                                 // MSDOS starts calling
			                                 // int 13 ah 16
			if (reg_ah == 3) {
				reg_cx = static_cast<uint16_t>(ts >> 16);
				reg_dx = static_cast<uint16_t>(ts & 0xffff);
			}
			CALLBACK_SCF(false);
		} else {
			if (drivenum < DOS_DRIVES &&
			    (Drives[drivenum] != nullptr || drivenum < 2)) {
				if (drivenum < 2) {
					// TODO use actual size (using 1.44 for
					// now).
					reg_ah = 0x1; // type
					              //					reg_cx
					//= 0; 					reg_dx = 2880; //Only set size
					//for harddrives.
				} else {
					// TODO use actual size (using 105 mb
					// for now).
					reg_ah = 0x3; // type
					reg_cx = 3;
					reg_dx = 0x4800;
				}
				CALLBACK_SCF(false);
			} else {
				LOG(LOG_BIOS, LOG_WARN)(
				        "INT13: no images, but invalid drive for call 15");
				reg_ah = 0xff;
				CALLBACK_SCF(true);
			}
		}
		break;
	case 0x17: /* Set disk type for format */
		/* Pirates! needs this to load */
		killRead = true;
		reg_ah   = 0x00;
		CALLBACK_SCF(false);
		break;
	default:
		LOG(LOG_BIOS,
		    LOG_ERROR)("INT13: Function %x called on drive %x (dos drive %d)",
		               reg_ah,
		               reg_dl,
		               drivenum);
		reg_ah = 0xff;
		CALLBACK_SCF(true);
	}
	return CBRET_NONE;
}

void BIOS_SetupDisks(void)
{
	FLOPPY_Init();
	/* TODO Start the time correctly */
	call_int13 = CALLBACK_Allocate();
	CALLBACK_Setup(call_int13, &INT13_DiskHandler, CB_INT13, "Int 13 Bios disk");
	RealSetVec(0x13, CALLBACK_RealPointer(call_int13));

	// Clean any the numbered images
	disk_slots.fill(nullptr);

	// Clear any raw disk images
	boot_disks.fill(nullptr);
	swap_position      = 0;
	swapping_requested = false;

	diskparm0    = CALLBACK_Allocate();
	diskparm1    = CALLBACK_Allocate();

	RealSetVec(0x41, CALLBACK_RealPointer(diskparm0));
	RealSetVec(0x46, CALLBACK_RealPointer(diskparm1));

	PhysPt dp0physaddr = CALLBACK_PhysPointer(diskparm0);
	PhysPt dp1physaddr = CALLBACK_PhysPointer(diskparm1);
	for (int i = 0; i < 16; i++) {
		phys_writeb(dp0physaddr + i, 0);
		phys_writeb(dp1physaddr + i, 0);
	}

	imgDTASeg = 0;

	/* Setup the Bios Area */
	mem_writeb(BIOS_HARDDISK_COUNT, 2);

	MAPPER_AddHandler(swap_disks, SDL_SCANCODE_F4, PRIMARY_MOD, "swapimg", "Swap Image");
	killRead           = false;
	swapping_requested = false;
}
