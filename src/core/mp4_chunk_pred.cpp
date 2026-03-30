// Chunk prediction, chunk ordering, and chunk index management.

#include <iostream>

#include "mp4.h"

using namespace std;

// Chunk index correction

void Mp4::correctChunkIdx(int track_idx) {
	assertt(track_idx >= 0 && track_idx != idx_free_);
	if (!ctx_.order_.track_order_.size()) {
		correctChunkIdxSimple(track_idx);
		return;
	}

	// TODO: call correctChunkIdx once we know n_samples
	//       ATM result could be wrong if 'chunk::likely_n_samples_.size() > 1'
	while (ctx_.order_.track_order_[ctx_.order_.next_chunk_idx_ % ctx_.order_.track_order_.size()].first != track_idx)
		ctx_.order_.next_chunk_idx_++;

	if (tracks_[track_idx].likely_n_samples_.size() > 1) logg(W, "correctChunkIdx(", track_idx, ") could be wrong\n");
}

void Mp4::correctChunkIdxSimple(int track_idx) {
	assertt(track_idx != idx_free_);
	auto order_sz = ctx_.order_.track_order_simple_.size();
	if (!order_sz) return;

	int off_ok = -1;
	for (uint off = 0; off < order_sz; off++) {
		if (ctx_.order_.track_order_simple_[(ctx_.order_.next_chunk_idx_ + off) % order_sz] == track_idx) {
			if (off_ok < 0)
				off_ok = off;
			else {
				logg(W, "correctChunkIdxSimple(", track_idx, "): next chunk is ambiguous\n");
				break;
			};
		}
	}

	assertt(off_ok >= 0);

	if (off_ok) {
		logg(V, "correctChunkIdxSimple(", track_idx, "): skipping ", off_ok, "chunks\n\n");
		ctx_.order_.next_chunk_idx_ += off_ok;
	}
}

void Mp4::onFirstChunkFound(int track_idx) {
	if (track_idx == idx_free_) return;
	ctx_.scan_.first_chunk_found_ = true;
	assertt(ctx_.order_.next_chunk_idx_ == 0);
	correctChunkIdx(track_idx);
	if (ctx_.order_.next_chunk_idx_)
		logg(W, "different start chunk: ", track_idx, " instead of ", ctx_.order_.track_order_simple_[0], "\n");
}

// currently only used to distinguish duplicate tracks (e.g. 2x mp4a)
bool Mp4::isExpectedTrackIdx(int i) {
	if (ctx_.order_.track_order_simple_.empty()) return true;
	int expected_idx =
	    ctx_.order_.track_order_simple_[ctx_.order_.next_chunk_idx_ % ctx_.order_.track_order_simple_.size()];
	if (expected_idx == i) return true;

	if (getCodecName(i) != getCodecName(expected_idx)) {
		logg(W, "expected codec ", getCodecName(expected_idx), " but found ", getCodecName(i), "\n");
		correctChunkIdx(i);
		return true;
	}
	return false;
}

// Chunk prediction

int Mp4::skipNextZeroCave(off_t off, int max_sz, int n_zeros) {
	// skip to cave + skip over cave

	for (auto pos = off; max_sz > 0; pos += n_zeros, max_sz -= n_zeros)
		if (isAllZerosAt(pos, n_zeros))
			for (; max_sz > 0; pos += 1, max_sz -= 1)
				if (!isAllZerosAt(pos, 1)) return pos - off;
	return -1;
}

Mp4::Chunk Mp4::fitChunk(off_t offset, uint track_idx_to_fit, uint known_n_samples) {
	Mp4::Chunk c;
	auto &t = tracks_[track_idx_to_fit];
	if (!t.hasPredictableChunks()) return c;

	for (auto n_samples : t.likely_n_samples_) {
		if (known_n_samples) n_samples = known_n_samples;
		for (auto s_sz : t.likely_sample_sizes_) {
			auto dst_off = offset + n_samples * s_sz + t.pad_after_chunk_;
			if (dst_off < mdatContentSize() &&
			    wouldMatch(WMCfg{dst_off, t.codec_.name_, false, (int)track_idx_to_fit})) {
				assertt(n_samples > 0);
				c = Chunk(offset, n_samples, track_idx_to_fit, s_sz);
				return c;
			}
		}
		if (known_n_samples) break;
	}

	// VBR fallback: when no CBR candidate lands on a chunk boundary, scan the
	// range [lo, hi] byte-by-byte for the next chunk start of any OTHER track.
	// The current track's codec is skipped to avoid false positives from its
	// permissive matchSample (e.g. mp4a matches any non-zero byte).
	// Only tried when there are multiple known sample sizes (VBR audio).
	if (t.likely_sample_sizes_.size() > 1 && !t.likely_n_samples_.empty()) {
		const auto &szs = t.likely_sample_sizes_;
		int pad = max(0, t.pad_after_chunk_);
		// Guard against corrupted reference data: a non-positive front value would
		// produce a huge lo_per_sample via uint cast and blow up the scan range.
		if (szs.front() <= 0) return c;
		// Extend the scan range by ±10% around the reference file's min/max sample
		// sizes to handle broken files with different encoding complexity.
		// VBR audio sample sizes depend on content (not just codec), so the broken
		// file may have slightly different sizes than the reference.
		uint lo_per_sample = (uint)(szs.front() * 9 / 10);
		uint hi_per_sample = (uint)(szs.back() * 11 / 10);
		for (auto n_samples : t.likely_n_samples_) {
			if (known_n_samples) n_samples = known_n_samples;
			off_t lo = offset + (off_t)n_samples * lo_per_sample + pad;
			off_t hi = offset + (off_t)n_samples * hi_per_sample + pad;
			for (off_t try_off = lo; try_off <= hi && try_off < (off_t)mdatContentSize(); try_off++) {
				// Skip the fitted track's codec so its permissive matchSample cannot
				// cause a false positive (would match the audio data itself).
				if (!wouldMatch(WMCfg{try_off, t.codec_.name_, false, (int)track_idx_to_fit})) continue;
				int64_t chunk_bytes = try_off - offset - pad;
				if (chunk_bytes <= 0) continue;
				// size_ holds the exact boundary. sample_size_ is the integer-average
				// per sample: n_samples * sample_size_ may be up to n_samples-1 bytes
				// less than size_. addChunk() advances per-frame by sample_size_ and
				// does not use size_ for offset arithmetic, so the exact chunk boundary
				// is preserved for subsequent scan positioning.
				c.off_ = offset;
				c.size_ = chunk_bytes;
				c.n_samples_ = n_samples;
				c.track_idx_ = (int)track_idx_to_fit;
				c.sample_size_ = (int)(chunk_bytes / n_samples);
				logg(V, "fitChunk VBR: n=", n_samples, ", avg_sz=", c.sample_size_, ", boundary=", try_off, "\n");
				return c;
			}
			if (known_n_samples) break;
		}
	}

	return c;
}

int Mp4::getLikelyNextTrackIdx(int *n_samples) {
	auto p = ctx_.order_.track_order_[ctx_.order_.next_chunk_idx_ % ctx_.order_.track_order_.size()];
	if (n_samples) *n_samples = p.second;
	return p.first;
}

int Mp4::calcFallbackTrackIdx() {
	if (!ctx_.order_.using_dyn_patterns_) return -1;
	for (uint i = 0; i < tracks_.size(); i++) {
		auto &t = tracks_[i];
		if (!t.isChunkTrack()) continue;

		for (auto &t2 : tracks_) {
			if (t2.dyn_patterns_[i].size()) continue;
		}

		return i;
	}
	return -1;
}

// returns true in case we are sure
bool Mp4::predictChunkViaOrder(off_t offset, Mp4::Chunk &c) {
	if (!ctx_.order_.track_order_.size()) return false;
	if (amInFreeSequence()) return false;
	if (!currentChunkFinished()) {
		if (!nearEnd(offset)) {
			dbgg("ignoring track_order since !currentChunkFinished");
			return false;
		}
		dbgg("using ctx_.order_.track_order_ despite !currentChunkFinished since nearEnd", offset);
	}

	int n_samples;
	int track_idx = getLikelyNextTrackIdx(&n_samples);
	auto &track = tracks_[track_idx];
	if (!track.hasPredictableChunks()) {
		logg(V, "Next track '", getCodecName(track_idx), "' has unpredictable chunks\n");
		return true;
	}

	if (track.likely_sample_sizes_.size() > 1) {
		c = fitChunk(offset, track_idx, n_samples);
		if (!c) {
			// The broken file may have different n_samples per chunk than the reference,
			// so the reference-predicted n_samples can produce the wrong scan range.
			// Fall back to trying all likely_n_samples_ values.
			logg(V, "fitChunk failed with reference n_samples=", n_samples, ", retrying with all n_samples\n");
			c = fitChunk(offset, track_idx, 0);
		}
		if (!c)
			logg(V, "fitChunk() failed despite supposedly known (track_idx, n_samples) = ", track_idx, ", ", n_samples,
			     "\n");
	} else {
		auto s_sz = track.likely_sample_sizes_[0];
		if (n_samples * s_sz <= mdatContentSize() - offset) {
			c = Chunk(offset, n_samples, track_idx, s_sz);
			logg(V, "chunk derived from ctx_.order_.track_order_: ", c, "\n");
		}
	}

	return true;
}

// This tries to skip audio noise at the end (e.g. due to changed padding)
bool Mp4::chunkStartLooksInvalid(off_t offset, const Mp4::Chunk &c) {
	auto &t = tracks_[c.track_idx_];

	if (t.unpredictable_start_cnt_) return false;

	if (ctx_.scan_.last_track_idx_ == -1) { // very first
		if (!anyPatternMatchesHalf(offset, c.track_idx_)) {
			dbgg("anyPatternMatchesHalf does not agree", c.track_idx_);
		}
		return false;
	}

	auto track_idx = getNextTrackViaDynPatterns(offset);
	if (track_idx >= 0) {
		if (track_idx != c.track_idx_) {
			dbgg("getNextTrackViaDynPatterns would predict different track", track_idx, c.track_idx_);
			return true;
		}
		t.predictable_start_cnt_++;
	} else {
		dbgg("getNextTrackViaDynPatterns does not agree", offset, c, t.predictable_start_cnt_);
		if (t.predictable_start_cnt_ > 5 && nearEnd(offset)) {
			return true;
		} else {
			t.unpredictable_start_cnt_++;
		}
	}
	return false;
}

Mp4::Chunk Mp4::getChunkPrediction(off_t offset, bool only_perfect_fit) {
	logg(V, "called getChunkPrediction(", offToStr(offset), ") ... \n");
	Mp4::Chunk c;
	if (ctx_.scan_.last_track_idx_ == kNoFreeTrack) {
		logg(V, "Skipping chunk prediction: ctx_.scan_.last_track_idx_ == kNoFreeTrack\n");
		return c; // could try all instead..
	}

	int track_idx;
	if (predictChunkViaOrder(offset, c)) {
		if (c && chunkStartLooksInvalid(offset, c)) {
			dbgg("Ignoring predictChunkViaOrder, chunkStartLooksInvalid fails", c);
			ctx_.scan_.ignored_chunk_order_ = true;
			return Mp4::Chunk();
		}
		return c;
	}

	if (ctx_.scan_.last_track_idx_ == -1) { // very first offset
		track_idx = getTrackIdx(ctx_.scan_.orig_first_track_->codec_.name_);
		logg(V, "orig_trak: ", ctx_.scan_.orig_first_track_->codec_.name_, " ", track_idx, '\n');
		if (ctx_.scan_.orig_first_track_->codec_.name_ == "priv") { // FC7203 adds 241 zeros after second thumbnail
			logg(V, "using skipNextZeroCave .. \n");
			int sz = skipNextZeroCave(offset, 1 << 21, 12) - 1;
			logg(V, "skipNextZeroCave(", offset, ") -> ", sz, "\n");
			if (sz >= 0) return Chunk(offset, 1, track_idx, sz);
		}
		if (!anyPatternMatchesHalf(offset, track_idx)) {
			logg(V, "no half-pattern suggests orig_trak ..\n");
			if (ctx_.scan_.fallback_track_idx_ >= 0) {
				logg(V, "using fallback track\n");
				track_idx = ctx_.scan_.fallback_track_idx_;
			} else {
				return c;
			}
		}
	} else {
		track_idx = getNextTrackViaDynPatterns(offset);
	}

	if (track_idx >= 0 && !tracks_[track_idx].shouldUseChunkPrediction()) {
		logg(V, "should not use chunk prediction for '", getCodecName(track_idx), "'\n");
		return c;
	} else if (track_idx < 0) {
		//		logg(V, "no chunk with future found\n");
		logg(V, "found no plausible chunk-transition pattern-match\n");
		// Fall back to simple track order when dynamic pattern matching fails.
		// This handles cases where the broken file's codec content differs from
		// the reference, making content-specific byte patterns unreliable.
		if (!ctx_.order_.track_order_simple_.empty()) {
			int expected_track =
			    ctx_.order_.track_order_simple_[ctx_.order_.next_chunk_idx_ % ctx_.order_.track_order_simple_.size()];
			auto &expected_t = tracks_[expected_track];
			if (expected_t.shouldUseChunkPrediction()) {
				logg(V, "falling back to ctx_.order_.track_order_simple_ for '", getCodecName(expected_track), "'\n");
				c = fitChunk(offset, expected_track, 0);
			}
		}
		return c;
	}

	auto &t = tracks_[track_idx];
	logg(V, "transition pattern ", getCodecName(ctx_.scan_.last_track_idx_), "_", t.codec_.name_, " worked\n");

	if ((c = fitChunk(offset, track_idx))) {
		logg(V, "chunk found: ", c, "\n");
		return c;
	}

	if (only_perfect_fit) return c;

	// we boldly assume that track_idx was correct
	auto s_sz = t.likely_sample_sizes_[0];
	if (t.likely_n_samples_.empty()) {
		assertt(false);
		return c;
	}
	int n_samples = t.likely_n_samples_[0]; // smallest sample_size
	if (t.likely_n_samples_p < 0.9) {
		const int min_sz = 64; // min assumed chunk size
		int new_n_samples = max(1, min_sz / s_sz);
		logg(V, "reducing n_sample ", n_samples, " -> ", new_n_samples, " because unsure\n");
		n_samples = new_n_samples;
	}
	n_samples = min((uint64_t)n_samples, (uint64_t)(mdatContentSize() - offset) / s_sz);

	bool at_end = n_samples < t.likely_n_samples_[0];
	if (!at_end) {
		logg(W2, "found chunk has no future: ", c, " at ", offToStr(c.off_ + c.size_), "\n");
		return c;
	}

	c = Chunk(offset, n_samples, track_idx, s_sz);
	assertt(0 <= c.track_idx_ && to_size_t(c.track_idx_) < tracks_.size());
	return c;
}

bool Mp4::currentChunkIsDone() {
	return g_options.use_chunk_stats && ctx_.scan_.last_track_idx_ >= 0 &&
	       tracks_[ctx_.scan_.last_track_idx_].chunkProbablyAtAnd();
}

int Mp4::getChunkPadding(off_t &offset) {
	if (!currentChunkIsDone()) return 0;

	if (ctx_.scan_.last_track_idx_ != idx_free_ && tracks_[ctx_.scan_.last_track_idx_].pad_after_chunk_ > 0 &&
	    !ctx_.scan_.done_padding_after_) {
		logg(V, "Applying pad_after_chunk_ ", getCodecName(ctx_.scan_.last_track_idx_), " ",
		     tracks_[ctx_.scan_.last_track_idx_].pad_after_chunk_, "\n");
		ctx_.scan_.done_padding_after_ = true;
		return tracks_[ctx_.scan_.last_track_idx_].pad_after_chunk_;
	}

	if (ctx_.order_.track_order_.size()) {
		int idx = getLikelyNextTrackIdx();
		int step = tracks_[idx].stepToNextOwnChunkAbs(offset);
		logg(V, "used stepToNextOwnChunkAbs of likelyNextTrack -> ", step, "\n");
		if (step > 4) {
			return step;
		}
	}

	return 0;
}

bool Mp4::currentChunkFinished(int add_extra) {
	if (ctx_.order_.next_chunk_idx_ == 0) return true;
	auto [cur_track_idx, expected_ns] =
	    ctx_.order_.track_order_[(ctx_.order_.next_chunk_idx_ - 1) % ctx_.order_.track_order_.size()];
	assertt(cur_track_idx == ctx_.scan_.last_track_idx_, cur_track_idx, getCodecName(cur_track_idx),
	        ctx_.scan_.last_track_idx_, getCodecName(ctx_.scan_.last_track_idx_));
	auto &t = tracks_[cur_track_idx];
	if (t.current_chunk_.n_samples_ + add_extra < expected_ns) {
		dbgg("current_chunk not finished", ctx_.order_.next_chunk_idx_, t.codec_.name_, t.current_chunk_.n_samples_,
		     expected_ns);
		return false;
	}
	return true;
}

int Mp4::getNextTrackViaDynPatterns(off_t offset) {
	int last_idx = ctx_.scan_.done_padding_ && idx_free_ >= 0 ? idx_free_ : ctx_.scan_.last_track_idx_;
	auto track_idx = useDynPatternsForTrack(last_idx, offset);
	logg(V, "'", getCodecName(last_idx), "'.useDynPatterns() -> track_idx=", track_idx, "\n");
	return track_idx;
}
