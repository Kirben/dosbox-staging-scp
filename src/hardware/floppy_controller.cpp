// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/floppy_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "hardware/dma.h"
#include "hardware/pic.h"
#include "hardware/port.h"
#include "ints/bios_disk.h"
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

enum class Phase { Command, Execution, Result };
enum class Command {
	Specify        = 0x03,
	WriteData      = 0x05,
	ReadData       = 0x06,
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
	std::array<Drive, 2> drives      = {};
	std::array<uint8_t, 9> command   = {};
	std::array<uint8_t, 7> result    = {};
	std::shared_ptr<imageDisk> disk  = {};
	const FloppySector* track_sector = nullptr;
	Phase phase                      = Phase::Command;
	size_t command_count             = 0;
	size_t result_count              = 0;
	size_t result_length             = 0;
	uint8_t dor                      = 0;
	uint8_t reset_unit               = 4;
	uint8_t seek_status              = 0;
	uint8_t seek_cylinder            = 0;
	uint8_t cylinder                 = 0;
	uint8_t head                     = 0;
	uint8_t sector                   = 0;
	bool irq_pending                 = false;
	bool seek_pending                = false;
	bool non_dma                     = false;
};

Controller fdc = {};

void update_irq()
{
	if (fdc.irq_pending && (fdc.dor & InterruptEnabled)) {
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

void finish_data(const uint8_t status_1, const uint8_t status_2 = 0)
{
	const auto status_0 = check_cast<uint8_t>(
	        ((status_1 || status_2) ? 0x40 : 0) | (fdc.head << 2) |
	        selected_unit());
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
	begin_result(fdc.result.size(), true);
}

double rotation_cycle_ms()
{
	if (const auto* track = fdc.disk->GetTrackImage()) {
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

void schedule_track_sector(const FloppyTrackImage& image, const bool read_id)
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
		const auto wait_ms = offset_ms > position_ms
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
	if (const auto* track = fdc.disk->GetTrackImage()) {
		schedule_track_sector(*track, false);
	} else {
		PIC_AddEvent(transfer_sector, slot_length_ms());
	}
}

void transfer_sector(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}

	const auto writing = (fdc.command[0] & 0x1f) ==
	                     static_cast<int>(Command::WriteData);
	auto* dma = DMA_GetChannel(FloppyDma);
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
	} else if (writing && !fdc.disk->WriteSectorForController(
	                              fdc.head, fdc.cylinder, fdc.sector, buffer)) {
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

void format_track(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}

	constexpr auto IdSize                 = 4;
	std::array<uint8_t, IdSize * 255> ids = {};
	const auto count                      = fdc.command[3] * size_t{IdSize};
	auto* dma                             = DMA_GetChannel(FloppyDma);
	auto* track                           = fdc.disk->GetTrackImage();
	if (!dma || dma->is_masked || !dma->is_memory_to_device ||
	    fdc.non_dma || !(fdc.dor & InterruptEnabled)) {
		finish_data(0x10);
		return;
	}

	const auto available = std::min(count,
	                                static_cast<size_t>(dma->curr_count) + 1);
	if (!track && available < count) {
		finish_data(0x10);
		return;
	}
	dma->RaiseRequest();
	const auto transferred = dma->Read(available, ids.data());
	dma->ClearRequest();
	const auto completed = transferred - transferred % IdSize;
	if (completed == 0 || (!track && completed != count)) {
		finish_data(0x10);
		return;
	}

	if (track) {
		using Result = FloppyTrackImage::FormatResult;
		const auto result = track->FormatScpTrack(fdc.cylinder,
		                                          fdc.head,
		                                          std::span(ids).first(completed),
		                                          fdc.command[5],
		                                          fdc.command[3]);
		switch (result) {
		case Result::InvalidLayout: finish_data(0x04); return;
		case Result::WriteError: finish_data(0x20); return;
		case Result::Success: {
			const auto last           = completed - IdSize;
			const FloppySector sector = {ids[last],
			                             ids[last + 1],
			                             ids[last + 2],
			                             ids[last + 3]};
			fdc.track_sector          = &sector;
			finish_data(completed == count ? 0 : 0x10);
			return;
		}
		}
	}

	// Raw images only represent a full track with sequential, fixed-size IDs.
	for (size_t i = 0; i < count; i += IdSize) {
		if (ids[i] != fdc.cylinder || ids[i + 1] != fdc.head ||
		    ids[i + 2] != i / IdSize + 1 || ids[i + 3] != SectorSizeCode) {
			finish_data(0x04);
			return;
		}
	}

	std::array<uint8_t, SectorSize> fill = {};
	fill.fill(fdc.command[5]);
	for (unsigned sector = 1; sector <= fdc.command[3]; ++sector) {
		fdc.sector = check_cast<uint8_t>(sector);
		if (!fdc.disk->WriteSectorForController(
		            fdc.head, fdc.cylinder, fdc.sector, fill)) {
			finish_data(0x20);
			return;
		}
	}
	finish_data(0);
}

void start_data(const Command command)
{
	const auto unit = selected_unit();
	fdc.disk        = BIOS_GetDisk(unit);
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
	fdc.phase = Phase::Execution;
	if (command == Command::FormatTrack) {
		const auto raw = !fdc.disk->GetTrackImage();
		if (fdc.command[3] == 0 || fdc.command[2] > 7 ||
		    (raw && (fdc.command[2] != SectorSizeCode ||
		             fdc.command[3] != fdc.disk->sectors ||
		             fdc.disk->getSectSize() != SectorSize))) {
			finish_data(0x04);
			return;
		}
		PIC_AddEvent(format_track, 2 * rotation_cycle_ms() - rotation_ms());
		return;
	}
	if (const auto* track = fdc.disk->GetTrackImage()) {
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
	const auto delay_ms = target_ms > position_ms
	                            ? target_ms - position_ms
	                            : rotation_cycle_ms() + target_ms - position_ms;
	PIC_AddEvent(transfer_sector, delay_ms);
}

void complete_seek(uint32_t)
{
	const auto unit = selected_unit();
	if (motor_on(unit) && unit == (fdc.dor & 3u)) {
		fdc.drives[unit].cylinder = fdc.seek_cylinder;
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
	case Command::ReadData: start_data(command); break;
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
	case Command::ReadData: return 9;
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

void reset_controller()
{
	PIC_RemoveEvents(complete_seek);
	PIC_RemoveEvents(complete_read_id);
	PIC_RemoveEvents(transfer_sector);
	PIC_RemoveEvents(format_track);
	PIC_RemoveEvents(missing_sector);
	fdc = {};
}

void write_dor(io_port_t, const io_val_t value, io_width_t)
{
	const auto previous = fdc.dor;
	if (!(value & ResetReleased)) {
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
	update_irq();
}

} // namespace

void FLOPPY_Init()
{
	if (is_machine_pcjr()) {
		return;
	}
	reset_controller();
	update_irq();
	IO_RegisterWriteHandler(0x3f2, write_dor, io_width_t::byte);
	IO_RegisterReadHandler(0x3f4, read_status, io_width_t::byte);
	IO_RegisterReadHandler(0x3f5, read_data, io_width_t::byte);
	IO_RegisterWriteHandler(0x3f5, write_data, io_width_t::byte);
}
