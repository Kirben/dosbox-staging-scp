// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/bios_disk.h"
#include "ints/disk_image_factory.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct CloseFile {
	void operator()(FILE* file) const
	{
		fclose(file);
	}
};
using File = std::unique_ptr<FILE, CloseFile>;

File make_image(const std::span<const uint8_t> bytes)
{
	File file(tmpfile());
	if (!file ||
	    fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
		return {};
	}
	return file;
}

TEST(DiskImageFactory, ContentProbesPreservePositionAndIgnoreMisleadingSuffixes)
{
	const std::array<std::vector<uint8_t>, 4> signatures = {
	        std::vector<uint8_t>{'H', 'X', 'C', 'M', 'F', 'M', 0},
	        std::vector<uint8_t>{'S', 'C', 'P'},
	        std::vector<uint8_t>{'8', '6', 'B', 'F'},
	        std::vector<uint8_t>{0x5a, 0xa5},
	};
	for (const auto& signature : signatures) {
		auto file = make_image(signature);
		ASSERT_TRUE(file);
		const auto position = ftell(file.get());
		for (const auto* name :
		     {"capture.bin", "capture.img", "capture", "wrong.scp"}) {
			EXPECT_TRUE(IsFloppyTrackImage(file.get(), name));
			EXPECT_EQ(ftell(file.get()), position);
		}
		// Recognizing a signature is not enough to accept a damaged
		// container.
		EXPECT_FALSE(LoadFloppyTrackDisk(file.get(), "capture.bin", true));
	}
}

TEST(DiskImageFactory, KnownSuffixRejectsMalformedContainerWithoutRawFallback)
{
	const std::array<uint8_t, 16> bytes = {};
	auto file                           = make_image(bytes);
	ASSERT_TRUE(file);
	for (const auto* name :
	     {"capture.mfm", "capture.scp", "capture.86f", "capture.tc"}) {
		EXPECT_TRUE(IsFloppyTrackImage(file.get(), name));
		EXPECT_FALSE(LoadFloppyTrackDisk(file.get(), name, true));
	}
	EXPECT_FALSE(IsFloppyTrackImage(file.get(), "capture.img"));
	EXPECT_FALSE(IsFloppyTrackImage(file.get(), "capture.bin"));
}

class DiskImageFactoryPathTest : public testing::Test {
protected:
	std::filesystem::path path = std::filesystem::path(testing::TempDir()) /
	                             "floppy-factory-raw-fallback.img";

	void TearDown() override
	{
		std::error_code error = {};
		std::filesystem::remove(path, error);
		EXPECT_FALSE(error) << error.message();
	}
};

TEST_F(DiskImageFactoryPathTest, RawFallbackHonorsDiskTypeAndRejectsDisguisedContainers)
{
	std::vector<uint8_t> bytes(360 * 1024);
	{
		File file(fopen(path.string().c_str(), "wb"));
		ASSERT_TRUE(file);
		ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), file.get()),
		          bytes.size());
	}
	bool read_only = true;
	auto disk      = OpenSectorDiskImage(path.string(), read_only, false);
	ASSERT_TRUE(disk);
	EXPECT_TRUE(disk->active);
	EXPECT_FALSE(disk->hardDrive);
	EXPECT_FALSE(disk->GetFloppyMedia());
	disk.reset();
	disk = OpenSectorDiskImage(path.string(), read_only, true);
	ASSERT_TRUE(disk);
	EXPECT_TRUE(disk->hardDrive);
	disk.reset();
	// Magic takes precedence over .img, and a damaged capture must not fall
	// through to raw loading or be accepted as an explicitly selected HDD.
	bytes[0] = 'S';
	bytes[1] = 'C';
	bytes[2] = 'P';
	{
		File file(fopen(path.string().c_str(), "wb"));
		ASSERT_TRUE(file);
		ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), file.get()),
		          bytes.size());
	}
	EXPECT_FALSE(OpenSectorDiskImage(path.string(), read_only, false));
	EXPECT_FALSE(OpenSectorDiskImage(path.string(), read_only, true));
}

} // namespace
