// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_CONTROLLER_H
#define DOSBOX_FLOPPY_CONTROLLER_H

#include <cstdint>
#include <span>

class imageDisk;

enum class FloppyFormatResult {
	Success,
	WriteProtected,
	InvalidLayout,
	WriteError
};

FloppyFormatResult FLOPPY_FormatTrack(imageDisk& disk, uint32_t cylinder,
                                      uint32_t head, std::span<const uint8_t> ids,
                                      uint8_t fill, uint8_t requested);

enum class FloppyBiosTransfer { Read, Write };

struct FloppyBiosTransferResult {
	uint8_t status  = 0;
	uint8_t sectors = 0;
};

FloppyBiosTransferResult FLOPPY_BiosDmaTransfer(
        const FloppyBiosTransfer operation, const uint8_t unit,
        const uint32_t cylinder, const uint8_t head, const uint8_t sector,
        const uint8_t size_code, const uint8_t count, const uint8_t gap,
        const uint32_t address);

uint8_t FLOPPY_BiosDmaFormat(uint8_t unit, uint32_t cylinder, uint8_t head,
                             uint8_t size_code, uint8_t count, uint8_t gap,
                             uint8_t fill, uint32_t address);

void FLOPPY_Init();

#endif
