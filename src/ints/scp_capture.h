// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef DOSBOX_SCP_CAPTURE_H
#define DOSBOX_SCP_CAPTURE_H
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// SCP floppy container reader. This class does not assume a disk encoding.
// Original entries, revolution boundaries and header flags remain available;
// IBM MFM sector decoding is a separate consumer in the floppy track loader.
class ScpCapture {
public:
	struct Header {
		uint8_t creator_version = 0, disk_type = 0, revolutions = 0;
		uint8_t start_track = 0, end_track = 0, flags = 0, cell_width = 16;
		uint8_t head_selector = 0, resolution = 0;
		bool Indexed() const
		{
			return (flags & 1) != 0;
		}
		bool WritableSource() const
		{
			return (flags & 0x10) != 0;
		}
		bool Normalized() const
		{
			return (flags & 8) != 0;
		}
		bool ThirdParty() const
		{
			return (flags & 0x80) != 0;
		}
		unsigned Rpm() const
		{
			return flags & 4 ? 360 : 300;
		}
		unsigned Tpi() const
		{
			return flags & 2 ? 96 : 48;
		}
		uint64_t FluxUnitNs() const
		{
			return (uint64_t(resolution) + 1) * 25;
		}
	};
	struct Revolution {
		uint32_t index_ticks_25ns     = 0;
		std::vector<uint32_t> entries = {}; // includes zero overflow
		                                    // entries
		uint64_t IndexDurationNs() const
		{
			return uint64_t(index_ticks_25ns) * 25;
		}
	};
	struct Track {
		uint8_t number                      = 0;
		std::vector<Revolution> revolutions = {};
	};
	struct Footer {
		// Manufacturer, model, serial, creator, application, comments.
		std::array<std::string, 6> strings = {};
		int64_t created_utc = 0, modified_utc = 0;
		uint8_t application_version = 0, hardware_version = 0,
		        firmware_version = 0, format_revision = 0;
	};
	bool Load(FILE* file, std::string& error);
	Header header                                = {};
	uint64_t fingerprint                         = 0;
	std::array<std::optional<Track>, 168> tracks = {};
	std::optional<Footer> footer                 = {};
	std::string legacy_timestamp                 = {};

private:
	bool Parse(const std::vector<uint8_t>& bytes, std::string& error);
};
#endif
