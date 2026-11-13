// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_track_image.h"

#include "ints/bitstream_capture.h"
#include "utils/checks.h"

CHECK_NARROWING();

std::unique_ptr<FloppyTrackImage> FloppyTrackImage::LoadTransCopy(
        FILE* file, const std::string& path, const bool read_only, std::string& error)
{
	const auto fail = [&](const char* message) {
		error = message;
		return std::unique_ptr<FloppyTrackImage>{};
	};
	if (fseek(file, 0, SEEK_END) != 0) {
		return fail("cannot seek TransCopy image");
	}
	const auto size = ftell(file);
	if (size < static_cast<long>(0x905) || size > 0x1010000 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		return fail("invalid TransCopy image size");
	}

	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		return fail("cannot read TransCopy image");
	}
	const auto capture = ParseTransCopyCapture(bytes, error);
	if (!capture) {
		return {};
	}
	return FromBitstreamCapture(
	        *capture, path, read_only, FloppyImageFingerprint(bytes), error);
}
