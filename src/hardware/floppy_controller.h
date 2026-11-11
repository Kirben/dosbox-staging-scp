// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_CONTROLLER_H
#define DOSBOX_FLOPPY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <span>

class FloppyMedia;
class imageDisk;

enum class FloppyFormatResult {
	Success,
	WriteProtected,
	InvalidLayout,
	WriteError
};

FloppyFormatResult FLOPPY_FormatTrack(imageDisk& disk, FloppyMedia* media,
                                      uint32_t cylinder, uint32_t head,
                                      std::span<const uint8_t> ids,
                                      uint8_t fill, uint8_t requested);

enum class FloppyBiosTransfer { Read, Write, Verify };

struct FloppyBiosTransferResult {
	uint8_t status  = 0;
	uint8_t sectors = 0;
};

FloppyBiosTransferResult FLOPPY_BiosTransfer(
        const FloppyBiosTransfer operation, const uint8_t unit,
        const uint32_t cylinder, const uint8_t head, const uint8_t sector,
        const uint8_t size_code, const uint8_t count, const uint8_t gap,
        const uint32_t address);

uint8_t FLOPPY_BiosDmaFormat(uint8_t unit, uint32_t cylinder, uint8_t head,
                             uint8_t size_code, uint8_t count, uint8_t gap,
                             uint8_t fill, uint32_t address);

void FLOPPY_SetDiskChanged(const size_t unit, const bool changed);
bool FLOPPY_IsDiskChanged(const size_t unit);

void FLOPPY_BiosPosition(const uint8_t unit, const uint8_t cylinder);
void FLOPPY_BiosReset(const uint8_t unit);
void FLOPPY_Init();

#endif
