// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/floppy_controller.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <memory>

#include "cpu/callback.h"
#include "hardware/dma.h"
#include "hardware/ide.h"
#include "hardware/pic.h"
#include "hardware/port.h"
#include "ints/bios_disk.h"
#include "ints/floppy_media.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {

constexpr auto FloppyIrq                  = 6;
constexpr auto FloppyDma                  = 2;
constexpr auto ResetReleased              = 0x04;
constexpr auto InterruptEnabled           = 0x08;
constexpr auto SectorSize                 = 512;
constexpr auto SectorSizeCode             = 2;
constexpr auto StandardRevolutionMs       = 200.0;
constexpr auto HighDensity525RevolutionMs = 1000.0 / 6;
constexpr auto StepMs                     = 3.0;

constexpr size_t FormatIdSize = 4;

enum class Phase { Command, Execution, Result };
enum class Command {
	ReadTrack      = 0x02,
	Specify        = 0x03,
	WriteData      = 0x05,
	ReadData       = 0x06,
	Verify         = 0x16,
	Recalibrate    = 0x07,
	SenseInterrupt = 0x08,
	ReadId         = 0x0a,
	FormatTrack    = 0x0d,
	Seek           = 0x0f,
};

struct Drive {
	uint8_t cylinder        = 0;
	double motor_started_ms = 0;
};

struct Controller {
	std::array<Drive, 2> drives                        = {};
	std::array<uint8_t, 9> command                     = {};
	std::array<uint8_t, 7> result                      = {};
	std::array<uint8_t, FormatIdSize * 255> format_ids = {};
	size_t track_offset                                = 0;
	uint8_t track_status_1                             = 0;
	uint8_t track_status_2                             = 0;
	size_t formatted_ids                               = 0;
	std::shared_ptr<imageDisk> disk                    = {};
	std::shared_ptr<FloppyMedia> media                 = {};
	const FloppySector* track_sector                   = nullptr;
	Phase phase                                        = Phase::Command;
	size_t command_count                               = 0;
	size_t result_count                                = 0;
	size_t result_length                               = 0;
	uint8_t dor                                        = 0;
	uint8_t reset_unit                                 = 4;
	uint8_t seek_status                                = 0;
	uint8_t seek_cylinder                              = 0;
	uint8_t cylinder                                   = 0;
	uint8_t head                                       = 0;
	uint8_t sector                                     = 0;
	bool irq_pending                                   = false;
	bool seek_pending                                  = false;
	bool non_dma                                       = false;
};

Controller fdc    = {};
bool irq_asserted = false;
// The latch belongs to the drive and survives controller resets.
std::array<bool, 2> disk_changed = {};

void update_irq()
{
	if (is_machine_pcjr()) {
		return;
	}
	const auto asserted = fdc.irq_pending && (fdc.dor & InterruptEnabled);
	if (asserted == irq_asserted) {
		return;
	}
	irq_asserted = asserted;
	if (asserted) {
		PIC_ActivateIRQ(FloppyIrq);
	} else {
		PIC_DeActivateIRQ(FloppyIrq);
	}
}

size_t selected_unit()
{
	return fdc.command[1] & 3;
}

bool motor_on(const size_t unit)
{
	return unit < fdc.drives.size() && (fdc.dor & (0x10u << unit));
}

bool media_ready(const size_t unit)
{
	return motor_on(unit) && unit == (fdc.dor & 3u) && fdc.disk &&
	       fdc.disk == BIOS_GetDisk(unit) && fdc.disk->active &&
	       !fdc.disk->hardDrive;
}

void begin_result(const size_t length, const bool interrupt)
{
	fdc.phase         = Phase::Result;
	fdc.result_count  = 0;
	fdc.result_length = length;
	fdc.irq_pending   = interrupt;
	update_irq();
}

void finish_data(const uint8_t status_1, const uint8_t status_2 = 0,
                 const bool terminal_count = false)
{
	const auto status_0 = check_cast<uint8_t>(
	        (((status_1 || status_2) && !terminal_count) ? 0x40 : 0) |
	        (fdc.head << 2) | selected_unit());
	const auto* sector = fdc.track_sector;
	fdc.result         = {status_0,
	                      status_1,
	                      status_2,
                      sector ? sector->cylinder : fdc.cylinder,
                      sector ? sector->head : fdc.head,
                      sector ? sector->id : fdc.sector,
                      sector ? sector->size_code
	                             : static_cast<uint8_t>(SectorSizeCode)};
	fdc.track_sector   = nullptr;
	fdc.disk.reset();
	fdc.media.reset();
	if (is_machine_pcjr() &&
	    (fdc.command[0] & 0x1f) == static_cast<int>(Command::ReadTrack)) {
		// PCjr reserves IRQ6 for its keyboard. Signal the disk
		// completion flag used by the guest without raising a PIC
		// interrupt.
		mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) | 0x80);
	}
	begin_result(fdc.result.size(), true);
}

double rotation_cycle_ms()
{
	if (const auto& track = fdc.media) {
		return track->GetCycleMs(fdc.cylinder, fdc.head);
	}
	// The 1.2 MB 5.25-inch drive spins at 360 RPM.
	return fdc.disk->GetBiosType() == 2 ? HighDensity525RevolutionMs
	                                    : StandardRevolutionMs;
}

double slot_length_ms()
{
	return rotation_cycle_ms() / fdc.disk->sectors;
}

double rotation_ms()
{
	const auto elapsed_ms = PIC_FullIndex() -
	                        fdc.drives[selected_unit()].motor_started_ms;
	return std::fmod(std::max(0.0, elapsed_ms), rotation_cycle_ms());
}

void complete_read_id(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
	} else {
		finish_data(fdc.track_sector && fdc.track_sector->id_crc_error ? 0x20
		                                                               : 0);
	}
}

void transfer_sector(uint32_t);

void missing_sector(uint32_t)
{
	finish_data(0x04);
}

void schedule_track_sector(const FloppyMedia& image, const bool read_id)
{
	const auto track       = image.GetTrack(fdc.cylinder, fdc.head);
	const auto position_ms = rotation_ms();
	const auto cycle_ms    = image.GetCycleMs(fdc.cylinder, fdc.head);
	auto delay_ms          = cycle_ms;
	fdc.track_sector       = nullptr;
	for (const auto& candidate : track) {
		if (!read_id &&
		    (candidate.cylinder != fdc.command[2] ||
		     candidate.head != fdc.head || candidate.id != fdc.sector ||
		     candidate.size_code != fdc.command[5])) {
			continue;
		}
		const auto offset_ms = candidate.cycle_fraction * cycle_ms;
		const auto wait_ms   = offset_ms > position_ms
		                             ? offset_ms - position_ms
		                             : cycle_ms + offset_ms - position_ms;
		if (!fdc.track_sector || wait_ms < delay_ms) {
			fdc.track_sector = &candidate;
			delay_ms         = wait_ms;
		}
	}
	const auto event = fdc.track_sector
	                         ? (read_id ? complete_read_id : transfer_sector)
	                         : missing_sector;
	PIC_AddEvent(event, delay_ms);
}

void schedule_next_sector()
{
	if (const auto& track = fdc.media) {
		schedule_track_sector(*track, false);
	} else {
		PIC_AddEvent(transfer_sector, slot_length_ms());
	}
}

void verify_sector()
{
	const auto* sector = fdc.track_sector;
	assert(sector);
	if (sector->id_crc_error) {
		finish_data(0x20);
	} else if (sector->data.empty()) {
		finish_data(0x01, 0x01);
	} else if (sector->data_crc_error) {
		finish_data(0x20, 0x20);
	} else if (sector->deleted) {
		finish_data(0, 0x40);
	} else if (fdc.sector == fdc.command[6]) {
		finish_data(0);
	} else {
		++fdc.sector;
		schedule_next_sector();
	}
}

void transfer_track_sector(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	const auto track = fdc.media->GetTrack(fdc.cylinder, fdc.head);
	if (fdc.track_offset >= track.size() ||
	    track[fdc.track_offset].revolution != 0) {
		finish_data(fdc.track_status_1 | 0x04, fdc.track_status_2);
		return;
	}
	const auto& sector = track[fdc.track_offset];
	fdc.track_sector   = &sector;
	auto* dma          = DMA_GetChannel(FloppyDma);
	if (!dma || dma->is_masked || !dma->is_device_to_memory ||
	    fdc.non_dma || !(fdc.dor & InterruptEnabled)) {
		finish_data(0x10);
		return;
	}
	const auto size = size_t{128} << fdc.command[5];
	const auto& data = size > sector.data.size() && !sector.extended_data.empty()
	                         ? sector.extended_data
	                         : sector.data;
	if (data.size() < size) {
		finish_data(fdc.track_status_1 | 0x01, fdc.track_status_2 | 0x01);
		return;
	}
	if (sector.id_crc_error || (&data == &sector.data && sector.data_crc_error)) {
		fdc.track_status_1 |= 0x20;
	}
	if (&data == &sector.data && sector.data_crc_error) {
		fdc.track_status_2 |= 0x20;
	}
	if (sector.cylinder != fdc.command[2] || sector.head != fdc.head ||
	    sector.size_code != fdc.command[5]) {
		fdc.track_status_1 |= 0x04;
	}
	if (sector.deleted) {
		fdc.track_status_2 |= 0x40;
	}
	const auto remaining = static_cast<size_t>(dma->curr_count) + 1;
	const auto count     = std::min(remaining, size);
	dma->RaiseRequest();
	const auto transferred = dma->Write(count,
	                                    const_cast<uint8_t*>(data.data()));
	dma->ClearRequest();
	if (transferred != count) {
		finish_data(fdc.track_status_1 | 0x10, fdc.track_status_2);
	} else if (count == remaining) {
		finish_data(fdc.track_status_1, fdc.track_status_2, true);
	} else if (++fdc.track_offset == fdc.command[6]) {
		finish_data(fdc.track_status_1 | 0x80, fdc.track_status_2);
	} else {
		const auto cycle     = rotation_cycle_ms();
		const auto offset_ms = sector.cycle_fraction * cycle;
		const auto next_ms =
		        fdc.track_offset < track.size() &&
		                        track[fdc.track_offset].revolution == 0
		                ? track[fdc.track_offset].cycle_fraction * cycle
		                : fdc.media->GetFormatCycleMs(fdc.cylinder, fdc.head);
		PIC_AddEvent(transfer_track_sector,
		             std::max(0.0, next_ms - offset_ms));
	}
}

void transfer_sector(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}

	const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
	if (command == Command::Verify) {
		verify_sector();
		return;
	}

	const auto writing = command == Command::WriteData;
	auto* dma          = DMA_GetChannel(FloppyDma);
	if (!dma || dma->is_masked ||
	    !(writing ? dma->is_memory_to_device : dma->is_device_to_memory) ||
	    fdc.non_dma || !(fdc.dor & InterruptEnabled)) {
		finish_data(0x10);
		return;
	}

	std::array<uint8_t, SectorSize> buffer = {};
	std::span<const uint8_t> data          = buffer;
	if (const auto* sector = fdc.track_sector) {
		if (sector->id_crc_error) {
			finish_data(0x20);
			return;
		}
		if (!writing && sector->data.empty()) {
			finish_data(0x01, 0x01);
			return;
		}
		if (!writing) {
			data = sector->data;
		}
	} else if (!writing && !fdc.disk->ReadSectorForController(fdc.head,
	                                                          fdc.cylinder,
	                                                          fdc.sector,
	                                                          buffer)) {
		finish_data(0x04);
		return;
	}

	const auto remaining = static_cast<size_t>(dma->curr_count) + 1;
	const auto count     = std::min(remaining, data.size());
	dma->RaiseRequest();
	// DMA Write copies from the source buffer without modifying it.
	const auto transferred =
	        writing ? dma->Read(count, buffer.data())
	                : dma->Write(count, const_cast<uint8_t*>(data.data()));
	dma->ClearRequest();
	if (transferred != count) {
		finish_data(0x10);
	} else if (writing &&
	           !(fdc.track_sector
	                     ? fdc.media->WriteSector(fdc.cylinder,
	                                              fdc.head,
	                                              fdc.sector,
	                                              buffer,
	                                              fdc.track_sector)
	                     : fdc.disk->WriteSectorForController(
	                               fdc.head, fdc.cylinder, fdc.sector, buffer))) {
		finish_data(0x20);
	} else if (fdc.track_sector && fdc.track_sector->data_crc_error) {
		finish_data(0x20, 0x20);
	} else if (fdc.track_sector && fdc.track_sector->deleted) {
		finish_data(0, 0x40);
	} else if (remaining == count) {
		finish_data(0);
	} else if (fdc.sector < fdc.command[6]) {
		++fdc.sector;
		schedule_next_sector();
	} else if ((fdc.command[0] & 0x80) && fdc.head == 0 && fdc.disk->heads > 1) {
		fdc.head   = 1;
		fdc.sector = 1;
		schedule_next_sector();
	} else {
		finish_data(0x80);
	}
}

bool format_dma_ready(const DmaChannel* dma)
{
	return dma && !dma->is_masked && dma->is_memory_to_device &&
	       !fdc.non_dma && (fdc.dor & InterruptEnabled);
}

uint8_t format_status(const FloppyFormatResult result)
{
	switch (result) {
	case FloppyFormatResult::Success: return 0;
	case FloppyFormatResult::WriteProtected: return 0x02;
	case FloppyFormatResult::InvalidLayout: return 0x04;
	case FloppyFormatResult::WriteError: return 0x20;
	}
	return 0x20;
}

uint8_t commit_track_format()
{
	const auto ids = std::span(fdc.format_ids).first(fdc.formatted_ids * FormatIdSize);
	return format_status(FLOPPY_FormatTrack(*fdc.disk,
	                                        fdc.media.get(),
	                                        fdc.cylinder,
	                                        fdc.head,
	                                        ids,
	                                        fdc.command[5],
	                                        fdc.command[3]));
}

void finish_track_format(const uint8_t status)
{
	if (!fdc.formatted_ids) {
		finish_data(status);
		return;
	}
	const auto result         = commit_track_format();
	const auto offset         = (fdc.formatted_ids - 1) * FormatIdSize;
	const auto& ids           = fdc.format_ids;
	const FloppySector sector = {ids[offset],
	                             ids[offset + 1],
	                             ids[offset + 2],
	                             ids[offset + 3]};
	fdc.track_sector          = &sector;
	finish_data(result ? result : status);
}

double track_format_slot_ms()
{
	return fdc.media->GetFormatCycleMs(fdc.cylinder, fdc.head) / fdc.command[3];
}

void format_track_sector(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	auto* dma = DMA_GetChannel(FloppyDma);
	if (!format_dma_ready(dma)) {
		finish_track_format(0x10);
		return;
	}
	const auto remaining = static_cast<size_t>(dma->curr_count) + 1;
	const auto count     = std::min(FormatIdSize, remaining);
	auto* id = fdc.format_ids.data() + fdc.formatted_ids * FormatIdSize;
	dma->RaiseRequest();
	const auto transferred = dma->Read(count, id);
	dma->ClearRequest();
	if (transferred != FormatIdSize) {
		finish_track_format(0x10);
	} else if (++fdc.formatted_ids == fdc.command[3]) {
		finish_track_format(0);
	} else if (remaining == FormatIdSize) {
		finish_track_format(0x10);
	} else {
		PIC_AddEvent(format_track_sector, track_format_slot_ms());
	}
}

void format_raw_track(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	std::array<uint8_t, FormatIdSize * 255> ids = {};
	const auto count = fdc.command[3] * FormatIdSize;
	auto* dma        = DMA_GetChannel(FloppyDma);
	if (!format_dma_ready(dma) ||
	    static_cast<size_t>(dma->curr_count) + 1 < count) {
		finish_data(0x10);
		return;
	}
	dma->RaiseRequest();
	const auto transferred = dma->Read(count, ids.data());
	dma->ClearRequest();
	if (transferred != count) {
		finish_data(0x10);
		return;
	}

	const auto result = FLOPPY_FormatTrack(*fdc.disk,
	                                       nullptr,
	                                       fdc.cylinder,
	                                       fdc.head,
	                                       std::span(ids).first(count),
	                                       fdc.command[5],
	                                       fdc.command[3]);
	if (result == FloppyFormatResult::Success) {
		fdc.sector = fdc.command[3];
	}
	finish_data(format_status(result));
}

void start_data(const Command command)
{
	const auto unit = selected_unit();
	fdc.disk        = BIOS_GetDisk(unit);
	fdc.media       = BIOS_GetFloppyMedia(unit);
	fdc.head        = (fdc.command[1] >> 2) & 1;
	fdc.cylinder = unit < fdc.drives.size() ? fdc.drives[unit].cylinder : 0;
	fdc.sector = (command == Command::ReadId || command == Command::FormatTrack)
	                   ? 1
	                   : fdc.command[4];

	if (!media_ready(unit) || fdc.head >= fdc.disk->heads ||
	    fdc.cylinder >= fdc.disk->cylinders) {
		finish_data(0x04);
		return;
	}

	if ((command == Command::WriteData || command == Command::FormatTrack) &&
	    fdc.disk->IsWriteProtected()) {
		finish_data(0x02);
		return;
	}
	// Support the single-track, end-of-track form used by the BIOS.
	if (command == Command::Verify &&
	    (!fdc.media || (fdc.command[0] & 0xa0) || (fdc.command[1] & 0x80))) {
		finish_data(0x04);
		return;
	}
	fdc.phase = Phase::Execution;
	if (command == Command::FormatTrack) {
		const auto raw = !fdc.media;
		if (fdc.command[3] == 0 || fdc.command[2] > 7 ||
		    (raw && (fdc.command[2] != SectorSizeCode ||
		             fdc.command[3] != fdc.disk->sectors ||
		             fdc.disk->getSectSize() != SectorSize))) {
			finish_data(0x04);
			return;
		}
		fdc.formatted_ids   = 0;
		const auto to_index = rotation_cycle_ms() - rotation_ms();
		if (raw) {
			PIC_AddEvent(format_raw_track,
			             to_index + rotation_cycle_ms());
		} else {
			PIC_AddEvent(format_track_sector,
			             to_index + track_format_slot_ms());
		}
		return;
	}
	if (command == Command::ReadTrack) {
		const auto& image = fdc.media;
		if (!image || fdc.command[5] > 7 || !fdc.command[6]) {
			finish_data(0x04);
			return;
		}
		fdc.track_offset   = 0;
		fdc.track_status_1 = 0;
		fdc.track_status_2 = 0;
		const auto track   = image->GetTrack(fdc.cylinder, fdc.head);
		const auto cycle   = rotation_cycle_ms();
		const auto first = track.empty() ? 1.0 : track.front().cycle_fraction;
		PIC_AddEvent(transfer_track_sector,
		             cycle - rotation_ms() + first * cycle);
		return;
	}
	if (const auto& track = fdc.media) {
		if (command != Command::ReadId &&
		    (fdc.command[3] != fdc.head || fdc.sector > fdc.command[6] ||
		     (command == Command::WriteData &&
		      fdc.command[5] != SectorSizeCode))) {
			finish_data(0x04);
			return;
		}
		schedule_track_sector(*track, command == Command::ReadId);
		return;
	}
	if (fdc.disk->sectors == 0 || fdc.disk->getSectSize() != SectorSize) {
		finish_data(0x04);
		return;
	}

	const auto length_ms   = slot_length_ms();
	const auto position_ms = rotation_ms();
	if (command == Command::ReadId) {
		const auto next_slot = std::floor(position_ms / length_ms) + 1.0;
		fdc.sector = check_cast<uint8_t>(
		        static_cast<unsigned>(next_slot) % fdc.disk->sectors + 1u);
		PIC_AddEvent(complete_read_id, next_slot * length_ms - position_ms);
		return;
	}

	const auto end_sector = fdc.command[6];
	if (fdc.command[2] != fdc.cylinder || fdc.command[3] != fdc.head ||
	    fdc.command[5] != SectorSizeCode || fdc.sector == 0 ||
	    fdc.sector > end_sector || end_sector > fdc.disk->sectors) {
		finish_data(0x04);
		return;
	}

	const auto target_ms = (fdc.sector - 1) * length_ms;
	const auto delay_ms  = target_ms > position_ms
	                             ? target_ms - position_ms
	                             : rotation_cycle_ms() + target_ms - position_ms;
	PIC_AddEvent(transfer_sector, delay_ms);
}

void complete_seek(uint32_t)
{
	const auto unit = selected_unit();
	if (motor_on(unit) && unit == (fdc.dor & 3u)) {
		auto& drive = fdc.drives[unit];
		if (drive.cylinder != fdc.seek_cylinder && BIOS_GetDisk(unit)) {
			FLOPPY_SetDiskChanged(unit, false);
		}
		drive.cylinder = fdc.seek_cylinder;
		fdc.seek_status = check_cast<uint8_t>(0x20u | (fdc.command[1] & 7u));
	} else {
		fdc.seek_status = check_cast<uint8_t>(0x68u | (fdc.command[1] & 7u));
	}
	fdc.seek_cylinder = unit < fdc.drives.size() ? fdc.drives[unit].cylinder : 0;
	fdc.seek_pending = true;
	fdc.phase        = Phase::Command;
	fdc.irq_pending  = true;
	update_irq();
}

void execute_command(const Command command)
{
	switch (command) {
	case Command::Specify: fdc.non_dma = (fdc.command[2] & 1) != 0; break;
	case Command::SenseInterrupt:
		if (fdc.reset_unit < 4) {
			const auto unit = fdc.reset_unit++;
			fdc.result = {check_cast<uint8_t>(0xc0u | unit), 0};
			begin_result(2, false);
		} else if (fdc.seek_pending) {
			fdc.seek_pending = false;
			fdc.result       = {fdc.seek_status, fdc.seek_cylinder};
			begin_result(2, false);
		} else {
			fdc.result[0] = 0x80;
			begin_result(1, false);
		}
		break;
	case Command::Seek:
	case Command::Recalibrate: {
		const auto unit     = selected_unit();
		const auto cylinder = unit < fdc.drives.size()
		                            ? fdc.drives[unit].cylinder
		                            : 0;
		fdc.seek_cylinder = command == Command::Seek ? fdc.command[2] : 0;
		const auto steps = std::max(1, std::abs(cylinder - fdc.seek_cylinder));
		fdc.phase = Phase::Execution;
		PIC_AddEvent(complete_seek, steps * StepMs);
		break;
	}
	case Command::ReadId:
	case Command::FormatTrack:
	case Command::WriteData:
	case Command::ReadData:
	case Command::ReadTrack:
	case Command::Verify: start_data(command); break;
	default:
		fdc.result[0] = 0x80;
		begin_result(1, false);
		break;
	}
}

size_t command_length(const Command command)
{
	switch (command) {
	case Command::FormatTrack: return 6;
	case Command::WriteData:
	case Command::ReadData:
	case Command::ReadTrack:
	case Command::Verify: return 9;
	case Command::Seek:
	case Command::Specify: return 3;
	case Command::Recalibrate:
	case Command::ReadId: return 2;
	default: return 1;
	}
}

void write_data(io_port_t, const io_val_t value, io_width_t)
{
	if (!(fdc.dor & ResetReleased) || fdc.phase != Phase::Command) {
		return;
	}
	fdc.command[fdc.command_count++] = check_cast<uint8_t>(value);
	const auto command = static_cast<Command>(fdc.command.front() & 0x1f);
	if (fdc.command_count == command_length(command)) {
		fdc.command_count = 0;
		execute_command(command);
	}
}

io_val_t read_data(io_port_t, io_width_t)
{
	if (fdc.phase != Phase::Result) {
		return 0xff;
	}
	if (fdc.result_count == 0) {
		fdc.irq_pending = false;
		update_irq();
	}
	const auto value = fdc.result[fdc.result_count++];
	if (fdc.result_count == fdc.result_length) {
		fdc.phase = Phase::Command;
	}
	return value;
}

io_val_t read_status(io_port_t, io_width_t)
{
	if (!(fdc.dor & ResetReleased)) {
		return 0;
	}
	switch (fdc.phase) {
	case Phase::Execution: return 0x10;
	case Phase::Result: return 0xd0;
	case Phase::Command: return fdc.command_count ? 0x90 : 0x80;
	}
	return 0;
}

io_val_t read_dir(io_port_t, io_width_t)
{
	const auto unit = fdc.dor & 3u;
	return IDE_ReadPrimaryDriveAddress() |
	       (FLOPPY_IsDiskChanged(unit) ? 0x80 : 0);
}

void reset_controller()
{
	PIC_RemoveEvents(complete_seek);
	PIC_RemoveEvents(complete_read_id);
	PIC_RemoveEvents(transfer_sector);
	PIC_RemoveEvents(transfer_track_sector);
	PIC_RemoveEvents(format_raw_track);
	PIC_RemoveEvents(format_track_sector);
	PIC_RemoveEvents(missing_sector);
	fdc = {};
}

void write_dor(io_port_t, const io_val_t value, io_width_t)
{
	const auto previous       = fdc.dor;
	const auto unit           = selected_unit();
	const auto formatting_track = fdc.phase == Phase::Execution &&
	                              fdc.disk && fdc.media &&
	                              (fdc.command[0] & 0x1f) ==
	                                      static_cast<int>(Command::FormatTrack) &&
	                              media_ready(unit);
	if (!(value & ResetReleased)) {
		if (formatting_track && fdc.formatted_ids && commit_track_format()) {
			LOG_WARNING("FDC: Could not save track format interrupted by reset");
		}
		reset_controller();
	}
	fdc.dor = check_cast<uint8_t>(value);
	for (size_t unit = 0; unit < fdc.drives.size(); ++unit) {
		if (motor_on(unit) &&
		    (!(value & ResetReleased) || !(previous & (0x10u << unit)))) {
			fdc.drives[unit].motor_started_ms = PIC_FullIndex();
		}
	}
	if ((value & ResetReleased) && !(previous & ResetReleased)) {
		fdc.reset_unit  = 0;
		fdc.irq_pending = true;
	}
	if ((value & ResetReleased) && formatting_track && !motor_on(unit)) {
		PIC_RemoveEvents(format_track_sector);
		finish_track_format(0x04);
		return;
	}
	update_irq();
}

} // namespace

FloppyFormatResult FLOPPY_FormatTrack(imageDisk& disk, FloppyMedia* media,
                                      const uint32_t cylinder, const uint32_t head,
                                      const std::span<const uint8_t> ids,
                                      const uint8_t fill, const uint8_t requested)
{
	using Result = FloppyFormatResult;
	if (disk.IsWriteProtected()) {
		return Result::WriteProtected;
	}
	if (disk.hardDrive || cylinder > UINT8_MAX || cylinder >= disk.cylinders ||
	    head >= disk.heads || head > UINT8_MAX || ids.empty() ||
	    ids.size() % FormatIdSize || ids.size() / FormatIdSize > requested) {
		return Result::InvalidLayout;
	}

	if (media) {
		using ScpResult = FloppyMedia::FormatResult;
		switch (media->FormatTrack(check_cast<uint8_t>(cylinder),
		                           check_cast<uint8_t>(head),
		                           ids,
		                           fill,
		                           requested)) {
		case ScpResult::Success: return Result::Success;
		case ScpResult::InvalidLayout: return Result::InvalidLayout;
		case ScpResult::WriteError: return Result::WriteError;
		}
		return Result::WriteError;
	}

	// Raw images only represent a full track with sequential, fixed-size IDs.
	if (requested != disk.sectors || ids.size() / FormatIdSize != requested ||
	    disk.getSectSize() != SectorSize) {
		return Result::InvalidLayout;
	}
	for (size_t i = 0; i < ids.size(); i += FormatIdSize) {
		if (ids[i] != cylinder || ids[i + 1] != head ||
		    ids[i + 2] != i / FormatIdSize + 1 ||
		    ids[i + 3] != SectorSizeCode) {
			return Result::InvalidLayout;
		}
	}

	std::array<uint8_t, SectorSize> data = {};
	data.fill(fill);
	for (uint32_t sector = 1; sector <= requested; ++sector) {
		if (!disk.WriteSectorForController(head, cylinder, sector, data)) {
			return Result::WriteError;
		}
	}
	return Result::Success;
}

namespace {

struct BiosCommandResult {
	std::array<uint8_t, 7> status = {};
	uint32_t transferred          = 0;
	uint8_t error                 = 0;
};

BiosCommandResult bios_command(const uint8_t unit, const uint32_t cylinder,
                               const uint8_t head,
                               const std::span<const uint8_t> command,
                               const uint32_t address, const uint32_t size,
                               const bool writing, const int revolutions)
{
	if (fdc.phase != Phase::Command || fdc.command_count || fdc.irq_pending) {
		return {.error = 0x80};
	}
	const auto disk = BIOS_GetDisk(unit);
	if (unit >= fdc.drives.size() || !disk || !disk->active ||
	    disk->hardDrive || cylinder > UINT8_MAX ||
	    cylinder >= disk->cylinders || head > 1 || head >= disk->heads) {
		return {.error = 0x04};
	}
	constexpr auto MaskChannel = 0x04 | FloppyDma;
	auto* dma                  = size ? DMA_GetChannel(FloppyDma) : nullptr;
	if (size) {
		const auto offset = static_cast<uint16_t>(address);
		if (!dma || size > 0x10000u - offset) {
			return {.error = 0x09};
		}
		const auto dma_count = check_cast<uint16_t>(size - 1);
		IO_WriteB(0x0a, MaskChannel);
		dma->has_reached_terminal_count = false;
		IO_WriteB(0x0c, 0);
		IO_WriteB(0x04, static_cast<uint8_t>(offset));
		IO_WriteB(0x04, check_cast<uint8_t>(offset >> 8));
		IO_WriteB(0x05, static_cast<uint8_t>(dma_count));
		IO_WriteB(0x05, check_cast<uint8_t>(dma_count >> 8));
		IO_WriteB(0x81, check_cast<uint8_t>(address >> 16));
		IO_WriteB(0x0b, (writing ? 0x48 : 0x44) | FloppyDma);
		IO_WriteB(0x0a, FloppyDma);
	}

	// The BIOS implicitly seeks, stepping away and back if already in
	// position.
	FLOPPY_SetDiskChanged(unit, false);
	FLOPPY_BiosPosition(unit, check_cast<uint8_t>(cylinder));
	fdc.non_dma = false;

	const auto track    = BIOS_GetFloppyMedia(unit);
	const auto cycle_ms = track ? track->GetCycleMs(cylinder, head)
	                            : StandardRevolutionMs;
	const auto deadline = PIC_FullIndex() + revolutions * cycle_ms;
	const auto mask     = IO_ReadB(0x21);
	PIC_SetIRQMask(FloppyIrq, false);
	mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) & 0x7f);
	for (const auto byte : command) {
		write_data(0x3f5, byte, io_width_t::byte);
	}

	bool shutdown = false;
	while (fdc.phase == Phase::Execution && PIC_FullIndex() < deadline &&
	       !shutdown) {
		shutdown = CALLBACK_Idle();
	}
	if (!shutdown && fdc.phase == Phase::Result && fdc.irq_pending) {
		shutdown = CALLBACK_Idle();
	}
	PIC_SetIRQMask(FloppyIrq, (mask & (1u << FloppyIrq)) != 0);
	if (dma) {
		IO_WriteB(0x0a, MaskChannel);
	}

	if (shutdown || fdc.phase != Phase::Result || fdc.result_count ||
	    fdc.result_length != fdc.result.size()) {
		reset_controller();
		update_irq();
		return {.error = 0x80};
	}

	BiosCommandResult result = {};
	for (auto& byte : result.status) {
		byte = check_cast<uint8_t>(read_data(0x3f5, io_width_t::byte));
	}
	if (dma) {
		const auto remaining = static_cast<uint32_t>(dma->curr_count) + 1;
		if (dma->has_reached_terminal_count ||
		    (remaining == 0x10000u && size < remaining)) {
			result.transferred = size;
		} else if (remaining <= size) {
			result.transferred = size - remaining;
		}
	}
	return result;
}
} // namespace

static FloppyBiosTransferResult bios_transfer_track(
        const FloppyBiosTransfer operation, const uint8_t unit,
        const uint32_t cylinder, const uint8_t head, const uint8_t sector,
        const uint8_t size_code, const uint8_t count, const uint8_t gap,
        const uint32_t address)
{
	const auto last_sector = sector + count - 1;
	if (!sector || !count || last_sector > UINT8_MAX || size_code > 7 ||
	    unit > 1 || cylinder > UINT8_MAX || head > 1) {
		return {0x04, 0};
	}

	const auto writing   = operation == FloppyBiosTransfer::Write;
	const auto verifying = operation == FloppyBiosTransfer::Verify;
	const auto opcode    = verifying ? uint8_t{0x56}
	                                 : (writing ? uint8_t{0x45} : uint8_t{0x46});
	const std::array<uint8_t, 9> command = {opcode,
	                                        check_cast<uint8_t>(unit |
	                                                            (head << 2)),
	                                        check_cast<uint8_t>(cylinder),
	                                        head,
	                                        sector,
	                                        size_code,
	                                        check_cast<uint8_t>(last_sector),
	                                        gap,
	                                        0xff};
	const auto sector_size               = 128u << size_code;
	const auto size   = verifying ? 0u : count * sector_size;
	const auto result = bios_command(
	        unit, cylinder, head, command, address, size, writing, 3 + count);
	if (result.error) {
		return {result.error, 0};
	}

	const auto st0          = result.status[0];
	const auto st1          = result.status[1];
	const auto st2          = result.status[2];
	const auto failed       = (st0 & 0xc0) || st1 || st2;
	const auto before_error = std::clamp(result.status[5] - sector,
	                                     0,
	                                     static_cast<int>(count));
	auto completed = check_cast<uint8_t>(result.transferred / sector_size);
	if (verifying) {
		completed = failed ? check_cast<uint8_t>(before_error) : count;
	} else if (writing && failed) {
		// DMA can finish before the overlay commits the current sector.
		completed = check_cast<uint8_t>(
		        std::min<int>(completed, before_error));
	} else if ((st2 & 0x60) && completed) {
		// Read Data reports CRC and deleted marks after transferring
		// the sector.
		--completed;
	}
	if (st1 & 0x02) {
		return {0x03, completed};
	}
	if (st1 & 0x10) {
		return {0x08, completed};
	}
	if (st1 & 0x20) {
		return {writing ? uint8_t{0x20} : uint8_t{0x10}, completed};
	}
	if (failed) {
		return {0x04, completed};
	}
	return {result.transferred == size ? uint8_t{0} : uint8_t{0x08}, completed};
}

FloppyBiosTransferResult FLOPPY_BiosTransfer(
        const FloppyBiosTransfer operation, const uint8_t unit,
        const uint32_t cylinder, const uint8_t head, const uint8_t sector,
        const uint8_t size_code, const uint8_t count, const uint8_t gap,
        const uint32_t address)
{
	const auto disk = BIOS_GetDisk(unit);
	if (!disk || !sector || !count || size_code > 7 || head >= disk->heads) {
		return {0x04, 0};
	}

	const auto sector_size = 128u << size_code;
	const auto dma         = operation != FloppyBiosTransfer::Verify;
	if (dma && count * sector_size > 0x10000u - (address & 0xffff)) {
		return {0x09, 0};
	}

	// IDs beyond the logical geometry can belong to a protection track.
	// Requests starting at such an ID stay on that track.
	if (sector > disk->sectors) {
		return bios_transfer_track(operation,
		                           unit,
		                           cylinder,
		                           head,
		                           sector,
		                           size_code,
		                           count,
		                           gap,
		                           address);
	}
	const auto end_of_track = std::min(disk->sectors, 255u);
	auto current_cylinder   = cylinder;
	auto current_head       = static_cast<uint32_t>(head);
	auto current_sector     = static_cast<uint32_t>(sector);
	auto completed          = 0u;
	while (completed < count) {
		if (BIOS_GetDisk(unit) != disk || current_cylinder >= disk->cylinders) {
			return {0x04, check_cast<uint8_t>(completed)};
		}

		const auto run    = std::min(count - completed,
                                          end_of_track - current_sector + 1);
		const auto result = bios_transfer_track(
		        operation,
		        unit,
		        current_cylinder,
		        check_cast<uint8_t>(current_head),
		        check_cast<uint8_t>(current_sector),
		        size_code,
		        check_cast<uint8_t>(run),
		        gap,
		        dma ? address + completed * sector_size : 0u);
		completed += result.sectors;
		if (result.status || result.sectors != run) {
			return {result.status ? result.status : uint8_t{0x04},
			        check_cast<uint8_t>(completed)};
		}

		current_sector = 1;
		if (++current_head == disk->heads) {
			current_head = 0;
			++current_cylinder;
		}
	}
	return {0, check_cast<uint8_t>(completed)};
}

uint8_t FLOPPY_BiosDmaFormat(const uint8_t unit, const uint32_t cylinder,
                             const uint8_t head, const uint8_t size_code,
                             const uint8_t count, const uint8_t gap,
                             const uint8_t fill, const uint32_t address)
{
	if (unit > 1 || head > 1 || !count || size_code > 7) {
		return 0x04;
	}
	const std::array<uint8_t, 6> command = {
	        0x4d, check_cast<uint8_t>(unit | (head << 2)), size_code, count, gap, fill};
	const auto size     = check_cast<uint32_t>(count * FormatIdSize);
	const auto transfer = bios_command(
	        unit, cylinder, head, command, address, size, true, 3);
	if (transfer.error) {
		return transfer.error;
	}
	const auto& result = transfer.status;
	const auto status  = result[1];
	if (status & 0x02) {
		return 0x03;
	}
	if (status & 0x10) {
		return 0x08;
	}
	if (status & 0x20) {
		return 0x20;
	}
	if ((result[0] & 0xc0) || status || result[2]) {
		return 0x04;
	}
	return transfer.transferred == size ? 0 : 0x08;
}

void FLOPPY_BiosPosition(const uint8_t unit, const uint8_t cylinder)
{
	assert(unit < fdc.drives.size());
	auto& drive    = fdc.drives[unit];
	drive.cylinder = cylinder;
	if (!motor_on(unit)) {
		drive.motor_started_ms = PIC_FullIndex();
	}
	fdc.dor = check_cast<uint8_t>((fdc.dor & 0xf0) | (0x10u << unit) |
	                              ResetReleased | InterruptEnabled | unit);
}

void FLOPPY_BiosReset(const uint8_t unit)
{
	assert(unit < fdc.drives.size());
	const auto dor = check_cast<uint8_t>((fdc.dor & 0xf0) | ResetReleased |
	                                     InterruptEnabled | unit);
	const auto was_masked = (IO_ReadB(0x21) & (1u << FloppyIrq)) != 0;
	PIC_SetIRQMask(FloppyIrq, false);
	mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) & 0x7f);
	write_dor(0x3f2, dor & ~ResetReleased, io_width_t::byte);
	write_dor(0x3f2, dor, io_width_t::byte);
	const auto shutdown = CALLBACK_Idle();
	PIC_SetIRQMask(FloppyIrq, was_masked);
	if (shutdown) {
		return;
	}
	// Sense Interrupt Status consumes the reset status for each drive.
	while (fdc.phase == Phase::Command && !fdc.command_count &&
	       fdc.reset_unit < 4) {
		write_data(0x3f5, 0x08, io_width_t::byte);
		read_data(0x3f5, io_width_t::byte);
		read_data(0x3f5, io_width_t::byte);
	}
}

void FLOPPY_SetDiskChanged(const size_t unit, const bool changed)
{
	assert(unit < disk_changed.size());
	disk_changed[unit] = changed;
}

bool FLOPPY_IsDiskChanged(const size_t unit)
{
	return unit < disk_changed.size() && disk_changed[unit];
}

void FLOPPY_Init()
{
	disk_changed.fill(false);
	reset_controller();
	update_irq();
	IO_RegisterWriteHandler(0x3f2, write_dor, io_width_t::byte);
	IO_RegisterReadHandler(0x3f4, read_status, io_width_t::byte);
	IO_RegisterReadHandler(0x3f5, read_data, io_width_t::byte);
	IO_RegisterWriteHandler(0x3f5, write_data, io_width_t::byte);
	if (!is_machine_pcjr()) {
		IO_RegisterReadHandler(0x3f7, read_dir, io_width_t::byte);
	}
}
