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

std::shared_ptr<imageDisk> OpenSectorDiskImage(const std::string& path,
                                               bool& read_only,
                                               const std::optional<bool> hard_disk)
{
	const auto is_mfm = IsHxcMfmPath(path);
	if (is_mfm && hard_disk.value_or(false)) {
		return {};
	}
	read_only |= is_mfm;
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(
	        fopen_wrap_ro_fallback(path, read_only), close_file);
	if (!file) {
		return {};
	}

	if (is_mfm) {
		return LoadHxcMfmDisk(file.get(), path);
	}

	const auto size_kb = stdio_size_kb(file.get());
	if (size_kb < 0 || size_kb > std::numeric_limits<uint32_t>::max()) {
		return {};
	}

	const auto is_hard_disk = hard_disk.value_or(size_kb > 2880);
	const auto disk = std::make_shared<imageDisk>(file.get(),
	                                              path.c_str(),
	                                              check_cast<uint32_t>(size_kb),
	                                              is_hard_disk);
	file.release();
	return disk;
}

bool IsHxcMfmPath(const std::string_view path)
{
	return path.size() >= 4 && iequals(path.substr(path.size() - 4), ".mfm");
}

std::shared_ptr<imageDisk> LoadHxcMfmDisk(FILE* file, const std::string& path)
{
	auto image = HxcMfmImage::Load(file);
	if (!image) {
		LOG_WARNING("Invalid HxC MFM image '%s'", path.c_str());
		return {};
	}
	return std::make_shared<imageDisk>(std::move(image), path);
}
