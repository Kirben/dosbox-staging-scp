// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/disk_image_factory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>

#include "ints/bios_disk.h"
#include "ints/floppy_track_image.h"
#include "misc/cross.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

namespace {

using TrackLoader = std::unique_ptr<FloppyTrackImage> (*)(FILE*, const std::string&,
                                                          bool, std::string&);

struct TrackProbe {
	std::string_view extension         = {};
	std::span<const uint8_t> signature = {};
	TrackLoader load                   = nullptr;
};

constexpr std::array<uint8_t, 7> HxcSignature = {'H', 'X', 'C', 'M', 'F', 'M', 0};
constexpr std::array<uint8_t, 3> ScpSignature       = {'S', 'C', 'P'};
constexpr std::array<uint8_t, 4> F86Signature       = {'8', '6', 'B', 'F'};
constexpr std::array<uint8_t, 2> TransCopySignature = {0x5a, 0xa5};
constexpr std::array TrackProbes                    = {
        TrackProbe{".mfm",       HxcSignature,    FloppyTrackImage::LoadHxcMfm},
        TrackProbe{".scp",       ScpSignature,       FloppyTrackImage::LoadScp},
        TrackProbe{".86f",       F86Signature,       FloppyTrackImage::Load86f},
        TrackProbe{ ".tc", TransCopySignature, FloppyTrackImage::LoadTransCopy},
};

const TrackProbe* extension_probe(const std::string_view path)
{
	const auto dot = path.rfind('.');
	if (dot == std::string_view::npos) {
		return nullptr;
	}
	for (const auto& probe : TrackProbes) {
		if (iequals(path.substr(dot), probe.extension)) {
			return &probe;
		}
	}
	return nullptr;
}

const TrackProbe* track_probe(FILE* file, const std::string_view path)
{
	const auto position = ftell(file);
	if (position < 0 || fseek(file, 0, SEEK_SET)) {
		return nullptr;
	}
	std::array<uint8_t, HxcSignature.size()> prefix = {};
	const auto size = fread(prefix.data(), 1, prefix.size(), file);
	if (fseek(file, position, SEEK_SET)) {
		return nullptr;
	}
	for (const auto& probe : TrackProbes) {
		if (size >= probe.signature.size() &&
		    std::ranges::equal(probe.signature,
		                       std::span(prefix).first(
		                               probe.signature.size()))) {
			return &probe;
		}
	}
	// A known suffix must not turn a malformed container into a raw disk.
	return extension_probe(path);
}

} // namespace

std::shared_ptr<imageDisk> OpenSectorDiskImage(const std::string& path,
                                               bool& read_only,
                                               const std::optional<bool> hard_disk)
{
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(fopen(path.c_str(), "rb"),
	                                                 close_file);
	if (!file) {
		return {};
	}
	if (track_probe(file.get(), path)) {
		if (hard_disk.value_or(false)) {
			return {};
		}
		auto disk = LoadFloppyTrackDisk(file.get(), path, read_only);
		if (disk) {
			read_only = disk->IsWriteProtected();
		}
		return disk;
	}

	// Raw images retain direct writes; track containers only use overlays.
	file.reset();
	file.reset(fopen_wrap_ro_fallback(path, read_only));
	if (!file) {
		return {};
	}

	const auto size_kb = stdio_size_kb(file.get());
	if (size_kb < 0 || size_kb > std::numeric_limits<uint32_t>::max()) {
		return {};
	}

	const auto is_hard_disk = hard_disk.value_or(size_kb > 2880);
	const auto disk         = std::make_shared<imageDisk>(file.get(),
                                                      path.c_str(),
                                                      check_cast<uint32_t>(size_kb),
                                                      is_hard_disk,
                                                      read_only);
	file.release();
	return disk;
}

bool IsFloppyTrackImage(FILE* file, const std::string_view path)
{
	return track_probe(file, path) != nullptr;
}

bool IsFloppyTrackImagePath(const std::string_view path)
{
	const auto close_file = [](FILE* file) { fclose(file); };
	const std::string filename(path);
	const std::unique_ptr<FILE, decltype(close_file)> file(
	        fopen(filename.c_str(), "rb"), close_file);
	return file ? IsFloppyTrackImage(file.get(), path)
	            : extension_probe(path) != nullptr;
}

std::shared_ptr<imageDisk> LoadFloppyTrackDisk(FILE* file, const std::string& path,
                                               const bool read_only)
{
	const auto* probe = track_probe(file, path);
	if (!probe) {
		return {};
	}
	std::string error = {};
	auto image        = probe->load(file, path, read_only, error);
	if (!image) {
		LOG_WARNING("Could not load floppy track image '%s': %s",
		            path.c_str(),
		            error.empty() ? "invalid image" : error.c_str());
		return {};
	}
	const auto heads     = image->heads;
	const auto cylinders = image->cylinders;
	const auto sectors   = image->logical_sectors;
	return std::make_shared<imageDisk>(std::move(image), path, heads, cylinders, sectors);
}
