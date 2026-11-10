// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DISK_IMAGE_FACTORY_H
#define DOSBOX_DISK_IMAGE_FACTORY_H

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class imageDisk;

// If the disk type is unspecified, use the existing FAT image size heuristic.
std::shared_ptr<imageDisk> OpenSectorDiskImage(
        const std::string& path, bool& read_only,
        const std::optional<bool> hard_disk = std::nullopt);

bool IsHxcMfmPath(const std::string_view path);
std::shared_ptr<imageDisk> LoadHxcMfmDisk(FILE* file, const std::string& path);

#endif
