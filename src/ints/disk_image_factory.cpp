// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/disk_image_factory.h"

#include <cstdio>
#include <limits>

#include "ints/bios_disk.h"
#include "misc/cross.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

namespace {

enum class TrackFormat { None, HxcMfm, Scp, F86 };

TrackFormat track_format(const std::string_view path)
{
	const auto dot = path.rfind('.');
	if (dot == std::string_view::npos) {
		return TrackFormat::None;
	}
	const auto extension = path.substr(dot);
	if (iequals(extension, ".mfm")) {
		return TrackFormat::HxcMfm;
	}
	if (iequals(extension, ".scp")) {
		return TrackFormat::Scp;
	}
	if (iequals(extension, ".86f")) {
		return TrackFormat::F86;
	}
	return TrackFormat::None;
}

} // namespace

std::shared_ptr<imageDisk> OpenSectorDiskImage(const std::string& path,
                                               bool& read_only,
                                               const std::optional<bool> hard_disk)
{
	const auto is_track_image = IsFloppyTrackImagePath(path);
	if (is_track_image && hard_disk.value_or(false)) {
		return {};
	}
	read_only |= is_track_image;
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(
	        fopen_wrap_ro_fallback(path, read_only), close_file);
	if (!file) {
		return {};
	}

	if (is_track_image) {
		return LoadFloppyTrackDisk(file.get(), path);
	}

	const auto size_kb = stdio_size_kb(file.get());
	if (size_kb < 0 || size_kb > std::numeric_limits<uint32_t>::max()) {
		return {};
	}

	const auto is_hard_disk = hard_disk.value_or(size_kb > 2880);
	const auto disk = std::make_shared<imageDisk>(file.get(),
	                                              path.c_str(),
	                                              check_cast<uint32_t>(size_kb),
	                                              is_hard_disk,
	                                              read_only);
	file.release();
	return disk;
}

bool IsFloppyTrackImagePath(const std::string_view path)
{
	return track_format(path) != TrackFormat::None;
}

std::shared_ptr<imageDisk> LoadFloppyTrackDisk(FILE* file, const std::string& path)
{
	std::string error                       = {};
	std::unique_ptr<FloppyTrackImage> image = {};
	switch (track_format(path)) {
	case TrackFormat::HxcMfm:
		image = FloppyTrackImage::LoadHxcMfm(file);
		break;
	case TrackFormat::Scp:
		image = FloppyTrackImage::LoadScp(file, error);
		break;
	case TrackFormat::F86:
		image = FloppyTrackImage::Load86f(file, error);
		break;
	case TrackFormat::None: return {};
	}
	if (!image) {
		LOG_WARNING("Could not load floppy track image '%s': %s",
		            path.c_str(),
		            error.empty() ? "invalid image" : error.c_str());
		return {};
	}
	return std::make_shared<imageDisk>(std::move(image), path);
}
