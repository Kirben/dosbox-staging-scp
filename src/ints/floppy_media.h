// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_FLOPPY_MEDIA_H
#define DOSBOX_FLOPPY_MEDIA_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// FNV-1a binds a sidecar to the exact source image.
inline uint64_t FloppyImageFingerprint(const std::span<const uint8_t> bytes)
{
	auto fingerprint = uint64_t{14695981039346656037ull};
	for (const auto byte : bytes) {
		fingerprint = (fingerprint ^ byte) * 1099511628211ull;
	}
	return fingerprint;
}

struct FloppyMfmTrack {
	std::vector<uint8_t> cells   = {};
	size_t bit_count             = 0;
	size_t index                 = 0;
	std::vector<bool> weak_cells = {};
	// Cumulative cell boundaries when several captured turns share one stream.
	std::vector<size_t> revolution_ends_bits = {};

	std::shared_ptr<FloppyMfmTrack> Sample(const uint64_t cycle,
	                                       const double duration_ms = 0,
	                                       const uint16_t rate_kbps = 0) const;
	static std::shared_ptr<FloppyMfmTrack> RecoverFlux(
	        const std::span<const uint64_t> transitions_ns,
	        const uint64_t duration_ns, const double nominal_cell_ns);

	size_t GetRevolutionBitCount(const size_t revolution) const
	{
		if (revolution_ends_bits.empty()) {
			return bit_count;
		}
		assert(revolution < revolution_ends_bits.size());
		const auto start = revolution
		                         ? revolution_ends_bits[revolution - 1]
		                         : 0;
		return revolution_ends_bits[revolution] - start;
	}
};

struct FloppySector {
	uint8_t cylinder                   = 0;
	uint8_t head                       = 0;
	uint8_t id                         = 0;
	uint8_t size_code                  = 0;
	double cycle_fraction              = 0;
	bool id_crc_error                  = false;
	bool data_crc_error                = false;
	bool deleted                       = false;
	size_t revolution                  = 0;
	size_t overlay_slot                = 0;
	std::vector<uint8_t> data          = {};
	std::vector<uint8_t> extended_data = {};
	std::vector<uint8_t> weak_mask = {}; // Payload followed by the two CRC
	                                     // bytes.
	uint16_t recorded_crc                     = 0;
	size_t weak_seed                          = 0;
	std::shared_ptr<FloppyMfmTrack> mfm_track = {};
	size_t data_bit                           = 0;
	size_t id_bit                             = 0;
	size_t data_delay_bits                    = 0;

	// Sample one physical turn and return whether its data CRC is bad.
	bool SampleWeakData(const uint64_t turn, std::vector<uint8_t>& sampled) const;
	bool ReadTrackData(const size_t size, const uint64_t turn,
	                   std::vector<uint8_t>& sampled, bool& bad_crc) const;
	void UpdateTrackData(const bool create_data_mark = false);
};

constexpr bool IsFloppyOverlaySizeCode(const int size_code)
{
	return size_code >= 1 && size_code <= 3;
}

constexpr bool IsFloppyDataRate(const uint16_t rate_kbps)
{
	return rate_kbps == 250 || rate_kbps == 300 || rate_kbps == 500 ||
	       rate_kbps == 1000;
}

// Physical track access. Logical sector callers use SectorMedia.
class FloppyMedia {
public:
	enum class FormatResult { Success, InvalidLayout, WriteError };

	virtual ~FloppyMedia()                               = default;
	virtual double GetCycleMs(const uint32_t cylinder,
	                          const uint32_t head) const = 0;
	// Index positions as fractions of the full capture cycle, ending at 1.
	virtual std::span<const double> GetRevolutionEnds(const uint32_t cylinder,
	                                                  const uint32_t head) const = 0;
	virtual double GetFormatCycleMs(const uint32_t cylinder,
	                                const uint32_t head) const = 0;
	virtual std::span<const FloppySector> GetTrack(const uint32_t cylinder,
	                                               const uint32_t head) const = 0;
	// Command-time field decoding is separate from the logical sector view.
	virtual std::vector<FloppySector> SampleTrack(const uint32_t cylinder,
	                                              const uint32_t head,
	                                              const uint64_t,
	                                              const uint16_t) const
	{
		const auto sectors = GetTrack(cylinder, head);
		return {sectors.begin(), sectors.end()};
	}
	virtual bool SupportsTrackWrites() const                   = 0;
	virtual bool IsWriteProtected() const                      = 0;
	virtual bool WriteSector(const uint32_t cylinder, const uint32_t head,
	                         const uint32_t sector,
	                         const std::span<const uint8_t> data,
	                         const FloppySector* selected = nullptr,
	                         const bool deleted           = false)       = 0;
	virtual FormatResult FormatTrack(const uint8_t cylinder, const uint8_t head,
	                                 const std::span<const uint8_t> ids,
	                                 const uint8_t fill, const uint8_t requested,
	                                 const uint8_t size_code,
	                                 const uint16_t rate_kbps) = 0;
};

#endif
