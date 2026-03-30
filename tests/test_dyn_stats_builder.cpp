// Tests for DynStatsBuilder's pure-logic private methods.
// Covered: chooseFreeSeqs, calcTransitionIsUnclear, setHasUnclearTransition,
// setDummyIsSkippable. None of these require file I/O.

#include "test_harness.h"
#include "core/dyn_stats_builder.h"
#include "core/free_seq.h"
#include "core/repair_context.h"
#include "track/track.h"
#include "util/common.h"

#include <functional>
#include <map>
#include <utility>
#include <vector>

using namespace std;

// Provides whitebox access to DynStatsBuilder's private methods via the
// friend struct declared in dyn_stats_builder.h.
struct DynStatsBuilderTest {
	static vector<FreeSeq> chooseFreeSeqs(DynStatsBuilder &b) { return b.chooseFreeSeqs(); }

	static bool calcTransitionIsUnclear(DynStatsBuilder &b, int a, int bx) { return b.calcTransitionIsUnclear(a, bx); }

	static void setHasUnclearTransition(DynStatsBuilder &b) { b.setHasUnclearTransition(); }

	static void setDummyIsSkippable(DynStatsBuilder &b) { b.setDummyIsSkippable(); }
};

// Builds a DynStatsBuilder from the given mutable state.
// The callbacks are no-ops unless advance_fn is provided.
static DynStatsBuilder makeBuilder(vector<Track> &tracks, map<pair<int, int>, vector<off_t>> &ct,
                                   vector<FreeSeq> &free_seqs, int &idx_free, FileState &file, ScanState &scan,
                                   ChunkOrderState &order, DummyTrackState &dummy,
                                   function<bool(off_t &, bool)> advance_fn = nullptr) {
	return DynStatsBuilder(DynStatsBuilder::Config{
	    tracks,
	    ct,
	    free_seqs,
	    idx_free,
	    /*twos_track_idx=*/-1,
	    file,
	    scan,
	    order,
	    dummy,
	    /*after_track_realloc=*/[] {},
	    /*advance_offset=*/advance_fn ? advance_fn : [](off_t &, bool) -> bool { return false; },
	    /*gen_track_order=*/[] {},
	});
}

void test_dyn_stats_builder() {
	cout << "test_dyn_stats_builder:\n";

	// --- chooseFreeSeqs ---
	//
	// chooseFreeSeqs removes the last occurrence of each prev_track_idx from
	// cfg_.free_seqs in-place, then returns the remaining entries (up to 100).
	// The purpose is to exclude the most-recent free sequence per track, which
	// may be at the end of the file and therefore not representative.

	// Two entries with the same prev_track_idx: the last is removed, the first
	// survives. After the call cfg_.free_seqs reflects the in-place modification.
	{
		vector<Track> tracks;
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs = {
		    FreeSeq{10, 50, 0, 200, "avc1"}, // idx 0
		    FreeSeq{60, 50, 0, 200, "avc1"}, // idx 1 (last for track 0, removed)
		};
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		auto result = DynStatsBuilderTest::chooseFreeSeqs(b);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0].offset, (off_t)10);
		CHECK_EQ((int)free_seqs.size(), 1); // cfg_.free_seqs modified in place
	}

	// Three entries across two tracks: one entry per track is removed as "the last",
	// leaving only the single earlier entry for track 0.
	{
		vector<Track> tracks;
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs = {
		    FreeSeq{10, 50, 0, 200, "avc1"},  // idx 0: track 0, kept
		    FreeSeq{60, 50, 0, 200, "avc1"},  // idx 1: last for track 0, removed
		    FreeSeq{120, 30, 1, 100, "mp4a"}, // idx 2: last for track 1, removed
		};
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		auto result = DynStatsBuilderTest::chooseFreeSeqs(b);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0].offset, (off_t)10);
	}

	// One entry per track: every entry is "the last" for its track, so all are
	// removed and chooseFreeSeqs returns an empty vector.
	{
		vector<Track> tracks;
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs = {
		    FreeSeq{10, 50, 0, 200, "avc1"},
		    FreeSeq{60, 50, 1, 100, "mp4a"},
		};
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		auto result = DynStatsBuilderTest::chooseFreeSeqs(b);
		CHECK_EQ((int)result.size(), 0);
	}

	// --- calcTransitionIsUnclear ---
	//
	// Returns true only when the target track is unsupported AND the source
	// track has no patterns for that target AND there are more than 5 transitions.
	// All three conditions must hold simultaneously.

	// All conditions met (unsupported target, >5 transitions, no patterns): unclear.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("unkn");
		tracks[0].dyn_patterns_.resize(2);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		ct[{0, 1}] = vector<off_t>(6); // 6 > 5
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs;
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		CHECK(DynStatsBuilderTest::calcTransitionIsUnclear(b, 0, 1));
	}

	// Patterns exist for A->B: transition is no longer "unclear" even with many transitions.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("unkn");
		tracks[0].dyn_patterns_.resize(2);
		// Add one pattern for the A->B slot to make it non-empty.
		vector<uchar> buf(kPatternSize, 0);
		tracks[0].dyn_patterns_[1].emplace_back(buf, buf);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		ct[{0, 1}] = vector<off_t>(6);
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs;
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		CHECK(!DynStatsBuilderTest::calcTransitionIsUnclear(b, 0, 1));
	}

	// Exactly 5 transitions: not > 5, so transition is not "unclear".
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("unkn");
		tracks[0].dyn_patterns_.resize(2);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		ct[{0, 1}] = vector<off_t>(5); // 5 is not > 5
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs;
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		CHECK(!DynStatsBuilderTest::calcTransitionIsUnclear(b, 0, 1));
	}

	// --- setHasUnclearTransition ---
	//
	// Populates has_unclear_transition_ for all track pairs. The result for A->B
	// depends on calcTransitionIsUnclear(A, B).

	// Two tracks: only the A->B transition is unclear (6 transitions, no patterns,
	// B unsupported). All other pairs must be false.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("unkn");
		tracks[0].dyn_patterns_.resize(2);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		ct[{0, 1}] = vector<off_t>(6);
		int idx_free = -2;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs;
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		DynStatsBuilderTest::setHasUnclearTransition(b);

		CHECK_EQ((int)tracks[0].has_unclear_transition_.size(), 2);
		CHECK(!tracks[0].has_unclear_transition_[0]); // A->A: 0 transitions -> false
		CHECK(tracks[0].has_unclear_transition_[1]);  // A->B: 6 transitions, no patterns -> true
		CHECK_EQ((int)tracks[1].has_unclear_transition_.size(), 2);
		CHECK(!tracks[1].has_unclear_transition_[0]); // B->A: 0 transitions -> false
		CHECK(!tracks[1].has_unclear_transition_[1]); // B->B: 0 transitions -> false
	}

	// --- setDummyIsSkippable ---
	//
	// Sets dummy.is_skippable_ based on whether the "free" track's sequences can
	// be skipped during repair. When idx_free < 0 there is no free track and the
	// method returns immediately without changing is_skippable_.

	// No free track: is_skippable_ is never set to true.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks[0].dyn_patterns_.resize(1);
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = -2; // kNoFreeTrack
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs;
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		DynStatsBuilderTest::setDummyIsSkippable(b);
		CHECK(!dummy.is_skippable_);
	}

	// Free track exists. chooseFreeSeqs removes the only entry (single entry per
	// track), leaving nothing for canSkipFree to check. An empty sequence list
	// means the free sequences are trivially skippable.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("free");
		tracks[0].dyn_patterns_.resize(2);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = 1;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		// Single entry: chooseFreeSeqs removes it as "the last" -> canSkipFree sees []
		vector<FreeSeq> free_seqs = {FreeSeq{100, 50, 0, 200, "avc1"}};
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy);
		DynStatsBuilderTest::setDummyIsSkippable(b);
		CHECK(dummy.is_skippable_);
	}

	// Free track exists, two entries with the same prev_track_idx: chooseFreeSeqs
	// keeps the first and removes the second. canSkipFree then checks whether
	// advance_offset can navigate from offset=100 to off_end=150. The mock
	// advances by only 1 byte, which never reaches 150, so the track is not skippable.
	{
		vector<Track> tracks;
		tracks.emplace_back("avc1");
		tracks.emplace_back("free");
		tracks[0].dyn_patterns_.resize(2);
		tracks[1].dyn_patterns_.resize(2);
		map<pair<int, int>, vector<off_t>> ct;
		int idx_free = 1;
		FileState file;
		ScanState scan;
		ChunkOrderState order;
		DummyTrackState dummy;
		vector<FreeSeq> free_seqs = {
		    FreeSeq{100, 50, 0, 200, "avc1"}, // kept by chooseFreeSeqs
		    FreeSeq{200, 50, 0, 200, "avc1"}, // removed as "last for track 0"
		};
		// advance_offset advances by 1, never reaches off_end=150
		auto b = makeBuilder(tracks, ct, free_seqs, idx_free, file, scan, order, dummy, [](off_t &off, bool) -> bool {
			off += 1;
			return true;
		});
		DynStatsBuilderTest::setDummyIsSkippable(b);
		CHECK(!dummy.is_skippable_);
	}
}
