// SPDX-License-Identifier: GPL-2.0-or-later
#include "ints/scp_capture.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <string_view>
#include <utility>

#include "ints/floppy_media.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace {
constexpr size_t TableEnd     = 0x2b0;
constexpr size_t MaxFileBytes = 256 * 1024 * 1024;
// Leave room for full 80-cylinder, five-revolution PC captures.
constexpr size_t MaxStoredEntries = 36 * 1024 * 1024;
bool fits(const size_t offset, const size_t length, const size_t end)
{
	return offset <= end && length <= end - offset;
}
std::string_view field_text(const std::vector<uint8_t>& bytes,
                            const size_t offset, const size_t length)
{
	return {reinterpret_cast<const char*>(bytes.data() + offset), length};
}
uint32_t little(const std::vector<uint8_t>& d, size_t p, unsigned n = 4)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; ++i) {
		v |= uint32_t(d[p + i]) << (8 * i);
	}
	return v;
}
int64_t signed64(const std::vector<uint8_t>& d, size_t p)
{
	const uint64_t v = uint64_t(little(d, p)) |
	                   (uint64_t(little(d, p + 4)) << 32);
	return std::bit_cast<int64_t>(v);
}
bool utf8(const std::string& s)
{
	for (size_t i = 0; i < s.size();) {
		const auto a = uint8_t(s[i++]);
		if (a == 0) {
			return false;
		}
		if (a < 0x80) {
			continue;
		}
		unsigned n  = 0;
		uint32_t cp = 0, min = 0;
		if (a >= 0xc2 && a <= 0xdf) {
			n   = 1;
			cp  = a & 31;
			min = 0x80;
		} else if (a >= 0xe0 && a <= 0xef) {
			n   = 2;
			cp  = a & 15;
			min = 0x800;
		} else if (a >= 0xf0 && a <= 0xf4) {
			n   = 3;
			cp  = a & 7;
			min = 0x10000;
		} else {
			return false;
		}
		if (n > s.size() - i) {
			return false;
		}
		while (n--) {
			const auto b = uint8_t(s[i++]);
			if ((b & 0xc0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (b & 63);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
			return false;
		}
	}
	return true;
}
} // namespace

bool ScpCapture::Load(FILE* file, std::string& error)
{
	error.clear();
	if (fseek(file, 0, SEEK_END) != 0) {
		error = "cannot seek SCP capture";
		return false;
	}
	const auto size = ftell(file);
	if (size < static_cast<long>(TableEnd) ||
	    static_cast<uint64_t>(size) > MaxFileBytes ||
	    fseek(file, 0, SEEK_SET) != 0) {
		error = "SCP capture outside supported file-size limits";
		return false;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		error = "cannot read SCP capture";
		return false;
	}
	ScpCapture parsed = {};
	if (!parsed.Parse(bytes, error)) {
		return false;
	}
	*this = std::move(parsed);
	return true;
}

bool ScpCapture::Parse(const std::vector<uint8_t>& d, std::string& error)
{
	const auto fail = [&](const char* message) {
		error = message;
		return false;
	};
	if (field_text(d, 0, 3) != "SCP") {
		return fail("invalid SCP signature");
	}
	header = {d[3], d[4], d[5], d[6], d[7], d[8], uint8_t(d[9] ? d[9] : 16), d[10], d[11]};
	if (!header.revolutions || header.start_track > header.end_track ||
	    header.end_track >= 168 || header.head_selector > 2) {
		return fail("invalid SCP floppy header");
	}
	if ((header.flags & 0x40) || (header.disk_type >> 4) >= 0xe) {
		return fail("extended SCP tape/hard-drive containers are not supported");
	}
	if (header.cell_width != 8 && header.cell_width != 16 &&
	    header.cell_width != 24 && header.cell_width != 32) {
		return fail("unsupported SCP entry width (supported: 8, 16, 24, 32 bits)");
	}
	if (header.WritableSource() && header.revolutions != 1) {
		return fail("writable SCP captures must have one revolution");
	}
	uint32_t sum = 0;
	for (const auto byte : std::span(d).subspan(16)) {
		sum += byte;
	}
	const auto checksum = little(d, 12);
	if (!(header.WritableSource() && checksum == 0) && sum != checksum) {
		return fail("SCP checksum mismatch");
	}
	fingerprint           = FloppyImageFingerprint(d);
	const bool has_footer = (header.flags & 0x20) != 0;
	const size_t data_end = has_footer ? d.size() - 48 : d.size();
	if (has_footer && (header.creator_version != 0 || data_end < TableEnd ||
	                   field_text(d, d.size() - 4, 4) != "FPCS")) {
		return fail("invalid SCP extension footer");
	}
	size_t last_track_end = TableEnd, total_entries = 0;
	for (size_t number = 0; number < 168; ++number) {
		const size_t off = little(d, 16 + number * 4);
		if (!off) {
			continue;
		}
		if (number < header.start_track || number > header.end_track) {
			return fail("SCP track exists outside declared range");
		}
		if (off < TableEnd) {
			return fail("SCP track overlaps offset table");
		}
		const size_t header_bytes = 4 + size_t(header.revolutions) * 12;
		if (!fits(off, header_bytes, data_end)) {
			return fail("SCP track header extends outside data area");
		}
		if (field_text(d, off, 3) != "TRK" || d[off + 3] != number) {
			return fail("invalid SCP track header");
		}
		last_track_end = std::max(last_track_end, off + header_bytes);
		Track track    = {};
		track.number   = uint8_t(number);
		for (size_t r = 0; r < header.revolutions; ++r) {
			Revolution rev       = {};
			rev.index_ticks_25ns = little(d, off + 4 + r * 12);
			if (header.Indexed() && !rev.index_ticks_25ns) {
				return fail("indexed SCP revolution has zero duration");
			}
			const size_t count    = little(d, off + 8 + r * 12),
			             relative = little(d, off + 12 + r * 12);
			const size_t stride   = header.cell_width / 8;
			if (relative < header_bytes || relative > data_end - off) {
				return fail("invalid SCP revolution offset");
			}
			const size_t p = off + relative;
			if (count > (data_end - p) / stride) {
				return fail("SCP flux extends outside data area");
			}
			if (count > MaxStoredEntries - total_entries) {
				return fail("SCP capture exceeds the 36 Mi-entry resource limit");
			}
			total_entries += count;
			rev.entries.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				uint32_t v = 0;
				for (size_t b = 0; b < stride; ++b) {
					v = (v << 8) | d[p + i * stride + b];
				}
				rev.entries.push_back(v);
			}
			last_track_end = std::max(last_track_end, p + count * stride);
			track.revolutions.push_back(std::move(rev));
		}
		tracks[number] = std::move(track);
	}
	if (has_footer) {
		Footer f = {};
		for (size_t i = 0; i < 6; ++i) {
			const size_t p = little(d, data_end + i * 4);
			if (!p) {
				continue;
			}
			if (p < last_track_end) {
				return fail("SCP footer string overlaps track data");
			}
			if (!fits(p, 2, data_end)) {
				return fail("SCP footer string header extends outside data area");
			}
			const size_t length = little(d, p, 2);
			if (!fits(p + 2, length + 1, data_end)) {
				return fail("SCP footer string extends outside data area");
			}
			if (d[p + 2 + length]) {
				return fail("unterminated SCP footer string");
			}
			f.strings[i].assign(reinterpret_cast<const char*>(
			                            d.data() + p + 2),
			                    length);
			if (!utf8(f.strings[i])) {
				return fail("invalid UTF-8 in SCP footer");
			}
		}
		f.created_utc         = signed64(d, data_end + 24);
		f.modified_utc        = signed64(d, data_end + 32);
		f.application_version = d[data_end + 40];
		f.hardware_version    = d[data_end + 41];
		f.firmware_version    = d[data_end + 42];
		f.format_revision     = d[data_end + 43];
		footer                = std::move(f);
	} else {
		for (size_t p = last_track_end;
		     p < d.size() && d[p] >= 0x20 && d[p] <= 0x7e;
		     ++p) {
			legacy_timestamp += char(d[p]);
		}
	}
	return true;
}
