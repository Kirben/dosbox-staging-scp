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

uint8_t FLOPPY_BiosDmaFormat(uint8_t unit, uint32_t cylinder, uint8_t head,
                             uint8_t size_code, uint8_t count, uint8_t gap,
                             uint8_t fill, uint32_t address);

void FLOPPY_Init();

#endif
