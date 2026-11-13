// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_BITSTREAM_CAPTURE_H
#define DOSBOX_BITSTREAM_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Container data, independent of sector decoding, BIOS/FDC state, and overlays.
struct BitstreamCaptureTrack {
	std::vector<uint8_t> cells = {};
	size_t bit_count           = 0;
	size_t index               = 0;
	double cycle_ms            = 200.0;
	bool detect_weak           = false;
};

struct BitstreamCapture {
	uint32_t cylinders                        = 0;
	uint32_t heads                            = 0;
	bool read_only                            = false;
	std::vector<BitstreamCaptureTrack> tracks = {};
};

std::optional<BitstreamCapture> Parse86fCapture(const std::span<const uint8_t> bytes,
                                                std::string& error);

std::optional<BitstreamCapture> ParseHxcMfmCapture(const std::span<const uint8_t> bytes,
                                                   std::string& error);

std::optional<BitstreamCapture> ParseTransCopyCapture(const std::span<const uint8_t> bytes,
                                                      std::string& error);

#endif
