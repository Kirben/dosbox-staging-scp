// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_CONTROLLER_H
#define DOSBOX_FLOPPY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <optional>
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
                                      const uint32_t cylinder, const uint32_t head,
                                      const std::span<const uint8_t> ids,
                                      const uint8_t fill, const uint8_t requested,
                                      const uint8_t size_code,
                                      uint16_t rate_kbps = 0);

enum class FloppyBiosTransfer { Read, Write, Verify };

struct FloppyBiosTransferResult {
	uint8_t status  = 0;
	uint8_t sectors = 0;
};

// INT 13h supplies the guest's EOT; a pre-boot read uses the image geometry.
FloppyBiosTransferResult FLOPPY_BiosTransfer(
        const FloppyBiosTransfer operation, const uint8_t unit,
        const uint32_t cylinder, const uint8_t head, const uint8_t sector,
        const uint8_t size_code, const uint8_t count, const uint8_t gap,
        const uint32_t address, const uint16_t buffer_offset = 0,
        const std::optional<uint8_t> end_of_track = {});

uint8_t FLOPPY_BiosFormat(const uint8_t unit, const uint32_t cylinder,
                          const uint8_t head, const uint8_t size_code,
                          const uint8_t count, const uint8_t gap, const uint8_t fill,
                          const uint32_t address, const uint16_t buffer_offset);

void FLOPPY_SetDiskChanged(const size_t unit, const bool changed);
bool FLOPPY_IsDiskChanged(const size_t unit);

bool FLOPPY_CanStartBiosCommand();
void FLOPPY_BiosPosition(const uint8_t unit, const uint8_t cylinder);
void FLOPPY_BiosReset(const uint8_t unit);
void FLOPPY_Init();

#endif
