// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_media.h"

#include <algorithm>
#include <array>

#include <gtest/gtest.h>

TEST(FloppyTrackData, PreserveGapPhaseChangesAcrossIndex)
{
	FloppySector sector = {};
	sector.mfm_track    = std::make_shared<FloppyMfmTrack>();
	auto& track         = *sector.mfm_track;
	// Two phase changes in a gap must not be repaired as sector sync marks.
	for (int i = 0; i < 5; ++i) {
		track.cells.insert(track.cells.end(), {0xa4, 0x92});
	}
	track.cells.insert(track.cells.end(), {0xa4, 0xa4, 0x95});
	for (int i = 0; i < 16; ++i) {
		track.cells.insert(track.cells.end(), {0x24, 0x95});
	}
	track.bit_count                            = track.cells.size() * 8;
	sector.data_bit                            = track.bit_count - 30;
	const auto original                        = track.cells;
	constexpr std::array<uint8_t, 10> expected = {
	        0x4e, 0x4e, 0x48, 0x48, 0x48, 0x48, 0x48, 0x44, 0xe4, 0xe4};
	std::vector<uint8_t> first = {};
	auto bad_crc               = false;
	ASSERT_TRUE(sector.ReadTrackData(128, 0, first, bad_crc));
	ASSERT_EQ(first.size(), size_t{128});
	EXPECT_TRUE(bad_crc);
	EXPECT_TRUE(std::equal(expected.begin(), expected.end(), first.begin()));
	std::vector<uint8_t> next = {};
	ASSERT_TRUE(sector.ReadTrackData(128, 1, next, bad_crc));
	EXPECT_EQ(first, next);
	EXPECT_EQ(original, track.cells);
}

TEST(FloppyTrackData, KeepByteCadenceAcrossEmbeddedSyncMarks)
{
	// Each triple sync mark starts one cell after the data field's
	// byte boundary. Fixed-cadence reads sample its clock bits.
	constexpr std::array<std::array<uint8_t, 8>, 2> captures = {
	        {
                 {0x22, 0x44, 0xa2, 0x44, 0xa2, 0x44, 0x80, 0x00},
                 {0x29, 0x12, 0x29, 0x12, 0x29, 0x12, 0x00, 0x00},
	         }
        };
	constexpr std::array<uint8_t, 2> expected = {0x0a, 0x14};
	for (size_t i = 0; i < captures.size(); ++i) {
		FloppySector sector = {};
		sector.mfm_track    = std::make_shared<FloppyMfmTrack>();
		auto& track         = *sector.mfm_track;
		track.cells.assign(captures[i].begin(), captures[i].end());
		track.bit_count              = track.cells.size() * 8;
		std::vector<uint8_t> sampled = {};
		auto bad_crc                 = false;
		ASSERT_TRUE(sector.ReadTrackData(3, 0, sampled, bad_crc));
		EXPECT_EQ(sampled, std::vector<uint8_t>(3, expected[i]));
	}
}
