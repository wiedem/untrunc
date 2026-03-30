#include "test_harness.h"
#include "track/track.h"
#include "util/mutual_pattern.h"

// Tests for Track chunk-offset and scan helpers.
// No FFmpeg, no file I/O, no Mp4 dependency.
// All tests use the dummy Track constructor.

void test_track_scanner() {
	std::cout << "test_track_scanner:\n";

	// isChunkOffsetOk: when current_chunk_.off_==0, only the absolute alignment
	// (mdat_content_start_ + off) % start_off_gcd_ is checked.
	{
		Track t("dummy");
		t.start_off_gcd_ = 32;
		t.mdat_content_start_ = 0;
		// current_chunk_.off_ defaults to 0, so the distance check is skipped
		CHECK(t.isChunkOffsetOk(0));
		CHECK(t.isChunkOffsetOk(32));
		CHECK(t.isChunkOffsetOk(64));
		CHECK(!t.isChunkOffsetOk(16));
		CHECK(!t.isChunkOffsetOk(48));
	}

	// isChunkOffsetOk: when current_chunk_.off_ != 0, both the absolute alignment
	// and the chunk-distance condition must hold.
	{
		Track t("dummy");
		t.start_off_gcd_ = 32;
		t.chunk_distance_gcd_ = 128;
		t.mdat_content_start_ = 0;
		t.current_chunk_.off_ = 128;

		// absolute ok (256%32==0), distance ok ((256-128)%128==0)
		CHECK(t.isChunkOffsetOk(256));
		// absolute ok (160%32==0), distance fail ((160-128)%128==32)
		CHECK(!t.isChunkOffsetOk(160));
		// absolute fail (16%32!=0), short-circuits before distance check
		CHECK(!t.isChunkOffsetOk(16));
	}

	// stepToNextOwnChunk: when current_chunk_.off_ != 0, returns the delta to
	// the next multiple of chunk_distance_gcd_ relative to the last chunk offset.
	{
		Track t("dummy");
		t.chunk_distance_gcd_ = 64;
		t.mdat_content_start_ = 0;
		t.current_chunk_.off_ = 64;

		// off=100: (100-64)%64=36, step=64-36=28
		CHECK_EQ(t.stepToNextOwnChunk(100), (int64_t)28);
		// off=127: (127-64)%64=63, step=1
		CHECK_EQ(t.stepToNextOwnChunk(127), (int64_t)1);
		// off=128: (128-64)%64=0, step=64 (already aligned, skips to next boundary)
		CHECK_EQ(t.stepToNextOwnChunk(128), (int64_t)64);
	}

	// stepToNextOwnChunkAbs: returns the step to the next multiple of start_off_gcd_
	// from (mdat_content_start_ + off). Returns 0 when start_off_gcd_ <= 1.
	{
		Track t("dummy");
		t.start_off_gcd_ = 32;
		t.mdat_content_start_ = 0;

		// off=40: abs=40, step=(32 - 40%32) % 32 = (32-8)%32 = 24
		CHECK_EQ(t.stepToNextOwnChunkAbs(40), (int64_t)24);
		// off=64: abs=64, aligned, step=(32-0)%32=0
		CHECK_EQ(t.stepToNextOwnChunkAbs(64), (int64_t)0);

		// start_off_gcd_<=1 always returns 0 regardless of offset
		t.start_off_gcd_ = 1;
		CHECK_EQ(t.stepToNextOwnChunkAbs(40), (int64_t)0);
	}

	// finalizeCurrentChunk: appends current_chunk_ to chunks_, resets n_samples_
	// and size_, and preserves off_ (needed by stepToNextChunkOff).
	{
		Track t("dummy");
		t.is_dummy_ = false;
		t.current_chunk_.off_ = 200;
		t.current_chunk_.n_samples_ = 3;
		t.current_chunk_.size_ = 150;

		t.finalizeCurrentChunk();

		CHECK_EQ((int)t.chunks_.size(), 1);
		CHECK_EQ(t.chunks_[0].off_, (off_t)200);
		CHECK_EQ(t.chunks_[0].n_samples_, 3);
		CHECK_EQ(t.chunks_[0].size_, (int64_t)150);
		CHECK_EQ(t.current_chunk_.n_samples_, 0);
		CHECK_EQ(t.current_chunk_.size_, (int64_t)0);
		CHECK_EQ(t.current_chunk_.off_, (off_t)200); // preserved
	}

	// finalizeCurrentChunk: dummy track with n_samples_==0 returns early without
	// appending, even when size_ > 0. This is the invariant that makes
	// Mp4Repairer::pushBackLastChunk correct: addUnknownSequence must only be
	// called when n_samples_ > 0, so the size_>0 guard in pushBackLastChunk must
	// include n_samples_ to match the early-return order in the original code.
	{
		Track t("free"); // dummy constructor sets is_dummy_=true
		t.current_chunk_.size_ = 512;
		// n_samples_ defaults to 0
		t.finalizeCurrentChunk();
		CHECK_EQ((int)t.chunks_.size(), 0);
		CHECK_EQ(t.current_chunk_.size_, (int64_t)512); // size_ unchanged: early return fired
	}

	// genPatternPerm: higher-entropy patterns are sorted to the front of
	// dyn_patterns_perm_. Uses 6-distinct-byte vs all-same-byte patterns.
	{
		Track t("dummy");
		// dyn_patterns_[0]: 6 distinct bytes -> entropy = log2(6) ~= 2.58 (high)
		std::vector<uchar> hi_a = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
		std::vector<uchar> hi_b = hi_a;
		// dyn_patterns_[1]: all same bytes -> entropy = 0 (low)
		std::vector<uchar> lo_a = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
		std::vector<uchar> lo_b = lo_a;

		t.dyn_patterns_.resize(2);
		t.dyn_patterns_[0].emplace_back(hi_a, hi_b);
		t.dyn_patterns_[1].emplace_back(lo_a, lo_b);

		t.genPatternPerm(-1); // twos_track_idx=-1: no twos fallback

		CHECK_EQ((int)t.dyn_patterns_perm_.size(), 2);
		CHECK_EQ((int)t.dyn_patterns_perm_[0], 0); // high entropy first
		CHECK_EQ((int)t.dyn_patterns_perm_[1], 1);
		CHECK_EQ(t.use_looks_like_twos_idx_, -1); // not set when no twos track
	}

	// genPatternPerm: use_looks_like_twos_idx_ is set to the position in sorted
	// perm where the first pattern with entropy < 2 appears, when twos_track_idx
	// matches a valid dyn_patterns_ entry.
	{
		Track t("dummy");
		// dyn_patterns_[0]: high entropy (> 2), not the twos track
		std::vector<uchar> hi_a = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
		std::vector<uchar> hi_b = hi_a;
		// dyn_patterns_[1]: low entropy (= 0, < 2), this is the twos track
		std::vector<uchar> lo_a = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
		std::vector<uchar> lo_b = lo_a;

		t.dyn_patterns_.resize(2);
		t.dyn_patterns_[0].emplace_back(hi_a, hi_b);
		t.dyn_patterns_[1].emplace_back(lo_a, lo_b);

		t.genPatternPerm(1); // twos_track_idx=1

		// Sorted perm: [0 (hi), 1 (lo)]. At perm[1] entropy=0 < 2 -> idx set to 1.
		CHECK_EQ(t.use_looks_like_twos_idx_, 1);
	}
}
