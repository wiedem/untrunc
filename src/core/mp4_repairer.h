// Mp4Repairer owns the repair scan loop.
// Mp4::repair() is a thin wrapper that delegates here.
// All helper methods of the scan loop that are not also used during stats
// generation live here; the rest stay in Mp4 private (accessible via friend).
#pragma once

#include <optional>
#include <string>

#include "mp4.h" // brings in all needed types
#include "repair_report.h"

class Mp4Repairer {
  public:
	Mp4Repairer(Mp4 &mp4, RepairReport &report);
	void repair(const std::string &filename);

	// Pure function: returns the sizes from `sizes` whose relative frequency
	// is >= min_freq, sorted ascending. Exposed for unit testing.
	static std::vector<int> collectLikelySizes(const std::vector<int> &sizes, double min_freq = 0.01);

	// Returns {offset, length} of the first IDR_SLICE NAL unit in an AVCC-format stream,
	// or nullopt if none is found within `size` bytes. Exposed for unit testing.
	static std::optional<std::pair<int, int>> findIdrInAvcc(const uchar *data, int size, int nal_length_size);

	// Returns {offset, length} of the first IDR NAL unit (IDR_W_RADL or IDR_N_LP) in an
	// HVCC-format stream, or nullopt if none is found within `size` bytes. Exposed for unit testing.
	static std::optional<std::pair<int, int>> findIdrInHvcc(const uchar *data, int size, int nal_length_size);

	// Returns {offset, length} of the first SPS NAL unit (type 33) in an HVCC-format stream,
	// or nullopt if none is found before the first slice or within `size` bytes.
	// Per ISO 14496-15, hev1 streams may carry VPS/SPS/PPS in-band (in mdat), which makes
	// this relevant for camera recordings that repeat parameter sets before each IDR.
	// Exposed for unit testing.
	static std::optional<std::pair<int, int>> findSpsInHvcc(const uchar *data, int size, int nal_length_size);

	// Parses {profile_idc, level_idc} from a raw H.265 SPS NAL unit (2-byte header + RBSP).
	// Strips emulation prevention bytes (00 00 03 -> 00 00) before reading the fixed-offset
	// fields. Returns nullopt if nal_bytes < 15 or EPB stripping yields fewer than 13 RBSP bytes.
	// Exposed for unit testing.
	static std::optional<std::pair<int, int>> parseSpsH265ProfileLevel(const uchar *nal, int nal_bytes);

  private:
	Mp4 &mp4_;
	RepairReport &report_;

	// repair() phases
	bool handleSpecialModes(const std::string &filename);
	void prepareStats(const std::string &filename);
	BufferedAtom *setupFile(const std::string &filename);
	void precomputeAudioSampleSizes();
	off_t calcStartOffset(BufferedAtom *mdat);

	// Scan-loop helpers
	bool shouldPreferChunkPrediction();
	void pushBackLastChunk();
	void onNewChunkStarted(int new_track_idx);

	void checkRefCompatibility();
	void verifyCompatByDecode();

	bool chkOffset(off_t &offset);
	void chkExcludeOverlap(off_t &start, int64_t &length);

	void addMatch(off_t &offset, FrameInfo &match);
	bool tryMatch(off_t &offset);
	bool tryChunkPrediction(off_t &offset);
	bool tryAll(off_t &offset);
};
