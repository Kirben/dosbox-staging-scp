// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bios_disk.h"
#include "ints/floppy_media.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "utils/checks.h"

CHECK_NARROWING();

namespace {

std::unique_ptr<imageDisk> make_raw_disk(const uint32_t sector_size,
                                         const size_t recorded_size,
                                         const bool read_only = false)
{
	auto* file = std::tmpfile();
	if (!file) {
		return {};
	}
	auto disk = std::make_unique<imageDisk>(file, "sector-test", 0, true, read_only);
	const auto data = std::vector<uint8_t>(recorded_size, 0x37);
	if (fwrite(data.data(), 1, data.size(), file) != data.size() ||
	    fflush(file) != 0) {
		return {};
	}
	rewind(file);
	disk->Set_Geometry(1, 1, 2, sector_size);
	return disk;
}

class TestFloppyMedia final : public FloppyMedia {
public:
	std::vector<FloppySector> sectors = {};
	bool protected_media              = false;
	bool fail_write                   = false;
	std::vector<uint8_t> written      = {};

	double GetCycleMs(const uint32_t, const uint32_t) const override
	{
		return 200;
	}
	double GetFormatCycleMs(const uint32_t, const uint32_t) const override
	{
		return 200;
	}
	std::span<const double> GetRevolutionEnds(uint32_t, uint32_t) const override
	{
		return {};
	}
	std::span<const FloppySector> GetTrack(const uint32_t, const uint32_t) const override
	{
		return sectors;
	}
	bool SupportsTrackWrites() const override
	{
		return true;
	}
	bool IsWriteProtected() const override
	{
		return protected_media;
	}
	bool WriteSector(const uint32_t, const uint32_t, const uint32_t,
	                 const std::span<const uint8_t> data,
	                 const FloppySector*) override
	{
		if (fail_write) {
			return false;
		}
		written.assign(data.begin(), data.end());
		return true;
	}
	FormatResult FormatTrack(const uint8_t, const uint8_t,
	                         const std::span<const uint8_t>, const uint8_t,
	                         const uint8_t, const uint8_t, const uint16_t) override
	{
		return FormatResult::InvalidLayout;
	}
};

} // namespace

TEST(SectorMedia, RawSizedBuffersAndFullTransfers)
{
	static_assert(static_cast<int>(MediaResult::Ok) == 0);
	for (const auto size : {256u, 512u, 1024u}) {
		auto disk = make_raw_disk(size, size * 2);
		ASSERT_TRUE(disk);
		SectorMedia& media = *disk;
		auto guarded       = std::vector<uint8_t>(size + 2, 0xa5);
		auto buffer        = std::span(guarded).subspan(1, size);
		EXPECT_EQ(media.Read_AbsoluteSector(0, buffer.first(size - 1)),
		          MediaResult::InvalidBuffer);
		EXPECT_TRUE(std::ranges::all_of(guarded, [](const auto b) {
			return b == 0xa5;
		}));
		EXPECT_EQ(media.Read_AbsoluteSector(0, guarded),
		          MediaResult::InvalidBuffer);
		EXPECT_EQ(media.Read_AbsoluteSector(0, buffer), MediaResult::Ok);
		EXPECT_TRUE(std::ranges::all_of(buffer, [](const auto b) {
			return b == 0x37;
		}));
		EXPECT_EQ(guarded.front(), 0xa5);
		EXPECT_EQ(guarded.back(), 0xa5);
		std::ranges::fill(buffer, 0x62);
		EXPECT_EQ(media.Write_AbsoluteSector(0, buffer.first(size - 1)),
		          MediaResult::InvalidBuffer);
		EXPECT_EQ(media.Write_AbsoluteSector(0, guarded),
		          MediaResult::InvalidBuffer);
		EXPECT_EQ(media.Read_AbsoluteSector(0, buffer), MediaResult::Ok);
		EXPECT_EQ(buffer.front(), 0x37);
		std::ranges::fill(buffer, 0x62);
		EXPECT_EQ(media.Write_Sector(0, 0, 2, buffer), MediaResult::Ok);
		std::ranges::fill(buffer, 0);
		EXPECT_EQ(media.Read_Sector(0, 0, 2, buffer), MediaResult::Ok);
		EXPECT_TRUE(std::ranges::all_of(buffer, [](const auto b) {
			return b == 0x62;
		}));
		EXPECT_EQ(media.Read_Sector(0, 0, 0, buffer), MediaResult::NoSector);
		EXPECT_EQ(media.Read_Sector(1, 0, 1, buffer), MediaResult::NoSector);
		EXPECT_EQ(media.Read_AbsoluteSector(2, buffer), MediaResult::NoSector);
		EXPECT_EQ(media.Write_AbsoluteSector(2, buffer),
		          MediaResult::NoSector);
	}
}

TEST(SectorMedia, ShortReadIsNotSuccess)
{
	auto disk = make_raw_disk(512, 100);
	ASSERT_TRUE(disk);
	std::array<uint8_t, 512> data = {};
	EXPECT_EQ(disk->Read_AbsoluteSector(0, data), MediaResult::ReadError);
	EXPECT_EQ(disk->Read_AbsoluteSector(1, data), MediaResult::ReadError);
}

TEST(SectorMedia, RawWriteProtectionPreservesData)
{
	auto disk = make_raw_disk(512, 1024, true);
	ASSERT_TRUE(disk);
	std::array<uint8_t, 512> data = {};
	EXPECT_EQ(disk->Write_Sector(0, 0, 1, data), MediaResult::WriteProtected);
	EXPECT_EQ(disk->Read_Sector(0, 0, 1, data), MediaResult::Ok);
	EXPECT_TRUE(std::ranges::all_of(data,
	                                [](const auto b) { return b == 0x37; }));
}

TEST(SectorMedia, TrackConditionsAndDuplicateSelection)
{
	auto track          = std::make_shared<TestFloppyMedia>();
	FloppySector sector = {};
	sector.id           = 1;
	sector.size_code    = 2;
	sector.data.assign(512, 0x37);
	track->sectors.push_back(sector);
	imageDisk disk(track, "track-test", 1, 1, 1);
	std::array<uint8_t, 512> data = {};
	EXPECT_EQ(disk.Read_Sector(0, 0, 1, data), MediaResult::Ok);
	data.fill(0xa5);
	track->sectors[0].data_crc_error = true;
	EXPECT_EQ(disk.Read_Sector(0, 0, 1, data), MediaResult::CrcError);
	EXPECT_EQ(data.front(), 0xa5);
	track->sectors[0].data_crc_error = false;
	track->sectors[0].deleted        = true;
	EXPECT_EQ(disk.Read_Sector(0, 0, 1, data), MediaResult::DeletedMark);
	EXPECT_EQ(data.front(), 0xa5);
	track->sectors.push_back(sector);
	EXPECT_EQ(disk.Read_Sector(0, 0, 1, data), MediaResult::Ok);
	EXPECT_EQ(data.front(), 0x37);
	EXPECT_EQ(disk.Read_Sector(0, 0, 2, data), MediaResult::NoSector);
	EXPECT_EQ(disk.Write_Sector(0, 0, 1, std::span(data).first(511)),
	          MediaResult::InvalidBuffer);
	EXPECT_TRUE(track->written.empty());
	track->protected_media = true;
	EXPECT_EQ(disk.Write_Sector(0, 0, 1, data), MediaResult::WriteProtected);
	track->protected_media = false;
	track->fail_write      = true;
	EXPECT_EQ(disk.Write_Sector(0, 0, 1, data), MediaResult::WriteError);
	track->fail_write = false;
	EXPECT_EQ(disk.Write_Sector(0, 0, 2, data), MediaResult::NoSector);
	EXPECT_EQ(disk.Write_Sector(0, 0, 1, data), MediaResult::Ok);
	EXPECT_EQ(track->written.size(), data.size());
}
