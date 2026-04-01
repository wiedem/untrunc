// Mp4Repairer owns the repair scan loop.
// Mp4::repair() is a thin wrapper that delegates here.
// All helper methods of the scan loop that are not also used during stats
// generation live here; the rest stay in Mp4 private (accessible via friend).
#pragma once

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

	bool chkOffset(off_t &offset);
	void chkExcludeOverlap(off_t &start, int64_t &length);

	void addMatch(off_t &offset, FrameInfo &match);
	bool tryMatch(off_t &offset);
	bool tryChunkPrediction(off_t &offset);
	bool tryAll(off_t &offset);
};
