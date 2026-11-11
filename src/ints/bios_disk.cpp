// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bios_disk.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>
#include <vector>

#include "cpu/callback.h"
#include "cpu/registers.h"
#include "dos/dos.h" /* for Drives[] */
#include "dos/drives.h"
#include "gui/mapper.h"
#include "hardware/floppy_controller.h"
#include "hardware/memory.h"
#include "ints/floppy_media.h"
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
struct BiosDiskSlot {
	std::shared_ptr<imageDisk> sector   = {};
	std::shared_ptr<FloppyMedia> floppy = {};
};

static std::array<BiosDiskSlot, MAX_DISK_IMAGES> disk_slots = {};
static BiosBootDisks boot_disks                             = {};

static size_t swap_position = 0;

static void update_disk_parameter_tables()
{
	uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
	if (disk_slots[2].sector) {
		PhysPt dp0physaddr = CALLBACK_PhysPointer(diskparm0);
		disk_slots[2].sector->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp0physaddr, (uint16_t)tmpcyl);
		phys_writeb(dp0physaddr + 0x2, (uint8_t)tmpheads);
		phys_writew(dp0physaddr + 0x3, 0);
		phys_writew(dp0physaddr + 0x5, (uint16_t)-1);
		phys_writeb(dp0physaddr + 0x7, 0);
		phys_writeb(dp0physaddr + 0x8,
		            (0xc0 | (((disk_slots[2].sector->heads) > 8) << 3)));
		phys_writeb(dp0physaddr + 0x9, 0);
		phys_writeb(dp0physaddr + 0xa, 0);
		phys_writeb(dp0physaddr + 0xb, 0);
		phys_writew(dp0physaddr + 0xc, (uint16_t)tmpcyl);
		phys_writeb(dp0physaddr + 0xe, (uint8_t)tmpsect);
	}
	if (disk_slots[3].sector) {
		PhysPt dp1physaddr = CALLBACK_PhysPointer(diskparm1);
		disk_slots[3].sector->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp1physaddr, (uint16_t)tmpcyl);
		phys_writeb(dp1physaddr + 0x2, (uint8_t)tmpheads);
		phys_writeb(dp1physaddr + 0xe, (uint8_t)tmpsect);
	}
}

std::shared_ptr<imageDisk> BIOS_GetDisk(const size_t slot)
{
	return slot < disk_slots.size() ? disk_slots[slot].sector : nullptr;
}

std::shared_ptr<FloppyMedia> BIOS_GetFloppyMedia(const size_t slot)
{
	return slot < disk_slots.size() ? disk_slots[slot].floppy : nullptr;
}

static uint8_t floppy_drive_count()
{
	return check_cast<uint8_t>(bool(disk_slots[0].sector) +
	                           bool(disk_slots[1].sector));
}

static void update_floppy_equipment()
{
	constexpr uint16_t floppy_bits = 0x00c1;
	auto equipment   = static_cast<uint16_t>(mem_readw(BIOS_CONFIGURATION) &
                                               ~floppy_bits);
	const auto count = floppy_drive_count();
	if (count) {
		equipment |= check_cast<uint16_t>(1 | ((count - 1) << 6));
	}
	BIOS_SetEquipment(equipment);
}

static void set_disk_slot(const size_t slot, std::shared_ptr<imageDisk> disk)
{
	if (slot < 2 && disk_slots[slot].sector != disk) {
		FLOPPY_SetDiskChanged(slot, true);
	}
	auto floppy      = slot < 2 && disk ? disk->GetFloppyMedia() : nullptr;
	disk_slots[slot] = {std::move(disk), std::move(floppy)};
	if (slot < 2) {
		update_floppy_equipment();
	} else {
		if (!disk_slots[slot].sector) {
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
	if (slot < 2 && disk_slots[slot].sector != disk) {
		// Prevent a later BOOT swap from restoring an ejected image.
		boot_disks.fill(nullptr);
		swap_position = 0;
	}
	set_disk_slot(slot, std::move(disk));
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
	if (floppy_media) {
		uint8_t status = 0x04;
		for (const auto& candidate : floppy_media->GetTrack(cylinder, head)) {
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
	if (floppy_media) {
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
	if (floppy_media) {
		if (IsWriteProtected()) {
			return 0x03;
		}
		const std::span bytes(static_cast<const uint8_t*>(data), sector_size);
		return floppy_media->WriteSector(cylinder, head, sector, bytes)
		             ? 0
		             : 0x05;
	}
	uint32_t sectnum;

	sectnum = ((cylinder * heads + head) * sectors) + sector - 1L;

	return Write_AbsoluteSector(sectnum, data);
}

uint8_t imageDisk::Write_AbsoluteSector(uint32_t sectnum, void* data)
{
	if (floppy_media) {
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
	return read_only || (floppy_media && floppy_media->IsWriteProtected());
}

bool imageDisk::WriteSectorForController(const uint32_t head, const uint32_t cylinder,
                                         const uint32_t sector,
                                         const std::span<const uint8_t> data)
{
	if (data.size() != sector_size || floppy_media) {
		return false;
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
		}
	}
}

imageDisk::imageDisk(std::shared_ptr<FloppyMedia> image, const std::string& name,
                     const uint32_t head_count, const uint32_t cylinder_count,
                     const uint32_t sectors_per_track)
        : imageDisk(nullptr, name.c_str(), 0, false)
{
	heads        = head_count;
	cylinders    = cylinder_count;
	sectors      = sectors_per_track;
	floppy_media = std::move(image);
	active       = true;
	for (size_t i = 0; i < disk_geometry_list.size(); ++i) {
		const auto& geometry = disk_geometry_list[i];
		if (geometry.cylcount == cylinders &&
		    geometry.headscyl == heads && geometry.secttrack == sectors) {
			floppytype = check_cast<uint8_t>(i);
			break;
		}
	}
}

std::shared_ptr<FloppyMedia> imageDisk::GetFloppyMedia() const
{
	return floppy_media;
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

uint8_t BIOS_GetFloppyCmosType(const size_t slot)
{
	const auto disk = slot < 2 ? BIOS_GetDisk(slot) : nullptr;
	if (!disk) {
		return 0;
	}
	const auto type = disk->GetBiosType();
	// CMOS uses 5 for 2.88 MB and has no separate single-sided type.
	if (type == 6) {
		return 5;
	}
	return type == 0 ? 1 : type;
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

static bool drive_inactive(const uint8_t drive_num)
{
	if (drive_num >= MAX_DISK_IMAGES) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d non-existent", drive_num);
		last_status = 0x01;
		return true;
	}
	if (!disk_slots[drive_num].sector) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d not active", drive_num);
		last_status = 0x01;
		return true;
	}
	if (!disk_slots[drive_num].sector->active) {
		LOG(LOG_BIOS, LOG_ERROR)("Disk %d not active", drive_num);
		last_status = 0x01;
		return true;
	}
	return false;
}

static uint8_t format_floppy_track(imageDisk& disk, const uint8_t unit,
                                   const uint32_t cylinder, const uint8_t head,
                                   const uint8_t count, const RealPt table)
{
	if (!count) {
		return 0x01;
	}
	if (disk.IsWriteProtected()) {
		return 0x03;
	}

	constexpr size_t IdSize = 4;
	const auto table_size   = count * IdSize;
	const auto address      = RealToPhysical(table);
	const auto use_dma      = !is_machine_pcjr_or_tandy();
	if (use_dma && table_size > 0x10000u - (address & 0xffff)) {
		return 0x09;
	}

	const auto parameter_table = RealGetVec(0x1e);
	if (!parameter_table) {
		return 0x01;
	}
	const auto params    = RealToPhysical(parameter_table);
	const auto size_code = mem_readb(params + 3);
	const auto fill      = mem_readb(params + 8);
	if (size_code > 7) {
		return 0x01;
	}
	for (size_t i = IdSize - 1; i < table_size; i += IdSize) {
		if (mem_readb(address + check_cast<PhysPt>(i)) != size_code) {
			return 0x04;
		}
	}
	if (use_dma) {
		const auto gap = mem_readb(params + 7);
		return FLOPPY_BiosDmaFormat(
		        unit, cylinder, head, size_code, count, gap, fill, address);
	}

	// A pending FDC command can hold a pointer into the track being formatted.
	if (!FLOPPY_CanStartBiosCommand()) {
		return 0x80;
	}
	std::array<uint8_t, UINT8_MAX * IdSize> buffer = {};
	const auto ids = std::span(buffer).first(table_size);
	for (size_t i = 0; i < ids.size(); ++i) {
		ids[i] = mem_readb(address + check_cast<PhysPt>(i));
	}

	switch (FLOPPY_FormatTrack(
	        disk, disk_slots[unit].floppy.get(), cylinder, head, ids, fill, count)) {
	case FloppyFormatResult::Success: return 0;
	case FloppyFormatResult::WriteProtected: return 0x03;
	case FloppyFormatResult::InvalidLayout: return 0x04;
	case FloppyFormatResult::WriteError: return 0x20;
	}
	return 0x20;
}

struct DiskServiceRequest {
	uint8_t function  = 0;
	uint8_t drive     = 0;
	uint8_t count     = 0;
	uint8_t head      = 0;
	uint8_t sector    = 0;
	uint16_t cylinder = 0;
	RealPt buffer     = 0;
};

struct DiskServiceResult {
	struct DriveParameters {
		uint32_t max_cylinder      = 0;
		uint32_t max_head          = 0;
		uint32_t sectors_per_track = 0;
		uint8_t bios_type          = 0;
		uint8_t drive_count        = 0;
	};

	std::optional<uint8_t> status              = {};
	bool carry                                 = false;
	std::optional<uint8_t> completed           = {};
	std::optional<DriveParameters> parameters  = {};
	std::optional<uint32_t> fixed_disk_sectors = {};
};

static FloppyBiosTransferResult bios_track_sector_transfer(
        const DiskServiceRequest& request, const uint8_t unit,
        const FloppyBiosTransfer operation)
{
	const auto& disk = *disk_slots[unit].sector;
	if (!request.count) {
		return {0x01, 0};
	}
	if (operation == FloppyBiosTransfer::Write && disk.IsWriteProtected()) {
		return {0x03, 0};
	}

	std::array<uint8_t, 512> data = {};
	auto cylinder                 = uint32_t{request.cylinder};
	auto head                     = uint32_t{request.head};
	auto sector                   = uint32_t{request.sector};
	auto offset                   = RealOffset(request.buffer);
	const auto segment            = RealSegment(request.buffer);
	// Protection tracks can contain IDs beyond the logical geometry.
	const auto cross_tracks = sector <= disk.sectors;
	for (uint8_t completed = 0; completed < request.count; ++completed) {
		if (cylinder >= disk.cylinders || head >= disk.heads ||
		    !sector || sector > UINT8_MAX) {
			return {0x04, completed};
		}
		FLOPPY_BiosPosition(unit, check_cast<uint8_t>(cylinder));
		if (operation == FloppyBiosTransfer::Write) {
			for (auto& byte : data) {
				byte = real_readb(segment, offset++);
			}
			const auto status = disk_slots[unit].sector->Write_Sector(
			        head, cylinder, sector, data.data());
			if (status) {
				return {status, completed};
			}
		} else {
			const auto status = disk_slots[unit].sector->Read_Sector(
			        head, cylinder, sector, data.data());
			if (status) {
				return {status, completed};
			}
			if (operation == FloppyBiosTransfer::Read) {
				for (const auto byte : data) {
					real_writeb(segment, offset++, byte);
				}
			}
		}
		if (++sector > disk.sectors && cross_tracks) {
			sector = 1;
			if (++head == disk.heads) {
				head = 0;
				++cylinder;
			}
		}
	}
	FLOPPY_SetDiskChanged(unit, false);
	return {0, request.count};
}

static DiskServiceResult bios_floppy_transfer(const DiskServiceRequest& request,
                                              const uint8_t drivenum,
                                              const FloppyBiosTransfer operation)
{
	auto transfer = FloppyBiosTransferResult{0x04, 0};
	if (operation != FloppyBiosTransfer::Read || !killRead) {
		if (is_machine_pcjr_or_tandy()) {
			transfer = bios_track_sector_transfer(request,
			                                      drivenum,
			                                      operation);
		} else {
			const auto parameters = RealToPhysical(RealGetVec(0x1e));
			transfer = FLOPPY_BiosTransfer(operation,
			                               drivenum,
			                               request.cylinder,
			                               request.head,
			                               request.sector,
			                               mem_readb(parameters + 3),
			                               request.count,
			                               mem_readb(parameters + 5),
			                               RealToPhysical(request.buffer));
		}
	}
	if (operation == FloppyBiosTransfer::Read) {
		killRead = false;
	}
	last_status = transfer.status;
	mem_writeb(BIOS_DISK_STATUS, transfer.status);
	return {.status    = transfer.status,
	        .carry     = transfer.status != 0,
	        .completed = transfer.sectors};
}

static DiskServiceResult run_disk_service(const DiskServiceRequest& request)
{
	uint16_t bufptr                  = 0;
	std::array<uint8_t, 512> sectbuf = {};
	DiskServiceResult result         = {};
	last_drive                       = request.drive;
	const auto unit                  = GetDosDriveNumber(request.drive);
	const auto any_images = std::ranges::any_of(disk_slots, [](const auto& slot) {
		return slot.sector != nullptr;
	});

	// NOTE: the 0xff error code returned in some cases is questionable;
	// 0x01 seems more correct
	switch (request.function) {
	case 0x0: /* Reset disk */
	{
		/* if there aren't any diskimages (so only localdrives and
		 * virtual drives) always succeed on reset disk. If there are
		 * diskimages then and only then do real checks
		 */
		if (any_images && drive_inactive(unit)) {
			result.carry = true;
			if (is_machine_cga() || is_machine_pcjr()) {
				/* those bioses call floppy drive reset for
				 * invalid drive values */
				if ((disk_slots[0].sector &&
				     disk_slots[0].sector->active) ||
				    (disk_slots[1].sector &&
				     disk_slots[1].sector->active)) {
					if (!is_machine_pcjr() && request.drive < 2) {
						FLOPPY_BiosReset(request.drive);
					}
					last_status = 0x00;
					result.carry = false;
				}
			}
			return result;
		}
		if (!is_machine_pcjr() && request.drive < 2) {
			FLOPPY_BiosReset(request.drive);
		}
		last_status = 0x00;
		break;
	}
	case 0x1: // Get status of last operation
		return {.status = last_status, .carry = last_status != 0};
	case 0x2: { // Read sectors
		if (request.count == 0) {
			return {.status = 0x01, .carry = true};
		}
		if (unit >= MAX_DISK_IMAGES || !disk_slots[unit].sector) {
			if (unit >= DOS_DRIVES || !Drives[unit] ||
			    Drives[unit]->IsRemovable()) {
				return {.status = 0x01, .carry = true};
			}
			// Inherit the Earth cdrom and Amberstar use it as a
			// disk test
			if (((request.drive & 0x80) == 0x80) &&
			    (request.head == 0) && (request.sector == 1)) {
				if ((request.cylinder & 0xff) == 0) {
					// write some MBR data into buffer for
					// Amberstar installer
					real_writeb(RealSegment(request.buffer),
					            RealOffset(request.buffer) + 0x1be,
					            0x80); // first partition is
					                   // active
					real_writeb(RealSegment(request.buffer),
					            RealOffset(request.buffer) + 0x1c2,
					            0x06); // first partition is
					                   // FAT16B
				}
				result.status = 0;
				result.carry  = false;
				return result;
			}
		}
		if (drive_inactive(unit)) {
			return {.status = 0xff, .carry = true};
		}

		if (disk_slots[unit].floppy) {
			return bios_floppy_transfer(request,
			                            unit,
			                            FloppyBiosTransfer::Read);
		}

		const auto segat = RealSegment(request.buffer);
		bufptr           = RealOffset(request.buffer);
		for (uint32_t i = 0; i < request.count; ++i) {
			last_status = disk_slots[unit].sector->Read_Sector(
			        request.head,
			        request.cylinder,
			        request.sector + i,
			        sectbuf.data());
			if ((last_status != 0x00) || killRead) {
				LOG_MSG("Error in disk read");
				killRead = false;
				return {.status = 0x04, .carry = true};
			}
			for (size_t t = 0; t < 512; t++) {
				real_writeb(segat, bufptr, sectbuf[t]);
				bufptr++;
			}
		}
		if (unit < 2) {
			FLOPPY_SetDiskChanged(unit, false);
		}
		result.status = 0x00;
		result.carry  = false;
		break;
	}
	case 0x3: /* Write sectors */
		if (drive_inactive(unit)) {
			return {.status = 0xff, .carry = true};
		}
		if (disk_slots[unit].floppy) {
			return bios_floppy_transfer(request,
			                            unit,
			                            FloppyBiosTransfer::Write);
		}
		bufptr = RealOffset(request.buffer);
		for (uint32_t i = 0; i < request.count; ++i) {
			for (size_t t = 0;
			     t < disk_slots[unit].sector->getSectSize();
			     t++) {
				sectbuf[t] = real_readb(RealSegment(request.buffer),
				                        bufptr);
				bufptr++;
			}
			last_status = disk_slots[unit].sector->Write_Sector(
			        request.head,
			        request.cylinder,
			        request.sector + i,
			        sectbuf.data());
			if (last_status != 0x00) {
				result.carry = true;
				return result;
			}
		}
		if (unit < 2 && request.count) {
			FLOPPY_SetDiskChanged(unit, false);
		}
		result.status = 0x00;
		result.carry  = false;
		break;
	case 0x04: /* Verify sectors */
		if (request.count == 0) {
			return {.status = 0x01, .carry = true};
		}
		if (drive_inactive(unit)) {
			return {.status = last_status, .carry = true};
		}

		if (disk_slots[unit].floppy) {
			return bios_floppy_transfer(request,
			                            unit,
			                            FloppyBiosTransfer::Verify);
		}
		result.status = 0x00;
		result.carry  = false;
		break;
	case 0x05: /* Format track */
		if (drive_inactive(unit)) {
			last_status = 0xff;
		} else if (disk_slots[unit].sector->hardDrive) {
			// Preserve the existing fixed-disk BIOS behavior.
			last_status = 0;
		} else {
			last_status = format_floppy_track(*disk_slots[unit].sector,
			                                  unit,
			                                  request.cylinder,
			                                  request.head,
			                                  request.count,
			                                  request.buffer);
			mem_writeb(BIOS_DISK_STATUS, last_status);
		}
		result.status = last_status;
		result.carry  = last_status != 0;
		break;
	case 0x08: { // Get drive parameters
		if (drive_inactive(unit)) {
			last_status = 0x07;
			return {.status = last_status, .carry = true};
		}
		uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
		disk_slots[unit].sector->Get_Geometry(&tmpheads,
		                                      &tmpcyl,
		                                      &tmpsect,
		                                      &tmpsize);
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
		const auto drive_count = request.drive & 0x80
		                               ? check_cast<uint8_t>(
		                                         bool(disk_slots[2].sector) +
		                                         bool(disk_slots[3].sector))
		                               : floppy_drive_count();
		result.parameters      = DiskServiceResult::DriveParameters{
                        tmpcyl,
                        tmpheads,
                        tmpsect,
                        disk_slots[unit].sector->GetBiosType(),
                        drive_count};
		last_status  = 0;
		result.carry = false;
		break;
	}
	case 0x11: /* Recalibrate drive */
		result.status = 0x00;
		result.carry  = false;
		break;
	case 0x15: /* Get disk type */
		/* Korean Powerdolls uses this to detect harddrives */
		LOG(LOG_BIOS, LOG_WARN)("INT13: Get disktype used!");
		if (any_images) {
			if (drive_inactive(unit)) {
				last_status = 0x07;
				return {.status = last_status, .carry = true};
			}

			uint32_t tmpheads, tmpcyl, tmpsect, tmpsize;
			disk_slots[unit].sector->Get_Geometry(&tmpheads,
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

			// DOS probes AH=16h if the floppy is reported as type 2.
			result.status = unit < 2 ? 1 : 3;
			if (unit >= 2) {
				result.fixed_disk_sectors = ts;
			}
			result.carry = false;
		} else if (unit < DOS_DRIVES && (Drives[unit] || unit < 2)) {
			result.status = unit < 2 ? 1 : 3;
			if (unit >= 2) {
				result.fixed_disk_sectors = 0x4800;
			}
		} else {
			LOG(LOG_BIOS, LOG_WARN)
			("INT13: no images, but invalid drive for call 15");
			return {.status = 0xff, .carry = true};
		}
		break;
	case 0x16: // Diskette change line status
		if (request.drive >= 2) {
			last_status = 0x01;
		} else {
			const auto& disk = disk_slots[request.drive].sector;
			last_status      = !disk || !disk->active
			                         ? 0x80
			                         : (FLOPPY_IsDiskChanged(request.drive)
			                                    ? 0x06
			                                    : 0);
			mem_writeb(BIOS_DISK_STATUS, last_status);
		}
		result.status = last_status;
		result.carry  = last_status != 0;
		break;
	case 0x17: /* Set disk type for format */
		/* Pirates! needs this to load */
		killRead = true;
		result.status = 0x00;
		result.carry  = false;
		break;
	default:
		LOG(LOG_BIOS, LOG_ERROR)
		("INT13: Function %x called on drive %x (dos drive %d)",
		 request.function,
		 request.drive,
		 unit);
		result.status = 0xff;
		result.carry  = true;
	}
	return result;
}

static Bitu INT13_DiskHandler()
{
	const DiskServiceRequest request = {reg_ah,
	                                    reg_dl,
	                                    reg_al,
	                                    reg_dh,
	                                    check_cast<uint8_t>(reg_cl & 0x3f),
	                                    check_cast<uint16_t>(
	                                            reg_ch | ((reg_cl & 0xc0) << 2)),
	                                    RealMake(SegValue(es), reg_bx)};
	CALLBACK_SIF(true);
	const auto result = run_disk_service(request);
	if (result.status) {
		reg_ah = *result.status;
	}
	if (result.completed) {
		reg_al = *result.completed;
	}
	if (const auto& params = result.parameters) {
		reg_ax = 0;
		reg_bl = params->bios_type;
		reg_ch = check_cast<uint8_t>(params->max_cylinder & 0xff);
		reg_cl = check_cast<uint8_t>(((params->max_cylinder >> 2) & 0xc0) |
		                             (params->sectors_per_track & 0x3f));
		reg_dh = static_cast<uint8_t>(params->max_head);
		reg_dl = params->drive_count;
	}
	if (result.fixed_disk_sectors) {
		reg_cx = check_cast<uint16_t>(*result.fixed_disk_sectors >> 16);
		reg_dx = check_cast<uint16_t>(*result.fixed_disk_sectors & 0xffff);
	}
	CALLBACK_SCF(result.carry);
	return CBRET_NONE;
}

static Bitu floppy_irq6()
{
	mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) | 0x80);
	return CBRET_NONE;
}

void BIOS_SetupDisks(void)
{
	// INT 1Eh points to the guest-writable floppy parameter table.
	const auto floppy_params = RealMake(0x40, 0xe0);
	RealSetVec(0x1e, floppy_params);
	constexpr std::array<uint8_t, 11> defaults = {
	        0xdf, 0x02, 0x25, 0x02, 0x12, 0x1b, 0xff, 0x6c, 0xf6, 0x0f, 0x08};
	for (size_t i = 0; i < defaults.size(); ++i) {
		phys_writeb(RealToPhysical(floppy_params) + static_cast<PhysPt>(i),
		            defaults[i]);
	}

	FLOPPY_Init();
	if (!is_machine_pcjr()) {
		const auto irq6 = CALLBACK_Allocate();
		CALLBACK_Setup(irq6, floppy_irq6, CB_IRET_EOI_PIC1, "IRQ 6 Floppy");
		RealSetVec(0x0e, CALLBACK_RealPointer(irq6));
	}
	/* TODO Start the time correctly */
	call_int13 = CALLBACK_Allocate();
	CALLBACK_Setup(call_int13, &INT13_DiskHandler, CB_INT13, "Int 13 Bios disk");
	RealSetVec(0x13, CALLBACK_RealPointer(call_int13));

	// Clean any the numbered images
	disk_slots.fill({});

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
