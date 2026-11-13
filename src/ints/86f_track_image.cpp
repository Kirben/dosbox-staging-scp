// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include "ints/bitstream_capture.h"
#include "utils/checks.h"

CHECK_NARROWING();

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::Load86f(FILE* file,
                                                            const std::string& path,
                                                            const bool read_only,
                                                            std::string& error)
{
	error.clear();
	if (fseek(file, 0, SEEK_END)) {
		error = "cannot seek 86F image";
		return {};
	}
	const auto size = ftell(file);
	if (size < 8 || size > 128 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
		error = "invalid 86F image size";
		return {};
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		error = "cannot read 86F image";
		return {};
	}
	const auto capture = Parse86fCapture(bytes, error);
	if (!capture) {
		return {};
	}
	return FromBitstreamCapture(
	        *capture, path, read_only, FloppyImageFingerprint(bytes), error);
}
