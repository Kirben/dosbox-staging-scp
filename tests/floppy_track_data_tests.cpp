// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_media.h"

#include <algorithm>
#include <array>
#include <limits>

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

TEST(FloppyTrackData, SampleWeakCellsBeforeFieldDecoding)
{
	FloppyMfmTrack raw = {};
	raw.cells.assign(32, 0xff);
	raw.bit_count            = 256;
	raw.index                = 5;
	raw.revolution_ends_bits = {128, 256};
	raw.weak_cells.resize(raw.bit_count);
	raw.weak_cells[0]            = true;
	raw.weak_cells[7]            = true;
	const auto original          = raw.cells;
	std::array<bool, 4> observed = {};
	for (uint64_t cycle = 0; cycle < 64; ++cycle) {
		const auto first    = raw.Sample(cycle);
		const auto repeated = raw.Sample(cycle);
		EXPECT_EQ(first->cells, repeated->cells);
		EXPECT_EQ(first->index, raw.index);
		EXPECT_EQ(first->revolution_ends_bits, raw.revolution_ends_bits);
		EXPECT_TRUE(first->weak_cells.empty());
		EXPECT_EQ(first->cells[0] & 0x7e, 0x7e);
		EXPECT_TRUE(std::equal(first->cells.begin() + 1,
		                       first->cells.end(),
		                       original.begin() + 1));
		observed[((first->cells[0] >> 6) & 2) | (first->cells[0] & 1)] = true;
	}
	EXPECT_TRUE(std::ranges::all_of(observed,
	                                [](const auto value) { return value; }));
	EXPECT_EQ(raw.cells, original);
	EXPECT_TRUE(raw.weak_cells[0]);
}

TEST(FloppyTrackData, RecoverTransitionsAtSelectedRate)
{
	// Pulses every four microseconds form alternating raw cells at 250 kbps.
	const std::array<uint64_t, 4> transitions = {4000, 8000, 12000, 16000};
	const auto normal = FloppyMfmTrack::RecoverFlux(transitions, 16000, 2000);
	ASSERT_TRUE(normal);
	EXPECT_EQ(normal->bit_count, size_t{8});
	EXPECT_EQ(normal->cells, std::vector<uint8_t>({0x55}));
	const auto fast = FloppyMfmTrack::RecoverFlux(transitions, 16000, 1000);
	ASSERT_TRUE(fast);
	EXPECT_EQ(fast->bit_count, size_t{16});
	EXPECT_EQ(fast->cells, std::vector<uint8_t>({0x11, 0x11}));
	EXPECT_EQ(normal->Sample(0, 0.016, 250)->cells, normal->cells);
	const auto resampled = normal->Sample(0, 0.016, 500);
	ASSERT_TRUE(resampled);
	EXPECT_EQ(resampled->cells, fast->cells);
	EXPECT_EQ(normal->cells, std::vector<uint8_t>({0x55}));
}

TEST(FloppyTrackData, ResampleIndexAndRevolutionBoundaries)
{
	FloppyMfmTrack raw       = {};
	raw.cells                = {0x55, 0x55};
	raw.bit_count            = 16;
	raw.index                = 3;
	raw.revolution_ends_bits = {8, 16};

	const auto fast = raw.Sample(0, 0.032, 500);
	ASSERT_TRUE(fast);
	EXPECT_EQ(fast->cells, std::vector<uint8_t>({0x11, 0x11, 0x11, 0x11}));
	EXPECT_EQ(fast->index, size_t{6});
	EXPECT_EQ(fast->revolution_ends_bits, std::vector<size_t>({16, 32}));

	const auto slow = raw.Sample(0, 0.032, 125);
	ASSERT_TRUE(slow);
	EXPECT_EQ(slow->cells, std::vector<uint8_t>({0xff}));
	EXPECT_EQ(slow->index, size_t{2}); // 1.5 cells rounds away from zero.
	EXPECT_EQ(slow->revolution_ends_bits, std::vector<size_t>({4, 8}));
	EXPECT_EQ(raw.index, size_t{3});
	EXPECT_EQ(raw.revolution_ends_bits, std::vector<size_t>({8, 16}));
}

TEST(FloppyTrackData, ResampleTimestampsBeyond32Bits)
{
	FloppyMfmTrack raw = {};
	raw.cells          = {0x01};
	raw.bit_count      = 8;
	const auto sampled = raw.Sample(0, 5000, 125);
	ASSERT_TRUE(sampled);
	EXPECT_EQ(sampled->bit_count, size_t{1'250'000});
	ASSERT_EQ(sampled->cells.size(), size_t{156'250});
	EXPECT_EQ(sampled->cells.back(), 0x01);
}

TEST(FloppyTrackData, RejectUnrepresentableResamplingValues)
{
	FloppyMfmTrack raw = {};
	raw.cells          = {0x55};
	raw.bit_count      = 8;
	for (const auto duration : {std::numeric_limits<double>::quiet_NaN(),
	                            std::numeric_limits<double>::infinity(),
	                            std::numeric_limits<double>::max(),
	                            0.0000001}) {
		EXPECT_FALSE(raw.Sample(0, duration, 500));
	}
	raw.index = std::numeric_limits<size_t>::max();
	EXPECT_FALSE(raw.Sample(0, 0.016, 500));
	raw.index                = 0;
	raw.revolution_ends_bits = {std::numeric_limits<size_t>::max()};
	EXPECT_FALSE(raw.Sample(0, 0.016, 500));
}
