// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bitstream_capture.h"
#include "ints/floppy_track_image.h"
#include "ints/scp_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

void write_le16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value)
{
	bytes[offset]     = static_cast<uint8_t>(value);
	bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

std::vector<uint8_t> stepped_capture()
{
	std::vector<uint8_t> bytes(0xb80);
	bytes[0]     = 0x5a;
	bytes[1]     = 0xa5;
	bytes[0x100] = 7;
	bytes[0x102] = 2;
	bytes[0x103] = 1;
	bytes[0x104] = 2;
	// Native tables address physical cylinders, with two head slots each.
	bytes[0x306] = 0x0a;
	bytes[0x30e] = 0x0b;
	write_le16(bytes, 0x505, 128);
	write_le16(bytes, 0x50d, 128);
	write_le16(bytes, 0x105, 0x1111);
	write_le16(bytes, 0x10d, 10000);
	std::fill_n(bytes.begin() + 0xa00, 128, 0x11);
	std::fill_n(bytes.begin() + 0xb00, 128, 0x77);
	return bytes;
}

TEST(BitstreamCapture, TransCopyPreservesPhysicalPlacementIndexAndUndecodedCells)
{
	const auto bytes   = stepped_capture();
	std::string error  = "old error";
	const auto capture = ParseTransCopyCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	EXPECT_TRUE(error.empty());
	EXPECT_EQ(capture->cylinders, 3u);
	EXPECT_EQ(capture->heads, 1u);
	ASSERT_EQ(capture->tracks.size(), 3u);
	EXPECT_EQ(capture->tracks[0].cells, std::vector<uint8_t>(128, 0x11));
	EXPECT_TRUE(capture->tracks[1].cells.empty());
	EXPECT_EQ(capture->tracks[2].cells, std::vector<uint8_t>(128, 0x77));
	EXPECT_EQ(capture->tracks[0].index, 0u);
	EXPECT_EQ(capture->tracks[2].index, 768u);
	EXPECT_EQ(capture->tracks[2].bit_count, 1024u);
	EXPECT_DOUBLE_EQ(capture->tracks[2].cycle_ms, 2.048);
	EXPECT_TRUE(capture->tracks[2].detect_weak);
}

std::vector<uint8_t> timed_transcopy_capture()
{
	std::vector<uint8_t> bytes(0x4100);
	bytes[0]     = 0x5a;
	bytes[1]     = 0xa5;
	bytes[0x100] = 7;
	bytes[0x103] = 1;
	bytes[0x104] = 1;
	bytes[0x306] = 0x40;
	write_le16(bytes, 0x505, 256);
	const std::array<uint16_t, 4> intervals = {4000, 8000, 12000, 16000};
	for (size_t i = 0; i < intervals.size(); ++i) {
		write_le16(bytes, 0x905 + 2 * i, intervals[i]);
	}
	for (const size_t offset : {8u, 32u, 64u}) {
		const auto iam = offset == 8;
		for (size_t i = 0; i < 3; ++i) {
			bytes[0x4000 + offset + 2 * i]     = iam ? 0x52 : 0x44;
			bytes[0x4000 + offset + 2 * i + 1] = iam ? 0x24 : 0x89;
		}
		const auto value = iam ? 0xfc : 0xfe;
		uint16_t word    = 0;
		for (auto bit = 7; bit >= 0; --bit) {
			word = static_cast<uint16_t>((word << 2) |
			                             ((value >> bit) & 1));
		}
		bytes[0x4000 + offset + 6] = static_cast<uint8_t>(word >> 8);
		bytes[0x4000 + offset + 7] = static_cast<uint8_t>(word);
	}
	return bytes;
}

TEST(BitstreamCapture, TransCopyAddressMarkTimingIncludesIamAndSkew)
{
	for (const auto skew : {uint16_t{0}, uint16_t{10000}}) {
		auto bytes = timed_transcopy_capture();
		write_le16(bytes, 0x105, skew);
		write_le16(bytes, 0x905, static_cast<uint16_t>(4000 + skew));
		std::string error  = {};
		const auto capture = ParseTransCopyCapture(bytes, error);
		ASSERT_TRUE(capture) << error;
		const auto& track = capture->tracks[0];
		ASSERT_EQ(track.address_marks.size(), 3u);
		for (size_t i = 0; i < 3; ++i) {
			EXPECT_EQ(track.address_marks[i].bit,
			          (std::array<size_t, 3>{64, 256, 512}[i]));
			EXPECT_DOUBLE_EQ(track.address_marks[i].cycle_fraction,
			                 (std::array<double, 3>{0.1, 0.3, 0.6}[i] +
			                  skew / 40000.0));
		}
		EXPECT_EQ(track.cells,
		          std::vector<uint8_t>(bytes.begin() + 0x4000, bytes.end()));
	}
}

TEST(BitstreamCapture, TransCopyInvalidOptionalTimingRetainsRawFallback)
{
	for (const auto damage : {0, 1, 2, 3, 4}) {
		auto bytes = timed_transcopy_capture();
		if (damage == 0) {
			std::fill(bytes.begin() + 0x905, bytes.begin() + 0x925, 0);
		}
		if (damage == 1) {
			write_le16(bytes, 0x907, 0);
		}
		if (damage == 2) {
			write_le16(bytes, 0x90b, 1);
		}
		if (damage == 3) {
			write_le16(bytes, 0x90d, 1);
		}
		if (damage == 4) {
			bytes[0x4000 + 32] = 0;
		}
		std::string error  = {};
		const auto capture = ParseTransCopyCapture(bytes, error);
		ASSERT_TRUE(capture) << error;
		EXPECT_TRUE(capture->tracks[0].address_marks.empty()) << damage;
		EXPECT_EQ(capture->tracks[0].cells,
		          std::vector<uint8_t>(bytes.begin() + 0x4000, bytes.end()));
	}
}

TEST(BitstreamCapture, TransCopyRejectsTruncatedAndInvalidTrackTables)
{
	const auto original = stepped_capture();
	std::string error   = {};
	for (const size_t length : {0u, 1u, 0x100u, 0x104u, 0x904u, 0xa00u, 0xb7fu}) {
		EXPECT_FALSE(ParseTransCopyCapture(std::span(original).first(length),
		                                   error));
		EXPECT_FALSE(error.empty());
	}
	const std::array<std::pair<size_t, uint8_t>, 7> damage = {
	        std::pair<size_t, uint8_t>{0x100,    0},
	        {0x103,    0},
	        {0x104,    0},
	        {0x306,    1},
	        {0x30e, 0x0a},
	        {0x30a, 0x0a},
	        {0x505,    0},
	};
	for (const auto& [offset, value] : damage) {
		auto bytes    = original;
		bytes[offset] = value;
		EXPECT_FALSE(ParseTransCopyCapture(bytes, error)) << offset;
		EXPECT_FALSE(error.empty());
	}
	const auto valid = ParseTransCopyCapture(original, error);
	EXPECT_TRUE(valid);
	EXPECT_TRUE(error.empty());
}

TEST(BitstreamCapture, TransCopyPcTypesPreserveCellsAndSelectClock)
{
	constexpr std::array<std::pair<uint8_t, double>, 6> cases = {
	        std::pair<uint8_t, double>{0x02, 1.024},
	        {0x03, 1.024},
	        {0x05, 4.096},
	        {0x07, 2.048},
	        {0x0c, 4.096},
	        {0xff, 2.048},
	};
	for (const auto& [type, cycle_ms] : cases) {
		auto bytes         = stepped_capture();
		bytes[0x100]       = type;
		std::string error  = {};
		const auto capture = ParseTransCopyCapture(bytes, error);
		ASSERT_TRUE(capture) << error;
		EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, cycle_ms);
		EXPECT_EQ(capture->tracks[0].cells, std::vector<uint8_t>(128, 0x11));
		EXPECT_EQ(capture->tracks[2].index, size_t{768});
	}
	for (const uint8_t type : {0x00, 0x01, 0x04, 0x06, 0x08, 0x09, 0xfe}) {
		auto bytes        = stepped_capture();
		bytes[0x100]      = type;
		std::string error = {};
		EXPECT_FALSE(ParseTransCopyCapture(bytes, error));
		EXPECT_FALSE(error.empty());
	}
}

TEST(BitstreamCapture, TransCopyTrackTypesOverrideDiskType)
{
	auto bytes   = stepped_capture();
	bytes[0x100] = 5;
	write_le16(bytes, 0x705, 0x0709);
	write_le16(bytes, 0x70d, 0x8589);
	std::string error = {};
	auto capture      = ParseTransCopyCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, 2.048);
	EXPECT_DOUBLE_EQ(capture->tracks[2].cycle_ms, 4.096);
	// PCE-style files can omit track types; unused table entries have
	// a separate sentinel rather than a defined encoding.
	for (const uint16_t flags : {0x0000, 0x7f09, 0xff09, 0x4444}) {
		write_le16(bytes, 0x705, flags);
		capture = ParseTransCopyCapture(bytes, error);
		ASSERT_TRUE(capture) << error;
		EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, 4.096);
	}
	write_le16(bytes, 0x705, 0x0409);
	EXPECT_FALSE(ParseTransCopyCapture(bytes, error));
	EXPECT_EQ(error, "unsupported TransCopy track encoding");
}

void write_le32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) {
		bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

std::vector<uint8_t> hxc_capture()
{
	std::vector<uint8_t> bytes(46, 0x77);
	std::fill_n(bytes.begin(), 30, 0);
	constexpr std::array<uint8_t, 7> signature = {'H', 'X', 'C', 'M', 'F', 'M', 0};
	std::copy(signature.begin(), signature.end(), bytes.begin());
	write_le16(bytes, 7, 1);
	bytes[9] = 1;
	write_le16(bytes, 10, 300);
	write_le16(bytes, 12, 250);
	write_le32(bytes, 15, 19);
	write_le32(bytes, 22, 16);
	write_le32(bytes, 26, 30);
	return bytes;
}

TEST(BitstreamCapture, HxcPreservesBitcellsAndRejectsMalformedTables)
{
	const auto bytes   = hxc_capture();
	std::string error  = {};
	const auto capture = ParseHxcMfmCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	ASSERT_EQ(capture->tracks.size(), 1u);
	EXPECT_EQ(capture->tracks[0].cells, std::vector<uint8_t>(16, 0x77));
	EXPECT_EQ(capture->tracks[0].bit_count, 128u);
	EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, 200.0);
	for (size_t size = 0; size < bytes.size(); ++size) {
		EXPECT_FALSE(ParseHxcMfmCapture(std::span(bytes).first(size), error))
		        << size;
	}
	for (const size_t offset : {7u, 9u, 12u, 15u, 22u, 26u}) {
		auto damaged    = bytes;
		damaged[offset] = offset == 12 ? 0 : 0xff;
		EXPECT_FALSE(ParseHxcMfmCapture(damaged, error)) << offset;
		EXPECT_FALSE(error.empty());
	}
}

std::vector<uint8_t> f86_capture()
{
	std::vector<uint8_t> bytes(0x422);
	constexpr std::array<uint8_t, 4> signature = {'8', '6', 'B', 'F'};
	std::copy(signature.begin(), signature.end(), bytes.begin());
	write_le16(bytes, 4, 0x020c);
	write_le16(bytes, 6, 0x1080);
	write_le32(bytes, 8, 0x408);
	write_le16(bytes, 0x408, 0x0a);
	write_le32(bytes, 0x40a, 128);
	write_le32(bytes, 0x40e, 32);
	std::fill(bytes.begin() + 0x412, bytes.end(), 0x55);
	return bytes;
}

TEST(BitstreamCapture, F86FmUsesHalfTheMfmDataRate)
{
	auto bytes = f86_capture();
	write_le16(bytes, 0x408, 2);
	std::string error  = {};
	const auto capture = Parse86fCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	ASSERT_EQ(capture->tracks.size(), size_t{1});
	EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, 0.512);
	EXPECT_EQ(capture->tracks[0].bit_count, size_t{128});
	EXPECT_EQ(capture->tracks[0].index, size_t{32});
	for (const uint16_t encoding : {uint16_t{0x12}, uint16_t{0x1a}}) {
		write_le16(bytes, 0x408, encoding);
		EXPECT_FALSE(Parse86fCapture(bytes, error));
	}
}

TEST(BitstreamCapture, F86PreservesBitCountIndexAndSurfaceWriteProtection)
{
	auto bytes        = f86_capture();
	std::string error = {};
	auto capture      = Parse86fCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	ASSERT_EQ(capture->tracks.size(), 1u);
	EXPECT_EQ(capture->tracks[0].cells, std::vector<uint8_t>(16, 0x55));
	EXPECT_EQ(capture->tracks[0].bit_count, 128u);
	EXPECT_EQ(capture->tracks[0].index, 32u);
	EXPECT_DOUBLE_EQ(capture->tracks[0].cycle_ms, 0.256);
	EXPECT_FALSE(capture->read_only);
	// Surface data containing marked zero bits must keep the capture
	// read-only.
	write_le16(bytes, 6, 0x1081);
	bytes.resize(bytes.size() + 16, 0xff);
	capture = Parse86fCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	EXPECT_TRUE(capture->read_only);
}

TEST(BitstreamCapture, F86PreservesExplicitWeakAndHoleCells)
{
	auto bytes = f86_capture();
	write_le16(bytes, 6, 0x1081);
	bytes.resize(bytes.size() + 16, 0x55);
	std::string error = {};
	for (const auto protect : {false, true}) {
		write_le16(bytes, 6, protect ? 0x1091 : 0x1081);
		const auto capture = Parse86fCapture(bytes, error);
		ASSERT_TRUE(capture) << error;
		EXPECT_EQ(capture->read_only, protect);
		const auto& weak = capture->tracks[0].weak_cells;
		ASSERT_EQ(weak.size(), 128u);
		for (size_t bit = 0; bit < weak.size(); ++bit) {
			EXPECT_EQ(weak[bit], (bit & 1) != 0);
		}
	}
	// A marked zero is also noisy, but cannot be overwritten.
	bytes[0x422] |= 0x80;
	write_le16(bytes, 6, 0x1081);
	const auto capture = Parse86fCapture(bytes, error);
	ASSERT_TRUE(capture) << error;
	EXPECT_TRUE(capture->read_only);
	EXPECT_TRUE(capture->tracks[0].weak_cells[0]);
}

TEST(BitstreamCapture, F86RejectsTruncatedInvalidOffsetsAndIndex)
{
	const auto original = f86_capture();
	std::string error   = {};
	for (const size_t size : {0u, 7u, 0x407u, 0x411u, 0x421u}) {
		EXPECT_FALSE(Parse86fCapture(std::span(original).first(size), error));
	}
	for (const size_t offset : {4u, 7u, 8u, 0x408u, 0x40au, 0x40eu}) {
		auto bytes    = original;
		bytes[offset] = 0xff;
		EXPECT_FALSE(Parse86fCapture(bytes, error)) << offset;
		EXPECT_FALSE(error.empty());
	}
}

std::vector<uint8_t> scp_capture()
{
	std::vector<uint8_t> bytes(0x2c4);
	bytes[0] = 'S';
	bytes[1] = 'C';
	bytes[2] = 'P';
	bytes[3] = 0x10;
	bytes[4] = 0x31;
	bytes[5] = 1;
	bytes[8] = 0x11;
	write_le32(bytes, 16, 0x2b0);
	bytes[0x2b0] = 'T';
	bytes[0x2b1] = 'R';
	bytes[0x2b2] = 'K';
	write_le32(bytes, 0x2b4, 8000000);
	write_le32(bytes, 0x2b8, 2);
	write_le32(bytes, 0x2bc, 16);
	bytes[0x2c3] = 80;
	return bytes;
}

bool load_scp(ScpCapture& capture, const std::vector<uint8_t>& bytes,
              std::string& error)
{
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(tmpfile(), close_file);
	if (!file ||
	    fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
		return false;
	}
	return capture.Load(file.get(), error);
}

TEST(BitstreamCapture, ScpPreservesOverflowEntriesAndRejectsDamagedExtentsAtomically)
{
	const auto bytes   = scp_capture();
	ScpCapture capture = {};
	std::string error  = {};
	ASSERT_TRUE(load_scp(capture, bytes, error)) << error;
	ASSERT_TRUE(capture.tracks[0]);
	EXPECT_EQ(capture.tracks[0]->revolutions[0].entries,
	          (std::vector<uint32_t>{0, 80}));
	EXPECT_EQ(capture.tracks[0]->revolutions[0].IndexDurationNs(), 200000000u);
	const auto fingerprint = capture.fingerprint;
	for (const size_t offset : {5u, 16u, 0x2b3u, 0x2b8u, 0x2bcu}) {
		auto damaged    = bytes;
		damaged[offset] = 0xff;
		EXPECT_FALSE(load_scp(capture, damaged, error)) << offset;
		EXPECT_FALSE(error.empty());
		EXPECT_EQ(capture.fingerprint, fingerprint);
	}
	// A read-only capture requires a valid checksum.
	auto damaged = bytes;
	damaged[8]   = 1;
	EXPECT_FALSE(load_scp(capture, damaged, error));
}

std::vector<uint8_t> overlapping_capture(const uint8_t size_code,
                                         const int records        = 768,
                                         const uint16_t cylinders = 1)
{
	std::vector<uint8_t> cells = {};
	bool previous              = false;
	const auto append          = [&](const uint8_t byte) {
                uint16_t word = 0;
                for (auto bit = 7; bit >= 0; --bit) {
                        const auto data = ((byte >> bit) & 1) != 0;
                        word            = static_cast<uint16_t>(
                                (word << 2) | ((!previous && !data) ? 2 : 0) | data);
                        previous = data;
                }
                cells.push_back(static_cast<uint8_t>(word >> 8));
                cells.push_back(static_cast<uint8_t>(word));
	};
	const auto field = [&](const std::span<const uint8_t> bytes) {
		for (auto i = 0; i < 12; ++i) {
			append(0);
		}
		cells.insert(cells.end(), {0x44, 0x89, 0x44, 0x89, 0x44, 0x89});
		previous = true;
		auto crc = uint16_t{0xcdb4};
		for (const auto byte : bytes) {
			append(byte);
			crc ^= static_cast<uint16_t>(byte << 8);
			for (auto bit = 0; bit < 8; ++bit) {
				crc = static_cast<uint16_t>(
				        (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
			}
		}
		append(static_cast<uint8_t>(crc >> 8));
		append(static_cast<uint8_t>(crc));
	};
	const std::array<uint8_t, 5> id   = {0xfe, 0, 0, 1, size_code};
	const std::array<uint8_t, 1> data = {0xfb};
	for (auto i = 0; i < records; ++i) {
		field(id);
		field(data);
	}
	const auto data_start = 19 + size_t{cylinders} * 11;
	std::vector<uint8_t> image(data_start + size_t{cylinders} * cells.size());
	constexpr std::array<uint8_t, 7> signature = {'H', 'X', 'C', 'M', 'F', 'M', 0};
	std::ranges::copy(signature, image.begin());
	write_le16(image, 7, cylinders);
	image[9] = 1;
	write_le16(image, 10, 300);
	write_le16(image, 12, 1000);
	write_le32(image, 15, 19);
	for (uint16_t cylinder = 0; cylinder < cylinders; ++cylinder) {
		const auto entry = 19 + size_t{cylinder} * 11;
		write_le16(image, entry, cylinder);
		write_le32(image, entry + 3, static_cast<uint32_t>(cells.size()));
		const auto offset = data_start + size_t{cylinder} * cells.size();
		write_le32(image, entry + 7, static_cast<uint32_t>(offset));
		std::ranges::copy(cells,
		                  std::span(image).subspan(offset, cells.size()).begin());
	}
	return image;
}

TEST(BitstreamCapture, BoundDecodedDataWithoutRejectingOverlappingIds)
{
	for (const auto size_code : {uint8_t{2}, uint8_t{7}}) {
		const auto bytes = overlapping_capture(size_code);
		ASSERT_LT(bytes.size(), 64 * 1024u);
		const auto close_file = [](FILE* file) { fclose(file); };
		std::unique_ptr<FILE, decltype(close_file)> file(tmpfile(),
		                                                 close_file);
		ASSERT_TRUE(file);
		ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), file.get()),
		          bytes.size());
		std::string error = {};
		const auto image  = FloppyTrackImage::LoadHxcMfm(file.get(),
                                                                "decoded-budget",
                                                                true,
                                                                error);
		if (size_code == 2) {
			ASSERT_TRUE(image) << error;
			EXPECT_EQ(image->GetTrack(0, 0).size(), 768u);
		} else {
			EXPECT_FALSE(image);
			EXPECT_NE(error.find("resource limits"), std::string::npos);
		}
	}
}

TEST(BitstreamCapture, BoundAggregateDecodedImageData)
{
	// Every track is below the per-track limit, but 33 * 4 MB exceeds
	// the image budget. The container itself occupies less than 1 MB.
	const auto bytes = overlapping_capture(7, 256, 33);
	ASSERT_LT(bytes.size(), 1024 * 1024u);
	const auto close_file = [](FILE* file) { fclose(file); };
	std::unique_ptr<FILE, decltype(close_file)> file(tmpfile(), close_file);
	ASSERT_TRUE(file);
	ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), file.get()), bytes.size());
	std::string error = {};
	EXPECT_FALSE(
	        FloppyTrackImage::LoadHxcMfm(file.get(), "decoded-budget", true, error));
	EXPECT_NE(error.find("resource limits"), std::string::npos);
}

} // namespace
