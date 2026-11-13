// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/floppy_controller.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "cpu/callback.h"
#include "hardware/dma.h"
#include "hardware/ide.h"
#include "hardware/pic.h"
#include "hardware/port.h"
#include "ints/bios.h"
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
constexpr auto DefaultDataRateKbps        = 250;
constexpr auto DefaultStepMs              = 3.0;

constexpr auto PcjrWatchdogEnable  = 0x20;
constexpr auto PcjrWatchdogTrigger = 0x40;
// IBM specifies a one-to-three-second hardware timeout.
constexpr auto PcjrWatchdogMs = 3000.0;

constexpr size_t FormatIdSize = 4;

enum class Phase { Command, Execution, Result };
enum class Command {
	ReadTrack        = 0x02,
	Specify          = 0x03,
	WriteData        = 0x05,
	ReadData         = 0x06,
	ReadDeletedData  = 0x0c,
	WriteDeletedData = 0x09,
	Verify           = 0x16,
	Recalibrate      = 0x07,
	SenseInterrupt   = 0x08,
	ReadId           = 0x0a,
	FormatTrack      = 0x0d,
	Seek             = 0x0f,
};

struct Drive {
	uint8_t cylinder            = 0;
	double motor_started_ms     = 0;
	double head_loaded_until_ms = 0;
};

struct Seek {
	uint8_t cylinder = 0;
	uint8_t head     = 0;
	uint8_t status   = 0;
	bool active      = false;
	bool pending     = false;
};

struct Controller {
	std::array<Drive, 2> drives                        = {};
	std::array<Seek, 4> seeks                          = {};
	std::array<uint8_t, 9> command                     = {};
	std::array<uint8_t, 7> result                      = {};
	std::array<uint8_t, FormatIdSize * 255> format_ids = {};
	size_t track_revolution                            = 0;
	size_t track_remaining                             = 0;
	bool track_physical_transfer                       = false;
	bool track_transfer_ready                          = false;
	double track_transfer_ms                           = 0;
	uint64_t track_sample_turn                         = 0;
	size_t track_offset                                = 0;
	uint8_t track_status_1                             = 0;
	uint8_t track_status_2                             = 0;
	size_t formatted_ids                               = 0;
	uint16_t format_rate_kbps                 = DefaultDataRateKbps;
	std::shared_ptr<imageDisk> disk           = {};
	std::shared_ptr<FloppyMedia> media        = {};
	std::vector<uint8_t> pio_data             = {};
	size_t pio_offset                         = 0;
	double pio_started_ms                     = 0;
	bool pio_ready                            = false;
	bool pio_writing                          = false;
	bool pio_waiting_crc                      = false;
	bool pio_underrun                         = false;
	bool sector_data_crc_error                = false;
	bool sector_transfer_ready                = false;
	double sector_end_ms                      = 0;
	uint64_t sector_sample_turn               = 0;
	const FloppySector* track_sector          = nullptr;
	const FloppySector* wrong_cylinder_sector = nullptr;
	Phase phase                               = Phase::Command;
	size_t command_count                      = 0;
	size_t result_count                       = 0;
	size_t result_length                      = 0;
	uint8_t dor                               = 0;
	uint8_t reset_unit                        = 4;
	uint8_t cylinder                          = 0;
	uint8_t head                              = 0;
	uint8_t sector                            = 0;
	bool irq_pending                          = false;
	bool seek_irq_pending                     = false;
	bool skipped_control_mark                 = false;
	bool missing_address_mark                 = false;
	bool non_dma                              = false;
	bool timing_specified                     = false;
	uint8_t step_rate                         = 0;
	uint8_t head_load                         = 0;
	uint8_t head_unload                       = 0;
};

double pio_byte_ms      = 8.0 / DefaultDataRateKbps;
uint16_t data_rate_kbps = DefaultDataRateKbps;

uint16_t bios_data_rate(imageDisk& disk)
{
	const auto type = disk.GetBiosType();
	return type == 6 ? 1000 : (type == 2 || type == 4 ? 500 : 250);
}

// The BIOS consumes PCjr FIFO bytes as they become ready, while the
// controller keeps its normal rotational, CRC and completion timing.
struct BiosPioTransfer {
	uint32_t base        = 0;
	uint32_t size        = 0;
	uint32_t transferred = 0;
	uint16_t offset      = 0;
	bool writing         = false;
};
BiosPioTransfer* bios_pio = nullptr;
void service_bios_pio();

Controller fdc              = {};
bool irq_asserted           = false;
uint8_t pcjr_dor            = 0;
bool pcjr_watchdog_asserted = false;
// The latch belongs to the drive and survives controller resets.
std::array<bool, 2> disk_changed = {};

void update_irq()
{
	if (is_machine_pcjr()) {
		return;
	}
	const auto asserted = (fdc.irq_pending || fdc.seek_irq_pending) &&
	                      (fdc.dor & InterruptEnabled);
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

void pio_byte_event(uint32_t);
void start_loaded_data(uint32_t);

void begin_result(const size_t length, const bool interrupt)
{
	fdc.phase         = Phase::Result;
	fdc.result_count  = 0;
	fdc.result_length = length;
	fdc.irq_pending   = interrupt;
	update_irq();
}

// SPECIFY timers use the 500 kbit/s table and scale with the data rate.
// Zero encodes the maximum HLT/HUT delay, rather than disabling the timer.
double timer_ms(const double milliseconds)
{
	return milliseconds * 500.0 / data_rate_kbps;
}

double head_load_delay_ms(const size_t unit)
{
	if (!fdc.timing_specified ||
	    PIC_FullIndex() < fdc.drives[unit].head_loaded_until_ms) {
		return 0;
	}
	return timer_ms(2.0 * (fdc.head_load ? fdc.head_load : 128));
}

void finish_data(const uint8_t status_1, const uint8_t status_2 = 0,
                 const bool terminal_count = false)
{
	PIC_RemoveEvents(pio_byte_event);
	PIC_RemoveEvents(start_loaded_data);
	const auto unit = selected_unit();
	if (unit < fdc.drives.size() && fdc.phase == Phase::Execution &&
	    motor_on(unit)) {
		fdc.drives[unit].head_loaded_until_ms =
		        PIC_FullIndex() +
		        (fdc.timing_specified
		                 ? timer_ms(16.0 * (fdc.head_unload ? fdc.head_unload
		                                                    : 16))
		                 : 0);
	}
	fdc.pio_data.clear();
	fdc.pio_ready       = false;
	fdc.pio_underrun    = false;
	const auto status_0 = check_cast<uint8_t>(
	        (((status_1 || status_2) && !terminal_count) ? 0x40 : 0) |
	        (fdc.head << 2) | selected_unit());
	const auto* sector   = fdc.track_sector;
	const auto command   = static_cast<Command>(fdc.command[0] & 0x1f);
	const auto size_code = command == Command::FormatTrack ? fdc.command[2]
	                     : command == Command::ReadId ? uint8_t{SectorSizeCode}
	                                                  : fdc.command[5];
	fdc.result           = {status_0,
	                        status_1,
	                        check_cast<uint8_t>(status_2 |
                                          (fdc.skipped_control_mark ? 0x40 : 0)),
                      sector ? sector->cylinder : fdc.cylinder,
                      sector ? sector->head : fdc.head,
                      sector ? sector->id : fdc.sector,
                      sector ? sector->size_code : size_code};
	fdc.track_sector     = nullptr;
	fdc.disk.reset();
	fdc.media.reset();
	if (is_machine_pcjr() && !fdc.non_dma &&
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

double rotation_elapsed_ms()
{
	return std::max(0.0, PIC_FullIndex() - fdc.drives[selected_unit()].motor_started_ms);
}

double rotation_ms()
{
	return std::fmod(rotation_elapsed_ms(), rotation_cycle_ms());
}

double next_index_ms()
{
	const auto cycle    = rotation_cycle_ms();
	const auto position = rotation_ms() / cycle;
	if (fdc.media) {
		const auto ends = fdc.media->GetRevolutionEnds(fdc.cylinder,
		                                               fdc.head);
		const auto next = std::upper_bound(ends.begin(), ends.end(), position);
		return (*next - position) * cycle;
	}
	return (1.0 - position) * cycle;
}

bool sample_sector_data(const FloppySector& sector,
                        std::vector<uint8_t>& sampled, const uint64_t turn)
{
	if (sector.weak_mask.empty()) {
		return sector.data_crc_error;
	}
	return sector.SampleWeakData(turn, sampled);
}

bool sample_sector_data(const FloppySector& sector, std::vector<uint8_t>& sampled)
{
	return sample_sector_data(sector,
	                          sampled,
	                          static_cast<uint64_t>(rotation_elapsed_ms() /
	                                                rotation_cycle_ms()));
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
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	if (!fdc.wrong_cylinder_sector) {
		finish_data(fdc.missing_address_mark ? 0x01 : 0x04);
		return;
	}
	fdc.track_sector = fdc.wrong_cylinder_sector;
	finish_data(0x04, 0x10 | (fdc.track_sector->cylinder == 0xff ? 0x02 : 0));
}

void schedule_track_sector(const FloppyMedia& image, const bool read_id)
{
	fdc.sector_transfer_ready = false;
	const auto track          = image.GetTrack(fdc.cylinder, fdc.head);
	const auto position_ms    = rotation_ms();
	const auto cycle_ms       = image.GetCycleMs(fdc.cylinder, fdc.head);
	const auto ends  = image.GetRevolutionEnds(fdc.cylinder, fdc.head);
	const auto next  = std::upper_bound(ends.begin(),
                                           ends.end() - 1,
                                           position_ms / cycle_ms);
	const auto index = check_cast<size_t>(next - ends.begin());
	// Search through two physical index pulses, not one capture cycle.
	// A capture can contain several distinct recorded revolutions.
	const auto second_index   = index + 1 < ends.size()
	                                  ? ends[index + 1] * cycle_ms
	                                  : (1 + ends[(index + 1) % ends.size()]) *
                                                  cycle_ms;
	const auto timeout_ms     = second_index - position_ms;
	auto delay_ms             = timeout_ms;
	fdc.track_sector          = nullptr;
	fdc.missing_address_mark  = true;
	fdc.wrong_cylinder_sector = nullptr;
	auto wrong_delay_ms       = timeout_ms;
	for (const auto& candidate : track) {
		const auto offset_ms = candidate.cycle_fraction * cycle_ms;
		const auto wait_ms   = offset_ms > position_ms
		                             ? offset_ms - position_ms
		                             : cycle_ms + offset_ms - position_ms;
		if (wait_ms >= timeout_ms) {
			continue;
		}
		fdc.missing_address_mark = false;
		if (!read_id && (candidate.head != fdc.command[3] ||
		                 candidate.id != fdc.sector ||
		                 candidate.size_code != fdc.command[5])) {
			continue;
		}
		if (!read_id && candidate.cylinder != fdc.command[2]) {
			if (!candidate.id_crc_error &&
			    (!fdc.wrong_cylinder_sector || wait_ms < wrong_delay_ms)) {
				fdc.wrong_cylinder_sector = &candidate;
				wrong_delay_ms            = wait_ms;
			}
			continue;
		}
		if (!fdc.track_sector || wait_ms < delay_ms) {
			fdc.track_sector = &candidate;
			delay_ms         = wait_ms;
		}
	}
	const auto event = fdc.track_sector
	                         ? (read_id ? complete_read_id : transfer_sector)
	                         : missing_sector;
	if (fdc.track_sector && (read_id || fdc.track_sector->id_crc_error)) {
		const auto& sector = *fdc.track_sector;
		const auto start = sector.revolution ? ends[sector.revolution - 1]
		                                     : 0;
		const auto bit_ms =
		        sector.mfm_track
		                ? (ends[sector.revolution] - start) * cycle_ms /
		                          static_cast<double>(sector.mfm_track->GetRevolutionBitCount(
		                                  sector.revolution))
		                : pio_byte_ms / 16;
		// The rotational ID position is the first A1 sync cell.
		// Read all three sync bytes, FE, CHRN and CRC before reporting
		// Read ID results or rejecting a data command's bad ID CRC.
		delay_ms += 160 * bit_ms;
	}
	PIC_AddEvent(event, delay_ms);
}

void schedule_next_sector()
{
	fdc.sector_transfer_ready = false;
	if (const auto& track = fdc.media) {
		schedule_track_sector(*track, false);
	} else {
		const auto target_ms   = (fdc.sector - 1) * slot_length_ms();
		const auto position_ms = rotation_ms();
		const auto delay_ms    = target_ms > position_ms
		                               ? target_ms - position_ms
		                               : rotation_cycle_ms() + target_ms -
                                                      position_ms;
		PIC_AddEvent(transfer_sector, delay_ms);
	}
}

void verify_sector()
{
	const auto* sector = fdc.track_sector;
	assert(sector);
	std::vector<uint8_t> sampled = {};
	if (sector->id_crc_error) {
		finish_data(0x20);
	} else if (sector->data.empty()) {
		finish_data(0x01, 0x01);
	} else if (sample_sector_data(*sector, sampled, fdc.sector_sample_turn)) {
		finish_data(0x20, 0x20 | (sector->deleted ? 0x40 : 0));
	} else if (sector->deleted) {
		finish_data(0, 0x40);
	} else if (fdc.sector == fdc.command[6]) {
		finish_data(0);
	} else {
		++fdc.sector;
		schedule_next_sector();
	}
}

void start_pio_transfer(const size_t size, const std::span<const uint8_t> source = {})
{
	fdc.pio_writing = source.empty();
	fdc.pio_data.assign(size, 0);
	if (!source.empty()) {
		std::copy(source.begin(), source.end(), fdc.pio_data.begin());
	}
	fdc.pio_offset      = 0;
	fdc.pio_waiting_crc = false;
	fdc.pio_underrun    = false;
	fdc.pio_started_ms  = PIC_FullIndex();
	fdc.pio_ready       = true;
	fdc.irq_pending     = true;
	update_irq();
	PIC_AddEvent(pio_byte_event, pio_byte_ms);
	service_bios_pio();
}

void transfer_track_sector(uint32_t);

void finish_track_sector(const bool terminal_count, const double elapsed_ms)
{
	const auto track   = fdc.media->GetTrack(fdc.cylinder, fdc.head);
	const auto& sector = track[fdc.track_offset];
	if (terminal_count) {
		finish_data(fdc.track_status_1, fdc.track_status_2, true);
	} else if (--fdc.track_remaining == 0) {
		// Read Track's EOT is a sector count and a normal stop
		// condition; unlike Read Data, it does not require DMA terminal
		// count.
		finish_data(fdc.track_status_1, fdc.track_status_2);
	} else if (fdc.track_physical_transfer) {
		// A command-sized field can pass other IDs or even cross index.
		// Resume searching at the physical position reached by the read.
		const auto cycle    = rotation_cycle_ms();
		const auto position = rotation_ms();
		auto delay_ms       = cycle;
		for (size_t i = 0; i < track.size(); ++i) {
			const auto offset_ms = track[i].cycle_fraction * cycle;
			const auto wait_ms   = offset_ms > position
			                             ? offset_ms - position
			                             : cycle + offset_ms - position;
			if (wait_ms < delay_ms) {
				delay_ms         = wait_ms;
				fdc.track_offset = i;
			}
		}
		fdc.track_revolution = track[fdc.track_offset].revolution;
		PIC_AddEvent(transfer_track_sector, delay_ms);
	} else {
		++fdc.track_offset;
		const auto cycle     = rotation_cycle_ms();
		const auto offset_ms = sector.cycle_fraction * cycle;
		const auto has_next  = fdc.track_offset < track.size() &&
		                      track[fdc.track_offset].revolution ==
		                              fdc.track_revolution;
		const auto ends = fdc.media->GetRevolutionEnds(fdc.cylinder,
		                                               fdc.head);
		const auto next = has_next ? track[fdc.track_offset].cycle_fraction
		                           : ends[fdc.track_revolution];
		PIC_AddEvent(transfer_track_sector,
		             std::max(0.0, next * cycle - offset_ms - elapsed_ms));
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
	    track[fdc.track_offset].revolution != fdc.track_revolution) {
		const auto has_id = std::ranges::any_of(track, [](const auto& sector) {
			return sector.revolution == fdc.track_revolution;
		});
		finish_data(fdc.track_status_1 | (has_id ? 0x04 : 0x01),
		            fdc.track_status_2);
		return;
	}
	const auto& sector = track[fdc.track_offset];
	fdc.track_sector   = &sector;
	if (sector.data.empty()) {
		finish_data(fdc.track_status_1 | 0x01, fdc.track_status_2 | 0x01);
		return;
	}
	auto* dma = DMA_GetChannel(FloppyDma);
	if (!fdc.non_dma && (!dma || dma->is_masked || !dma->is_device_to_memory ||
	                     !(fdc.dor & InterruptEnabled))) {
		finish_data(fdc.track_status_1 | 0x10, fdc.track_status_2);
		return;
	}
	const auto size             = size_t{128} << fdc.command[5];
	fdc.track_physical_transfer = sector.mfm_track != nullptr;
	if (fdc.track_physical_transfer && !fdc.track_transfer_ready) {
		const auto ends  = fdc.media->GetRevolutionEnds(fdc.cylinder,
                                                               fdc.head);
		const auto start = fdc.track_revolution
		                         ? ends[fdc.track_revolution - 1]
		                         : 0;
		const auto duration_ms = (ends[fdc.track_revolution] - start) *
		                         rotation_cycle_ms();
		const auto header_ms = static_cast<double>(sector.data_delay_bits) *
		                       duration_ms /
		                       static_cast<double>(sector.mfm_track->GetRevolutionBitCount(
		                               sector.revolution));
		fdc.track_transfer_ms = static_cast<double>(sector.data_delay_bits +
		                                            (size + 2) * 16) *
		                        duration_ms /
		                        static_cast<double>(sector.mfm_track->GetRevolutionBitCount(
		                                sector.revolution));
		fdc.track_sample_turn = static_cast<uint64_t>(
		        rotation_elapsed_ms() / rotation_cycle_ms());
		fdc.track_transfer_ready = true;
		fdc.sector_end_ms = PIC_FullIndex() + fdc.track_transfer_ms;
		PIC_AddEvent(transfer_track_sector,
		             fdc.non_dma ? header_ms : fdc.track_transfer_ms);
		return;
	}
	const auto extended = size > sector.data.size() &&
	                      !sector.extended_data.empty();
	std::span<const uint8_t> data = extended ? sector.extended_data : sector.data;
	std::vector<uint8_t> sampled = {};
	bool bad_crc                 = false;
	if (fdc.track_physical_transfer) {
		// Read Track checks the CRC after the commanded byte count,
		// even when the ID advertises a different data-field size.
		if (!sector.ReadTrackData(size, fdc.track_sample_turn, sampled, bad_crc)) {
			finish_data(fdc.track_status_1 | 0x01,
			            fdc.track_status_2 | 0x01);
			return;
		}
		data = sampled;
	} else if (data.size() < size) {
		finish_data(fdc.track_status_1 | 0x01, fdc.track_status_2 | 0x01);
		return;
	} else {
		bad_crc = !extended && sample_sector_data(sector, sampled);
		if (!sampled.empty()) {
			data = sampled;
		}
	}
	if (sector.id_crc_error || bad_crc) {
		fdc.track_status_1 |= 0x20;
	}
	if (bad_crc) {
		fdc.track_status_2 |= 0x20;
	}
	if (sector.cylinder != fdc.command[2] || sector.head != fdc.command[3] ||
	    sector.size_code != fdc.command[5]) {
		fdc.track_status_1 |= 0x04;
	}
	if (sector.deleted) {
		fdc.track_status_2 |= 0x40;
	}
	if (fdc.non_dma) {
		if (!fdc.track_physical_transfer) {
			fdc.sector_end_ms = PIC_FullIndex() +
			                    static_cast<double>(size + 2) * pio_byte_ms;
		}
		start_pio_transfer(size, std::span(data).first(size));
		return;
	}
	const auto remaining = static_cast<size_t>(dma->curr_count) + 1;
	const auto count     = std::min(remaining, size);
	dma->RaiseRequest();
	const auto transferred = dma->Write(count,
	                                    const_cast<uint8_t*>(data.data()));
	dma->ClearRequest();
	if (transferred != count) {
		finish_data(fdc.track_status_1 | 0x10, fdc.track_status_2);
	} else {
		fdc.track_transfer_ready = false;
		finish_track_sector(count == remaining,
		                    fdc.track_physical_transfer ? fdc.track_transfer_ms
		                                                : 0);
	}
}

bool is_write_command(const Command command)
{
	return command == Command::WriteData || command == Command::WriteDeletedData;
}

bool commit_sector_write(const std::span<const uint8_t> data)
{
	const auto deleted = static_cast<Command>(fdc.command[0] & 0x1f) ==
	                     Command::WriteDeletedData;
	return fdc.track_sector ? fdc.media->WriteSector(fdc.cylinder,
	                                                 fdc.head,
	                                                 fdc.sector,
	                                                 data,
	                                                 fdc.track_sector,
	                                                 deleted)
	                        : fdc.disk->WriteSectorForController(fdc.head,
	                                                             fdc.cylinder,
	                                                             fdc.sector,
	                                                             data);
}

void advance_data_sector()
{
	if (fdc.sector < fdc.command[6]) {
		++fdc.sector;
		schedule_next_sector();
	} else if ((fdc.command[0] & 0x80) && fdc.head == 0 && fdc.disk->heads > 1) {
		fdc.head = 1;
		fdc.command[3] ^= 1;
		fdc.sector = 1;
		schedule_next_sector();
	} else {
		finish_data(0x80);
	}
}

bool data_mark_mismatch()
{
	const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
	if (command != Command::ReadData && command != Command::ReadDeletedData) {
		return false;
	}
	const auto* sector = fdc.track_sector;
	if (sector && sector->data.empty()) {
		return false;
	}
	const auto deleted = sector && sector->deleted;
	return deleted != (command == Command::ReadDeletedData);
}

void finish_sector_transfer(const std::span<const uint8_t> write_data,
                            const bool terminal_count)
{
	const auto wrong_mark = write_data.empty() && data_mark_mismatch();
	if (!write_data.empty() && !commit_sector_write(write_data)) {
		finish_data(0x20);
	} else if (fdc.sector_data_crc_error) {
		finish_data(0x20, 0x20 | (wrong_mark ? 0x40 : 0));
	} else if (wrong_mark) {
		finish_data(0, 0x40);
	} else if (terminal_count) {
		// Normal Read/Write Data completion reports the next logical
		// position (82077AA table 5-6), without physically seeking.
		auto cylinder = fdc.command[2];
		auto head     = fdc.command[3];
		auto sector   = fdc.sector;
		if (sector < fdc.command[6]) {
			++sector;
		} else {
			sector                 = 1;
			const auto multi_track = (fdc.command[0] & 0x80) != 0;
			if (!multi_track || fdc.head == 1) {
				// The on-wire cylinder counter wraps at 255.
				cylinder = static_cast<uint8_t>(cylinder + 1u);
			}
			if (multi_track) {
				head ^= 1;
			}
		}
		finish_data(0);
		fdc.result[3] = cylinder;
		fdc.result[4] = head;
		fdc.result[5] = sector;
	} else {
		advance_data_sector();
	}
}

void transfer_sector(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}

	const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
	const auto* field  = fdc.track_sector;
	const auto writing = is_write_command(command);
	const auto skip    = data_mark_mismatch() && (fdc.command[0] & 0x20);
	if (!fdc.sector_transfer_ready &&
	    (!field || (!field->id_crc_error && (writing || !field->data.empty())))) {
		// The ID identifies a sector; completion follows its data and
		// CRC. PIO transfers start at the data field instead of its
		// end.
		auto bit_ms    = pio_byte_ms / 16;
		auto header_ms = 0.0;
		// Writes create a full command-sized field even if no old DAM
		// exists. The old decoded payload is only a read-size source.
		const auto bytes = writing ? size_t{128} << fdc.command[5]
		                 : field   ? field->data.size()
		                           : SectorSize;
		if (!fdc.media) {
			const auto rate = fdc.disk->sectors >= 36 ? 1000
			                : fdc.disk->sectors >= 15 ? 500
			                                          : 250;
			bit_ms          = 1.0 / (2 * rate);
		}
		if (field && field->mfm_track) {
			const auto ends = fdc.media->GetRevolutionEnds(fdc.cylinder,
			                                               fdc.head);
			const auto start = field->revolution
			                         ? ends[field->revolution - 1]
			                         : 0;
			bit_ms           = (ends[field->revolution] - start) *
			         rotation_cycle_ms() /
			         static_cast<double>(field->mfm_track->GetRevolutionBitCount(
			                 field->revolution));
			header_ms = static_cast<double>(field->data_delay_bits) *
			            bit_ms;
		}
		const auto data_ms = static_cast<double>((bytes + 2) * 16) * bit_ms;
		fdc.sector_end_ms      = PIC_FullIndex() + header_ms + data_ms;
		fdc.sector_sample_turn = static_cast<uint64_t>(
		        rotation_elapsed_ms() / rotation_cycle_ms());
		fdc.sector_transfer_ready = true;
		const auto pio = fdc.non_dma && command != Command::Verify &&
		                 (fdc.command[5] || fdc.command[8]);
		PIC_AddEvent(transfer_sector,
		             header_ms + ((pio || skip) ? 0 : data_ms));
		return;
	}
	if (command == Command::Verify) {
		verify_sector();
		return;
	}

	auto* dma = DMA_GetChannel(FloppyDma);
	if (!fdc.non_dma &&
	    (!dma || dma->is_masked ||
	     !(writing ? dma->is_memory_to_device : dma->is_device_to_memory) ||
	     !(fdc.dor & InterruptEnabled))) {
		finish_data(0x10);
		return;
	}

	std::array<uint8_t, 1024> buffer = {};
	const auto write_size            = fdc.track_sector && writing
	                                         ? size_t{128} << fdc.track_sector->size_code
	                                         : SectorSize;
	const auto transfer_buffer       = std::span(buffer).first(write_size);
	std::span<const uint8_t> data    = transfer_buffer;
	std::vector<uint8_t> sampled     = {};
	fdc.sector_data_crc_error        = false;
	if (field && field->id_crc_error) {
		finish_data(0x20);
		return;
	}
	if (skip) {
		fdc.skipped_control_mark = true;
		advance_data_sector();
		return;
	}
	if (const auto* sector = fdc.track_sector) {
		if (!writing && sector->data.empty()) {
			finish_data(0x01, 0x01);
			return;
		}
		if (!writing) {
			fdc.sector_data_crc_error = sample_sector_data(
			        *sector, sampled, fdc.sector_sample_turn);
			data = sampled.empty() ? sector->data : sampled;
		}
	} else if (!writing && !fdc.disk->ReadSectorForController(fdc.head,
	                                                          fdc.cylinder,
	                                                          fdc.sector,
	                                                          transfer_buffer)) {
		finish_data(0x04);
		return;
	}

	const auto host_size = fdc.command[5] == 0
	                             ? std::min(data.size(), size_t{fdc.command[8]})
	                             : data.size();
	if (!host_size) {
		finish_sector_transfer({}, false);
		return;
	}
	if (fdc.non_dma) {
		start_pio_transfer(host_size,
		                   writing ? std::span<const uint8_t>{}
		                           : data.first(host_size));
		return;
	}
	const auto remaining = static_cast<size_t>(dma->curr_count) + 1;
	const auto count     = std::min(remaining, host_size);
	dma->RaiseRequest();
	// DMA Write copies from the source buffer without modifying it.
	const auto transferred =
	        writing ? dma->Read(count, buffer.data())
	                : dma->Write(count, const_cast<uint8_t*>(data.data()));
	dma->ClearRequest();
	if (transferred != count) {
		finish_data(0x10);
	} else {
		finish_sector_transfer(writing ? transfer_buffer
		                               : std::span<const uint8_t>{},
		                       remaining == count);
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
	                                        fdc.command[3],
	                                        fdc.command[2],
	                                        fdc.format_rate_kbps));
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
	if (fdc.non_dma) {
		start_pio_transfer(FormatIdSize);
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

void finish_raw_format(const std::span<const uint8_t> ids)
{
	const auto result = FLOPPY_FormatTrack(*fdc.disk,
	                                       nullptr,
	                                       fdc.cylinder,
	                                       fdc.head,
	                                       ids,
	                                       fdc.command[5],
	                                       fdc.command[3],
	                                       fdc.command[2],
	                                       fdc.format_rate_kbps);
	if (result == FloppyFormatResult::Success) {
		fdc.sector = fdc.command[3];
	}
	finish_data(format_status(result));
}

void format_raw_track(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	std::array<uint8_t, FormatIdSize * 255> ids = {};
	const auto count = fdc.command[3] * FormatIdSize;
	if (fdc.non_dma) {
		start_pio_transfer(count);
		return;
	}
	auto* dma = DMA_GetChannel(FloppyDma);
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

	finish_raw_format(std::span(ids).first(count));
}

void finish_pio_transfer()
{
	const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
	if ((command == Command::ReadData || command == Command::ReadDeletedData ||
	     is_write_command(command) || command == Command::ReadTrack) &&
	    !fdc.pio_waiting_crc && PIC_FullIndex() < fdc.sector_end_ms) {
		// Defer once: rounding at the target timestamp must not queue
		// an endless sequence of events with effectively zero delay.
		fdc.pio_waiting_crc = true;
		PIC_AddEvent(pio_byte_event, fdc.sector_end_ms - PIC_FullIndex());
		return;
	}
	const auto elapsed_ms = PIC_FullIndex() - fdc.pio_started_ms;
	const auto data       = std::move(fdc.pio_data);
	fdc.pio_data.clear();
	switch (command) {
	case Command::ReadTrack:
		fdc.track_transfer_ready = false;
		finish_track_sector(false, elapsed_ms);
		break;
	case Command::WriteDeletedData:
	case Command::WriteData: finish_sector_transfer(data, false); break;
	case Command::ReadData:
	case Command::ReadDeletedData: finish_sector_transfer({}, false); break;
	case Command::FormatTrack:
		if (!fdc.media) {
			finish_raw_format(data);
			break;
		}
		std::copy(data.begin(),
		          data.end(),
		          fdc.format_ids.begin() + fdc.formatted_ids * FormatIdSize);
		if (++fdc.formatted_ids == fdc.command[3]) {
			finish_track_format(0);
		} else {
			PIC_AddEvent(format_track_sector,
			             std::max(0.0, track_format_slot_ms() - elapsed_ms));
		}
		break;
	default: assert(false); break;
	}
}

void pio_byte_event(uint32_t)
{
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	if (fdc.pio_underrun) {
		const auto written = commit_sector_write(fdc.pio_data);
		finish_data(written ? 0x10 : 0x20);
		return;
	}
	if (fdc.pio_ready) {
		const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
		if (command == Command::ReadTrack) {
			finish_data(fdc.track_status_1 | 0x10, fdc.track_status_2);
		} else if (command == Command::FormatTrack && fdc.media) {
			finish_track_format(0x10);
		} else {
			if (is_write_command(command)) {
				// Stop requesting bytes, but clock the
				// zero-filled remainder and CRC before
				// committing the write. Reset and motor loss
				// can still abort it meanwhile.
				fdc.pio_underrun = true;
				fdc.pio_ready    = false;
				fdc.irq_pending  = false;
				update_irq();
				PIC_AddEvent(pio_byte_event,
				             std::max(0.0,
				                      fdc.sector_end_ms -
				                              PIC_FullIndex()));
			} else {
				finish_data(0x10);
			}
		}
		return;
	}
	if (fdc.pio_offset == fdc.pio_data.size()) {
		finish_pio_transfer();
		return;
	}
	fdc.pio_ready   = true;
	fdc.irq_pending = true;
	update_irq();
	PIC_AddEvent(pio_byte_event, pio_byte_ms);
	service_bios_pio();
}

void acknowledge_pio_byte()
{
	fdc.pio_ready   = false;
	fdc.irq_pending = false;
	update_irq();
}

void start_data(const Command command)
{
	fdc.skipped_control_mark  = false;
	fdc.wrong_cylinder_sector = nullptr;
	const auto unit           = selected_unit();
	fdc.disk                  = BIOS_GetDisk(unit);
	fdc.media                 = BIOS_GetFloppyMedia(unit);
	fdc.head                  = (fdc.command[1] >> 2) & 1;
	fdc.cylinder = unit < fdc.drives.size() ? fdc.drives[unit].cylinder : 0;
	fdc.sector = (command == Command::ReadId || command == Command::FormatTrack)
	                   ? 1
	                   : fdc.command[4];

	if (!media_ready(unit) || fdc.head >= fdc.disk->heads ||
	    fdc.cylinder >= fdc.disk->cylinders) {
		finish_data(0x04);
		return;
	}

	if ((is_write_command(command) || command == Command::FormatTrack) &&
	    fdc.disk->IsWriteProtected()) {
		finish_data(0x02);
		return;
	}
	// Raw sector files cannot retain the deleted data-address mark.
	if (command == Command::WriteDeletedData && !fdc.media) {
		finish_data(0x04);
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
		fdc.formatted_ids = 0;
	}
	const auto load_ms = head_load_delay_ms(unit);
	fdc.drives[unit].head_loaded_until_ms = std::numeric_limits<double>::infinity();
	if (load_ms > 0) {
		PIC_AddEvent(start_loaded_data, load_ms);
	} else {
		start_loaded_data(0);
	}
}

void start_loaded_data(uint32_t)
{
	const auto command = static_cast<Command>(fdc.command[0] & 0x1f);
	if (!media_ready(selected_unit())) {
		finish_data(0x04);
		return;
	}
	if (command == Command::FormatTrack) {
		fdc.format_rate_kbps = data_rate_kbps;
		const auto raw       = !fdc.media;
		if (fdc.command[3] == 0 || fdc.command[2] > 7 ||
		    (raw && (fdc.command[2] != SectorSizeCode ||
		             fdc.command[3] != fdc.disk->sectors ||
		             fdc.disk->getSectSize() != SectorSize))) {
			finish_data(0x04);
			return;
		}
		fdc.formatted_ids   = 0;
		const auto to_index = next_index_ms();
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
		fdc.track_status_1 = 0;
		fdc.track_status_2 = 0;
		const auto track   = image->GetTrack(fdc.cylinder, fdc.head);
		const auto ends = image->GetRevolutionEnds(fdc.cylinder, fdc.head);
		const auto cycle    = rotation_cycle_ms();
		const auto position = rotation_ms() / cycle;
		// The final index wraps to the first recorded revolution.
		const auto index = std::upper_bound(ends.begin(),
		                                    ends.end() - 1,
		                                    position);
		const auto next  = check_cast<size_t>(index - ends.begin()) + 1;
		fdc.track_revolution = next % ends.size();
		const auto first     = std::lower_bound(
                        track.begin(),
                        track.end(),
                        fdc.track_revolution,
                        [](const FloppySector& sector, const size_t revolution) {
                                return sector.revolution < revolution;
                        });
		fdc.track_offset    = check_cast<size_t>(first - track.begin());
		fdc.track_remaining = fdc.command[6];
		fdc.track_transfer_ready = false;
		const auto has_sector    = first != track.end() &&
		                        first->revolution == fdc.track_revolution;
		const auto first_position = has_sector
		                                  ? first->cycle_fraction
		                                  : ends[fdc.track_revolution];
		const auto wrap = fdc.track_revolution == 0 ? 1.0 : 0.0;
		PIC_AddEvent(transfer_track_sector,
		             (wrap + first_position - position) * cycle);
		return;
	}
	if (const auto& track = fdc.media) {
		if (command != Command::ReadId &&
		    (fdc.sector > fdc.command[6] ||
		     (is_write_command(command) &&
		      !IsFloppyOverlaySizeCode(fdc.command[5])))) {
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

	schedule_next_sector();
}

void complete_seek(const uint32_t unit)
{
	auto& seek            = fdc.seeks[unit];
	const auto drive_bits = (seek.head << 2) | unit;
	if (motor_on(unit) && unit == (fdc.dor & 3u)) {
		auto& drive = fdc.drives[unit];
		if (drive.cylinder != seek.cylinder && BIOS_GetDisk(unit)) {
			FLOPPY_SetDiskChanged(unit, false);
		}
		drive.cylinder = seek.cylinder;
		seek.status    = check_cast<uint8_t>(0x20u | drive_bits);
	} else {
		seek.status = check_cast<uint8_t>(0x68u | drive_bits);
	}
	seek.cylinder = unit < fdc.drives.size() ? fdc.drives[unit].cylinder : 0;
	seek.active          = false;
	seek.pending         = true;
	fdc.seek_irq_pending = true;
	update_irq();
}

void execute_command(const Command command)
{
	switch (command) {
	case Command::Specify:
		fdc.non_dma          = (fdc.command[2] & 1) != 0;
		fdc.step_rate        = fdc.command[1] >> 4;
		fdc.head_unload      = fdc.command[1] & 0x0f;
		fdc.head_load        = fdc.command[2] >> 1;
		fdc.timing_specified = true;
		break;
	case Command::SenseInterrupt: {
		fdc.seek_irq_pending = false;
		const auto pending   = std::ranges::find_if(
                        fdc.seeks, [](const auto& seek) { return seek.pending; });
		if (fdc.reset_unit < 4) {
			const auto unit = fdc.reset_unit++;
			fdc.result = {check_cast<uint8_t>(0xc0u | unit), 0};
			begin_result(2, false);
		} else if (pending != fdc.seeks.end()) {
			pending->pending = false;
			fdc.result       = {pending->status, pending->cylinder};
			begin_result(2, false);
		} else {
			fdc.result[0] = 0x80;
			begin_result(1, false);
		}
		break;
	}
	case Command::Seek:
	case Command::Recalibrate: {
		const auto unit = selected_unit();
		auto& seek      = fdc.seeks[unit];
		if (seek.active || seek.pending) {
			fdc.result[0] = 0x80;
			begin_result(1, false);
			break;
		}
		const auto cylinder = unit < fdc.drives.size()
		                            ? fdc.drives[unit].cylinder
		                            : 0;
		seek.cylinder = command == Command::Seek ? fdc.command[2] : 0;
		seek.head     = (fdc.command[1] >> 2) & 1;
		seek.active   = true;
		const auto steps = std::max(1, std::abs(cylinder - seek.cylinder));
		// Seek has no result phase of its own. Keep accepting commands,
		// and latch the drive independently until Sense Interrupt.
		const auto step_ms = fdc.timing_specified
		                           ? timer_ms(16.0 - fdc.step_rate)
		                           : DefaultStepMs;
		PIC_AddEvent(complete_seek,
		             steps * step_ms,
		             check_cast<uint32_t>(unit));
		break;
	}
	case Command::ReadId:
	case Command::FormatTrack:
	case Command::WriteDeletedData:
	case Command::WriteData:
	case Command::ReadData:
	case Command::ReadDeletedData:
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
	case Command::WriteDeletedData:
	case Command::WriteData:
	case Command::ReadData:
	case Command::ReadDeletedData:
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
	if (fdc.phase == Phase::Execution && fdc.pio_ready && fdc.pio_writing) {
		fdc.pio_data[fdc.pio_offset++] = check_cast<uint8_t>(value);
		acknowledge_pio_byte();
		return;
	}
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
	if (fdc.phase == Phase::Execution && fdc.pio_ready && !fdc.pio_writing) {
		const auto value = fdc.pio_data[fdc.pio_offset++];
		acknowledge_pio_byte();
		return value;
	}
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

void service_bios_pio()
{
	if (!bios_pio || !fdc.pio_ready || bios_pio->transferred >= bios_pio->size ||
	    fdc.pio_writing != bios_pio->writing) {
		return;
	}
	// The PCjr BIOS increments its 16-bit buffer offset within a fixed
	// segment, rather than programming an 8237 physical-page boundary.
	const auto address = bios_pio->base +
	                     static_cast<uint16_t>(bios_pio->offset +
	                                           bios_pio->transferred);
	if (bios_pio->writing) {
		write_data(0xf5, mem_readb(address), io_width_t::byte);
	} else {
		mem_writeb(address,
		           check_cast<uint8_t>(read_data(0xf5, io_width_t::byte)));
	}
	++bios_pio->transferred;
}

io_val_t read_status(io_port_t, io_width_t)
{
	if (!(fdc.dor & ResetReleased)) {
		return 0;
	}
	uint8_t busy = 0;
	for (size_t unit = 0; unit < fdc.seeks.size(); ++unit) {
		if (fdc.seeks[unit].active || fdc.seeks[unit].pending) {
			busy |= check_cast<uint8_t>(1u << unit);
		}
	}
	switch (fdc.phase) {
	case Phase::Execution:
		if (fdc.pio_data.empty()) {
			return busy | (fdc.non_dma ? 0x30 : 0x10);
		}
		return busy | 0x30 | (fdc.pio_ready ? 0x80 : 0) |
		       (fdc.pio_writing ? 0 : 0x40);
	case Phase::Result: return busy | 0xd0;
	case Phase::Command: return busy | (fdc.command_count ? 0x90 : 0x80);
	}
	return 0;
}

io_val_t read_dir(io_port_t, io_width_t)
{
	const auto unit = fdc.dor & 3u;
	return IDE_ReadPrimaryDriveAddress() |
	       (FLOPPY_IsDiskChanged(unit) ? 0x80 : 0);
}

void write_ccr(io_port_t, const io_val_t value, io_width_t)
{
	constexpr std::array<uint16_t, 4> DataRatesKbps = {500, 300, 250, 1000};
	data_rate_kbps = DataRatesKbps[value & 3u];
	pio_byte_ms    = 8.0 / data_rate_kbps;
}

void reset_controller()
{
	PIC_RemoveEvents(start_loaded_data);
	PIC_RemoveEvents(complete_seek);
	PIC_RemoveEvents(complete_read_id);
	PIC_RemoveEvents(transfer_sector);
	PIC_RemoveEvents(transfer_track_sector);
	PIC_RemoveEvents(format_raw_track);
	PIC_RemoveEvents(format_track_sector);
	PIC_RemoveEvents(missing_sector);
	PIC_RemoveEvents(pio_byte_event);
	const auto drives = fdc.drives;
	fdc               = {};
	// Resetting the controller does not move the physical drive heads.
	fdc.drives = drives;
	for (auto& drive : fdc.drives) {
		drive.head_loaded_until_ms = 0;
	}
}

void write_dor(io_port_t, const io_val_t value, io_width_t)
{
	const auto previous     = fdc.dor;
	const auto unit         = selected_unit();
	const auto writing_data = fdc.phase == Phase::Execution &&
	                          is_write_command(static_cast<Command>(
	                                  fdc.command[0] & 0x1f));
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
		if (!motor_on(unit)) {
			fdc.drives[unit].head_loaded_until_ms = 0;
		}
		if (motor_on(unit) && !(previous & (0x10u << unit))) {
			fdc.drives[unit].motor_started_ms = PIC_FullIndex();
		}
	}
	if ((value & ResetReleased) && !(previous & ResetReleased)) {
		fdc.reset_unit  = 0;
		fdc.irq_pending = true;
	}
	if ((value & ResetReleased) && formatting_track && !media_ready(unit)) {
		PIC_RemoveEvents(format_track_sector);
		finish_track_format(0x04);
		return;
	}
	if ((value & ResetReleased) && !media_ready(unit)) {
		if (writing_data) {
			PIC_RemoveEvents(transfer_sector);
			PIC_RemoveEvents(missing_sector);
		}
		if (writing_data || !fdc.pio_data.empty()) {
			finish_data(0x04);
			return;
		}
	}
	update_irq();
}

void pcjr_watchdog_expired(uint32_t)
{
	if (!pcjr_watchdog_asserted) {
		pcjr_watchdog_asserted = true;
		PIC_ActivateIRQ(FloppyIrq);
	}
}

// The PCjr adapter exposes one drive and uses a different DOR layout.
void write_pcjr_dor(io_port_t, const io_val_t value, const io_width_t width)
{
	const auto previous = std::exchange(pcjr_dor, check_cast<uint8_t>(value));
	if (!(pcjr_dor & PcjrWatchdogEnable)) {
		PIC_RemoveEvents(pcjr_watchdog_expired);
		// The PIC has already latched any edge. IRQ6 is also used by
		// the emulated PCjr keyboard, so leave the PIC request alone.
		pcjr_watchdog_asserted = false;
	} else if ((previous & PcjrWatchdogTrigger) &&
	           !(pcjr_dor & PcjrWatchdogTrigger)) {
		PIC_RemoveEvents(pcjr_watchdog_expired);
		PIC_AddEvent(pcjr_watchdog_expired, PcjrWatchdogMs);
	}

	const auto translated = ((value & 0x80) ? ResetReleased : 0) |
	                        ((value & 1) ? 0x10 : 0);
	const auto was_reset = !(fdc.dor & ResetReleased);
	write_dor(0x3f2, translated, width);
	if (was_reset || !(translated & ResetReleased)) {
		fdc.non_dma = true;
	}
}

} // namespace

FloppyFormatResult FLOPPY_FormatTrack(imageDisk& disk, FloppyMedia* media,
                                      const uint32_t cylinder, const uint32_t head,
                                      const std::span<const uint8_t> ids,
                                      const uint8_t fill, const uint8_t requested,
                                      const uint8_t size_code, uint16_t rate_kbps)
{
	using Result = FloppyFormatResult;
	if (disk.IsWriteProtected()) {
		return Result::WriteProtected;
	}
	if (disk.hardDrive || cylinder > UINT8_MAX ||
	    cylinder >= disk.cylinders || head >= disk.heads ||
	    head > UINT8_MAX || ids.empty() || ids.size() % FormatIdSize ||
	    ids.size() / FormatIdSize > requested || size_code > 7) {
		return Result::InvalidLayout;
	}

	if (media) {
		if (!rate_kbps) {
			rate_kbps = bios_data_rate(disk);
		}
		using ScpResult = FloppyMedia::FormatResult;
		switch (media->FormatTrack(check_cast<uint8_t>(cylinder),
		                           check_cast<uint8_t>(head),
		                           ids,
		                           fill,
		                           requested,
		                           size_code,
		                           rate_kbps)) {
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
                               const bool writing, const int revolutions,
                               const uint16_t buffer_offset = 0)
{
	if (!FLOPPY_CanStartBiosCommand()) {
		return {.error = 0x80};
	}
	const auto disk = BIOS_GetDisk(unit);
	if (unit >= fdc.drives.size() || !disk || !disk->active ||
	    disk->hardDrive || cylinder > UINT8_MAX ||
	    cylinder >= disk->cylinders || head > 1 || head >= disk->heads) {
		return {.error = 0x04};
	}
	constexpr auto MaskChannel = 0x04 | FloppyDma;
	const auto pcjr            = is_machine_pcjr();
	auto* dma = size && !pcjr ? DMA_GetChannel(FloppyDma) : nullptr;
	if (size && !pcjr) {
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
	const auto previous_non_dma  = fdc.non_dma;
	fdc.non_dma                  = pcjr;
	BiosPioTransfer pio_transfer = {
	        address - buffer_offset, size, 0, buffer_offset, writing};
	bios_pio = pcjr && size ? &pio_transfer : nullptr;

	const auto track    = BIOS_GetFloppyMedia(unit);
	const auto cycle_ms = track ? track->GetCycleMs(cylinder, head)
	                            : StandardRevolutionMs;
	const auto deadline = PIC_FullIndex() + head_load_delay_ms(unit) +
	                      revolutions * cycle_ms;
	const auto mask = IO_ReadB(0x21);
	if (!pcjr) {
		PIC_SetIRQMask(FloppyIrq, false);
	}
	mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) & 0x7f);
	for (const auto byte : command) {
		write_data(0x3f5, byte, io_width_t::byte);
	}

	bool shutdown = false;
	while (fdc.phase == Phase::Execution && PIC_FullIndex() < deadline &&
	       !shutdown) {
		shutdown = CALLBACK_Idle();
	}
	if (!pcjr && !shutdown && fdc.phase == Phase::Result && fdc.irq_pending) {
		shutdown = CALLBACK_Idle();
	}
	bios_pio    = nullptr;
	fdc.non_dma = previous_non_dma;
	if (!pcjr) {
		PIC_SetIRQMask(FloppyIrq, (mask & (1u << FloppyIrq)) != 0);
	}
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
	result.transferred       = pio_transfer.transferred;
	for (auto& byte : result.status) {
		byte = check_cast<uint8_t>(read_data(0x3f5, io_width_t::byte));
	}
	MEM_BlockWrite(BIOS_FDC_RESULT_BUFFER,
	               result.status.data(),
	               result.status.size());
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
        const uint32_t address, const uint16_t buffer_offset)
{
	const auto last_sector = sector + count - 1;
	if (!count || last_sector > UINT8_MAX || size_code > 7 || unit > 1 ||
	    cylinder > UINT8_MAX || head > 1) {
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
	        unit, cylinder, head, command, address, size, writing, 3 + count, buffer_offset);
	if (result.error) {
		return {result.error, 0};
	}

	const auto st0 = result.status[0];
	const auto st1 = result.status[1];
	const auto st2 = result.status[2];
	// Non-DMA transfers stop at EOT without a DMA terminal-count input.
	const auto pio_end = is_machine_pcjr() && !verifying && st1 == 0x80 &&
	                     !st2 && result.transferred == size &&
	                     result.status[5] == last_sector;
	const auto failed       = ((st0 & 0xc0) || st1 || st2) && !pio_end;
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
        const uint32_t address, const uint16_t buffer_offset)
{
	const auto disk = BIOS_GetDisk(unit);
	if (!disk || !count || size_code > 7 || head >= disk->heads) {
		return {0x04, 0};
	}

	const auto sector_size = 128u << size_code;
	const auto has_data    = operation != FloppyBiosTransfer::Verify;
	const auto dma         = has_data && !is_machine_pcjr();
	if (dma && count * sector_size > 0x10000u - (address & 0xffff)) {
		return {0x09, 0};
	}

	// IDs outside the logical geometry, including zero, can belong to a
	// protection track. Even a missing ID must seek before failing.
	if (!sector || sector > disk->sectors) {
		return bios_transfer_track(operation,
		                           unit,
		                           cylinder,
		                           head,
		                           sector,
		                           size_code,
		                           count,
		                           gap,
		                           address,
		                           buffer_offset);
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
		const auto offset = static_cast<uint16_t>(
		        buffer_offset + completed * sector_size);
		const auto next_address = dma ? address + completed * sector_size
		                              : address - buffer_offset + offset;
		const auto result =
		        bios_transfer_track(operation,
		                            unit,
		                            current_cylinder,
		                            check_cast<uint8_t>(current_head),
		                            check_cast<uint8_t>(current_sector),
		                            size_code,
		                            check_cast<uint8_t>(run),
		                            gap,
		                            has_data ? next_address : 0u,
		                            has_data ? offset : uint16_t{0});
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

uint8_t FLOPPY_BiosFormat(const uint8_t unit, const uint32_t cylinder,
                          const uint8_t head, const uint8_t size_code,
                          const uint8_t count, const uint8_t gap, const uint8_t fill,
                          const uint32_t address, const uint16_t buffer_offset)
{
	if (unit > 1 || head > 1 || !count || size_code > 7) {
		return 0x04;
	}
	const std::array<uint8_t, 6> command = {
	        0x4d, check_cast<uint8_t>(unit | (head << 2)), size_code, count, gap, fill};
	const auto disk = BIOS_GetDisk(unit);
	if (!disk) {
		return 0x04;
	}
	if (!FLOPPY_CanStartBiosCommand()) {
		return 0x80;
	}
	const auto rate = bios_data_rate(*disk);
	write_ccr(0x3f7, rate == 1000 ? 3 : (rate == 500 ? 0 : 2), io_width_t::byte);
	const auto size     = check_cast<uint32_t>(count * FormatIdSize);
	const auto transfer = bios_command(
	        unit, cylinder, head, command, address, size, true, 3, buffer_offset);
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

bool FLOPPY_CanStartBiosCommand()
{
	return fdc.phase == Phase::Command && !fdc.command_count &&
	       !fdc.irq_pending && !fdc.seek_irq_pending &&
	       std::ranges::none_of(fdc.seeks,
	                            [](const auto& seek) { return seek.active; });
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
	// Boot loaders can use the BIOS motor mask to identify their boot drive.
	mem_writeb(BIOS_DRIVE_RUNNING,
	           check_cast<uint8_t>((mem_readb(BIOS_DRIVE_RUNNING) & 0xf0) |
	                               (fdc.dor >> 4)));
	const auto parameters = RealToPhysical(RealGetVec(0x1e));
	mem_writeb(BIOS_DISK_MOTOR_TIMEOUT, mem_readb(parameters + 2));
}

void FLOPPY_BiosReset(const uint8_t unit)
{
	assert(unit < fdc.drives.size());
	const auto dor = check_cast<uint8_t>((fdc.dor & 0xf0) | ResetReleased |
	                                     InterruptEnabled | unit);
	const auto was_masked = (IO_ReadB(0x21) & (1u << FloppyIrq)) != 0;
	if (is_machine_pcjr()) {
		// Cancel the adapter watchdog without touching its shared
		// keyboard NMI.
		PIC_RemoveEvents(pcjr_watchdog_expired);
		pcjr_watchdog_asserted = false;
		pcjr_dor = check_cast<uint8_t>(0x80u | ((dor & 0x10) ? 1u : 0u));
	} else {
		PIC_SetIRQMask(FloppyIrq, false);
	}
	mem_writeb(BIOS_DRIVE_ACTIVE, mem_readb(BIOS_DRIVE_ACTIVE) & 0x7f);
	write_dor(0x3f2, dor & ~ResetReleased, io_width_t::byte);
	write_dor(0x3f2, dor, io_width_t::byte);
	if (is_machine_pcjr()) {
		fdc.non_dma = true;
	} else {
		const auto shutdown = CALLBACK_Idle();
		PIC_SetIRQMask(FloppyIrq, was_masked);
		if (shutdown) {
			return;
		}
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
	data_rate_kbps = DefaultDataRateKbps;
	pio_byte_ms    = 8.0 / DefaultDataRateKbps;
	PIC_RemoveEvents(pcjr_watchdog_expired);
	pcjr_dor               = 0;
	pcjr_watchdog_asserted = false;
	disk_changed.fill(false);
	reset_controller();
	fdc.drives = {};
	update_irq();
	IO_RegisterWriteHandler(0x3f2, write_dor, io_width_t::byte);
	IO_RegisterReadHandler(0x3f4, read_status, io_width_t::byte);
	IO_RegisterReadHandler(0x3f5, read_data, io_width_t::byte);
	IO_RegisterWriteHandler(0x3f5, write_data, io_width_t::byte);
	if (is_machine_pcjr()) {
		IO_RegisterWriteHandler(0xf2, write_pcjr_dor, io_width_t::byte);
		IO_RegisterReadHandler(0xf4, read_status, io_width_t::byte);
		IO_RegisterReadHandler(0xf5, read_data, io_width_t::byte);
		IO_RegisterWriteHandler(0xf5, write_data, io_width_t::byte);
	} else {
		IO_RegisterReadHandler(0x3f7, read_dir, io_width_t::byte);
		IO_RegisterWriteHandler(0x3f7, write_ccr, io_width_t::byte);
	}
}
