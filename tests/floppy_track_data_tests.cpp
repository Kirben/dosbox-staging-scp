// SPDX-License-Identifier: GPL-2.0-or-later

#include "ints/floppy_media.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "misc/support.h"
#include "utils/checks.h"

CHECK_NARROWING();

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

TEST(FloppyTrackData, PreserveResampledCellTimes)
{
	// Changing clocks on a mixed-density track must not linearly stretch
	// the earlier fields to match the separator's final cell count.
	constexpr std::array<uint64_t, 4> transitions = {4000, 10000, 14000, 20000};
	const auto raw = FloppyMfmTrack::RecoverFlux(transitions, 24000, 4000);
	const auto timed = FloppyMfmTrack::RecoverFlux(transitions, 24000, 4000, true);
	ASSERT_TRUE(raw);
	ASSERT_TRUE(timed);
	EXPECT_EQ(raw->cells, timed->cells);
	EXPECT_TRUE(raw->cell_ends.empty());
	ASSERT_EQ(timed->cell_ends.size(), timed->bit_count);
	EXPECT_TRUE(std::ranges::is_sorted(timed->cell_ends));
	size_t next = 0;
	for (size_t cell = 0; cell < timed->bit_count; ++cell) {
		if (timed->cells[cell / 8] & (0x80u >> (cell % 8))) {
			ASSERT_LT(next, transitions.size());
			EXPECT_DOUBLE_EQ(timed->cell_ends[cell],
			                 static_cast<double>(transitions[next++]) /
			                         24000);
		}
	}
	EXPECT_EQ(next, transitions.size());
	EXPECT_DOUBLE_EQ(timed->cell_ends.back(), 1.0);
}

TEST(FloppyTrackData, HoldSeparatorFrequencyAcrossLongGaps)
{
	for (const auto nominal_ns : {500, 1000, 2000, 4000}) {
		for (const auto speed_percent : {91, 94, 106, 109}) {
			SCOPED_TRACE(nominal_ns);
			SCOPED_TRACE(speed_percent);
			std::vector<uint64_t> transitions = {};
			std::vector<uint64_t> normalized  = {};
			std::vector<uint8_t> expected     = {};
			auto elapsed_ns                   = uint64_t{0};
			auto normalized_ns                = uint64_t{0};
			auto cells                        = size_t{0};
			for (int pulse = 0; pulse < 288; ++pulse) {
				const auto spacing = pulse >= 128 && pulse < 160
				                           ? 12
				                           : 2;
				elapsed_ns += check_cast<uint64_t>(
				        nominal_ns * speed_percent * spacing / 100);
				normalized_ns += check_cast<uint64_t>(
				        nominal_ns * spacing);
				// Alternating edge displacement must not become
				// speed drift.
				const auto jitter_ns = pulse % 2 ? nominal_ns / 25
				                                 : 0;
				transitions.push_back(
				        elapsed_ns + check_cast<uint64_t>(jitter_ns));
				normalized.push_back(normalized_ns);
				cells += check_cast<size_t>(spacing);
				expected.resize((cells + 7) / 8);
				expected[(cells - 1) / 8] |= check_cast<uint8_t>(
				        0x80u >> ((cells - 1) % 8));
			}
			constexpr auto TailCells = 32;
			const auto duration_ns = elapsed_ns +
			                         check_cast<uint64_t>(
			                                 nominal_ns * speed_percent *
			                                 TailCells / 100);
			const auto control_duration_ns =
			        normalized_ns +
			        check_cast<uint64_t>(nominal_ns * TailCells);
			expected.resize((cells + TailCells + 7) / 8);
			const auto raw = FloppyMfmTrack::RecoverFlux(transitions,
			                                             duration_ns,
			                                             nominal_ns,
			                                             true);
			const auto control = FloppyMfmTrack::RecoverFlux(
			        normalized, control_duration_ns, nominal_ns);
			ASSERT_TRUE(raw);
			ASSERT_TRUE(control);
			EXPECT_EQ(raw->bit_count, cells + TailCells);
			EXPECT_EQ(raw->cells, expected);
			EXPECT_EQ(raw->cells, control->cells);
			ASSERT_EQ(raw->cell_ends.size(), raw->bit_count);
			EXPECT_TRUE(std::ranges::is_sorted(raw->cell_ends));
			EXPECT_DOUBLE_EQ(raw->cell_ends.back(), 1.0);
		}
	}
}

TEST(FloppyTrackData, AcquireSeparatorFromRecordedTail)
{
	constexpr uint64_t CellNs         = 1875;
	constexpr uint64_t DurationNs     = 512 * CellNs;
	std::vector<uint64_t> transitions = {};
	for (uint64_t time = 100; time < DurationNs; time += 2 * CellNs) {
		transitions.push_back(time);
	}
	const std::array<uint64_t, 1> ends = {DurationNs};
	const auto acquired = FloppyMfmTrack::RecoverFluxRevolutions(transitions,
	                                                             ends,
	                                                             2000);
	const auto cold = FloppyMfmTrack::RecoverFlux(transitions, DurationNs, 2000);
	ASSERT_TRUE(acquired);
	ASSERT_TRUE(cold);
	// The first transition is only 100 ns after index. Its interval starts
	// in the preceding recorded tail; cold decoding loses that transition.
	EXPECT_EQ(acquired->bit_count, size_t{512});
	EXPECT_EQ(acquired->cells, std::vector<uint8_t>(64, 0xaa));
	EXPECT_NE(acquired->cells, cold->cells);
	EXPECT_EQ(acquired->revolution_ends_bits, std::vector<size_t>({512}));
}

TEST(FloppyTrackData, KeepSeparatorPhaseAcrossFractionalIndex)
{
	constexpr uint64_t CellNs         = 1820;
	constexpr uint64_t DurationNs     = 1024 * CellNs;
	std::vector<uint64_t> transitions = {};
	for (uint64_t time = 2 * CellNs; time <= DurationNs; time += 2 * CellNs) {
		transitions.push_back(time);
	}
	// Neither boundary is a byte boundary; both bisect a flux interval.
	const std::array<uint64_t, 3> ends = {257 * CellNs + CellNs / 4,
	                                      513 * CellNs + 3 * CellNs / 4,
	                                      DurationNs};
	const std::array<uint64_t, 1> unsplit_end = {DurationNs};
	const auto split   = FloppyMfmTrack::RecoverFluxRevolutions(transitions,
	                                                            ends,
	                                                            2000);
	const auto unsplit = FloppyMfmTrack::RecoverFluxRevolutions(transitions,
	                                                            unsplit_end,
	                                                            2000);
	ASSERT_TRUE(split);
	ASSERT_TRUE(unsplit);
	EXPECT_EQ(split->bit_count, size_t{1024});
	EXPECT_EQ(split->cells, std::vector<uint8_t>(128, 0x55));
	EXPECT_EQ(split->cells, unsplit->cells);
	EXPECT_EQ(split->revolution_ends_bits,
	          std::vector<size_t>({257, 513, 1024}));
	EXPECT_EQ(split->GetRevolutionBitCount(0), size_t{257});
	EXPECT_EQ(split->GetRevolutionBitCount(1), size_t{256});
	EXPECT_EQ(split->GetRevolutionBitCount(2), size_t{511});
}

TEST(FloppyTrackData, AcquisitionDoesNotInventFlux)
{
	const std::array<uint64_t, 2> ends = {500000, 1000000};
	const auto empty = FloppyMfmTrack::RecoverFluxRevolutions({}, ends, 2000);
	ASSERT_TRUE(empty);
	EXPECT_EQ(empty->bit_count, size_t{500});
	EXPECT_EQ(empty->cells, std::vector<uint8_t>(63));
	EXPECT_EQ(empty->revolution_ends_bits, std::vector<size_t>({250, 500}));
	for (const auto nominal : {0.0,
	                           -1.0,
	                           std::numeric_limits<double>::infinity(),
	                           std::numeric_limits<double>::quiet_NaN()}) {
		EXPECT_FALSE(FloppyMfmTrack::RecoverFluxRevolutions({}, ends, nominal));
	}
	for (const auto invalid : {
	             std::array<uint64_t, 2>{   0, 1000},
	             std::array<uint64_t, 2>{1000, 1000},
	             std::array<uint64_t, 2>{2000, 1000}
        }) {
		EXPECT_FALSE(FloppyMfmTrack::RecoverFluxRevolutions({}, invalid, 2000));
	}
	const std::array<uint64_t, 2> unordered = {6000, 4000};
	EXPECT_FALSE(FloppyMfmTrack::RecoverFluxRevolutions(unordered, ends, 2000));
	const std::array<uint64_t, 1> beyond_end = {1000001};
	EXPECT_FALSE(FloppyMfmTrack::RecoverFluxRevolutions(beyond_end, ends, 2000));
	EXPECT_FALSE(FloppyMfmTrack::RecoverFlux(beyond_end, 1000001, 1e-20));
}

TEST(FloppyTrackData, ContinuousFluxKeepsPerRevolutionLimits)
{
	const std::array<uint64_t, 3> ends = {550000000, 1100000000, 1650000000};
	const auto many = FloppyMfmTrack::RecoverFluxRevolutions({}, ends, 500);
	ASSERT_TRUE(many);
	EXPECT_EQ(many->bit_count, size_t{3300000});
	EXPECT_EQ(many->revolution_ends_bits,
	          std::vector<size_t>({1100000, 2200000, 3300000}));
	const std::array<uint64_t, 1> oversized = {1050000500};
	EXPECT_FALSE(FloppyMfmTrack::RecoverFluxRevolutions({}, oversized, 500));
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

TEST(FloppyTrackData, ResampleBoundaryCellsWithAssertions)
{
	FloppyMfmTrack raw       = {};
	raw.cells                = {0x55};
	raw.bit_count            = 8;
	raw.revolution_ends_bits = {0, 1, 8};
	for (const auto index : {size_t{0}, size_t{1}, size_t{7}}) {
		raw.index          = index;
		const auto sampled = raw.Sample(0, 0.016, 500);
		ASSERT_TRUE(sampled);
		EXPECT_EQ(sampled->index, index * 2);
		EXPECT_EQ(sampled->revolution_ends_bits,
		          std::vector<size_t>({0, 2, 16}));
		EXPECT_EQ(sampled->cells, std::vector<uint8_t>({0x11, 0x11}));
	}
}

TEST(FloppyTrackData, ResamplePhysicalIndexTimestamp)
{
	const std::array<uint64_t, 8> transitions = {
	        3600, 7200, 10800, 14400, 18800, 23200, 27600, 32000};
	const auto raw = FloppyMfmTrack::RecoverFlux(transitions, 32000, 2000, true);
	ASSERT_TRUE(raw);
	raw->index                = 7;
	raw->revolution_ends_bits = {7, raw->bit_count};
	const auto original_times = raw->cell_ends;
	const auto sampled        = raw->Sample(0, 0.032, 500);
	ASSERT_TRUE(sampled);
	ASSERT_GT(sampled->index, size_t{0});
	EXPECT_DOUBLE_EQ(sampled->cell_ends[sampled->index - 1], raw->cell_ends[6]);
	EXPECT_EQ(sampled->revolution_ends_bits.front(), sampled->index);
	EXPECT_EQ(sampled->revolution_ends_bits.back(), sampled->bit_count);
	EXPECT_EQ(raw->index, size_t{7});
	EXPECT_EQ(raw->cell_ends, original_times);
}

TEST(FloppyTrackData, ResampleVariableClockRevolutionTimestamp)
{
	std::vector<uint64_t> transitions = {};
	uint64_t time                     = 0;
	for (const auto interval : {3600u, 4400u}) {
		for (size_t i = 0; i < 25'000; ++i) {
			time += interval;
			transitions.push_back(time);
		}
	}
	const auto raw = FloppyMfmTrack::RecoverFlux(transitions, time, 2000, true);
	ASSERT_TRUE(raw);
	raw->index                = 50'007;
	raw->revolution_ends_bits = {raw->index, raw->bit_count};
	const auto sampled        = raw->Sample(0, 200, 500);
	ASSERT_TRUE(sampled);
	const auto physical_time = raw->cell_ends[raw->index - 1];
	const auto upper         = std::lower_bound(sampled->cell_ends.begin(),
                                            sampled->cell_ends.end(),
                                            physical_time);
	ASSERT_NE(upper, sampled->cell_ends.end());
	const auto boundary = sampled->index;
	ASSERT_GT(boundary, size_t{0});
	const auto selected = sampled->cell_ends[boundary - 1];
	EXPECT_LE(std::abs(selected - physical_time),
	          std::abs(*upper - physical_time));
	if (upper != sampled->cell_ends.begin()) {
		EXPECT_LE(std::abs(selected - physical_time),
		          std::abs(*(upper - 1) - physical_time));
	}
	EXPECT_EQ(sampled->revolution_ends_bits.front(), boundary);
	EXPECT_EQ(sampled->revolution_ends_bits.back(), sampled->bit_count);
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

TEST(FloppyTrackData, StopWriteAtResetCell)
{
	for (const auto fm : {false, true}) {
		FloppySector original = {};
		original.fm           = fm;
		original.data.assign(128, 0x11);
		original.data_bit  = 64;
		original.mfm_track = std::make_shared<FloppyMfmTrack>();
		original.mfm_track->bit_count = 8192;
		original.mfm_track->cells.assign(1024, 0x55);
		original.UpdateTrackData();
		original.mfm_track->weak_cells.assign(8192, true);
		auto complete      = original;
		complete.mfm_track = std::make_shared<FloppyMfmTrack>(
		        *original.mfm_track);
		complete.data.assign(128, 0x55);
		complete.UpdateTrackData();
		// Include resets inside the DAM, payload, and CRC, not just at
		// byte boundaries. No trailing clock cell may be repaired.
		for (const auto limit : {1u, 15u, 16u, 17u, 1031u, 2069u, 2095u}) {
			auto interrupted = original;
			interrupted.mfm_track = std::make_shared<FloppyMfmTrack>(
			        *original.mfm_track);
			interrupted.data.assign(128, 0x55);
			interrupted.UpdateTrackData(false, limit);
			auto expected = original.mfm_track->cells;
			auto weak     = original.mfm_track->weak_cells;
			for (size_t offset = 0; offset < limit; ++offset) {
				const auto cell = (original.data_bit - 16 + offset) %
				                  8192;
				const auto mask = static_cast<uint8_t>(
				        0x80u >> (cell % 8));
				expected[cell / 8] = static_cast<uint8_t>(
				        (expected[cell / 8] &
				         static_cast<uint8_t>(~mask)) |
				        (complete.mfm_track->cells[cell / 8] & mask));
				weak[cell] = false;
			}
			EXPECT_EQ(interrupted.mfm_track->cells, expected);
			EXPECT_EQ(interrupted.mfm_track->weak_cells, weak);
		}
	}
}
