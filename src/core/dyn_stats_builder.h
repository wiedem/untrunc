#pragma once
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/free_seq.h"
#include "core/repair_context.h"
#include "track/track.h"
#include "util/common.h"
#include "util/mutual_pattern.h"

class DynStatsBuilder {
  public:
	struct Config {
		std::vector<Track> &tracks;
		std::map<std::pair<int, int>, std::vector<off_t>> &chunk_transitions;
		std::vector<FreeSeq> &free_seqs;
		int &idx_free;
		int twos_track_idx;
		FileState &file;
		ScanState &scan;
		ChunkOrderState &order;
		DummyTrackState &dummy;
		std::function<void()> after_track_realloc;
		std::function<bool(off_t &, bool)> advance_offset;
		std::function<void()> gen_track_order;
	};

	explicit DynStatsBuilder(Config cfg);

	void generate();

  private:
	friend struct DynStatsBuilderTest;

	Config cfg_;

	static constexpr int kNoFreeTrack = -2;
	static constexpr int pat_size_ = kPatternSize;

	void genChunks();
	void genChunkTransitions();
	void resetChunkTransitions();
	bool analyzeFree();
	void genLikelyAll();
	void genDynPatterns();
	void setHasUnclearTransition();
	void setDummyIsSkippable();

	bool canSkipFree();
	std::vector<FreeSeq> chooseFreeSeqs();
	bool calcTransitionIsUnclear(int track_idx_a, int track_idx_b);
	buffs_t offsToBuffs(const offs_t &offs, const std::string &load_prefix);
	patterns_t offsToPatterns(const offs_t &offs, const std::string &load_prefix);
};
