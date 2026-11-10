// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/disk_image_factory.h"

#include <cstdio>
#include <limits>

#include "ints/bios_disk.h"
#include "misc/cross.h"
#include "misc/support.h"
#include "utils/checks.h"

CHECK_NARROWING();

std::shared_ptr<imageDisk> OpenSectorDiskImage(const std::string& path,
                                               bool& read_only,
                                               const std::optional<bool> hard_disk)
{
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(
	        fopen_wrap_ro_fallback(path, read_only), close_file);
	if (!file) {
		return {};
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
