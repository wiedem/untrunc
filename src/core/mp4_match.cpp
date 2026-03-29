// Sample/frame matching methods for Mp4.

#include <iostream>
#include <iomanip>

#include "mp4.h"

using namespace std;

bool Mp4::pointsToZeros(off_t off) {
	if (ctx_.current_mdat_->contentSize() - off < 4) return false;
	auto buff = ctx_.current_mdat_->getFragment(off, 4);
	int tmp;
	memcpy(&tmp, buff, sizeof(tmp));
	if (tmp == 0) {
		logg(V, "pointsToZeros: found 4 zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}

bool Mp4::shouldBeStrict(off_t off, int track_idx) {
	auto &t = tracks_[track_idx];
	if (!ctx_.unknown_length_ || !t.isSupported()) return false;
	if (!g_options.use_chunk_stats || !tracks_.back().is_dummy_ || !t.chunkProbablyAtAnd() || !t.isChunkOffsetOk(off))
		return true;

	assertt(ctx_.last_track_idx_ == idx_free_);
	auto buff = getBuffAround(off, Mp4::pat_size_);
	if (!buff) return true;

	for (auto &p : tracks_.back().dyn_patterns_[track_idx]) {
		if (p.doesMatch(buff)) {
			logg(V, "won't be strict since a free_", t.codec_.name_, " pattern matched at ", off, "\n");
			return false;
		}
	}
	return true;
}

bool Mp4::wouldMatch(const WouldMatchCfg &cfg) {
	auto chkChunkOffOk = [&](uint i) -> bool {
		if (g_options.use_chunk_stats && to_uint(cfg.last_track_idx) != i && !tracks_[i].isChunkOffsetOk(cfg.offset)) {
			logg(V, "would match, but offset is not accepted as start-of-chunk by '", tracks_[i].codec_.name_, "'\n");
			return false;
		} else
			logg(V, "chunkOffOk: ", cfg.offset, " ok for ", tracks_[i].codec_.name_, "\n");
		return true;
	};

	auto start = loadFragment(cfg.offset);
	for (uint i = 0; i < tracks_.size(); i++) {
		auto &c = tracks_[i].codec_;
		if (cfg.very_first && ctx_.orig_first_track_->codec_.name_ != c.name_) continue;
		bool be_strict = cfg.force_strict || shouldBeStrict(cfg.offset, i);
		if (tracks_[i].codec_.name_ == cfg.skip) continue;
		if (be_strict && !c.matchSampleStrict(start)) continue;
		if (!be_strict && !c.matchSample(start)) continue;

		logg(V, "wouldMatch(", cfg, ") -> yes, ", c.name_, "\n");
		return chkChunkOffOk(i);
	}

	if (g_options.use_chunk_stats) {
		auto last_idx = cfg.last_track_idx >= 0 ? cfg.last_track_idx : ctx_.last_track_idx_;
		if (cfg.very_first) { // very first offset
			assertt(ctx_.last_track_idx_ == -1);
			bool r = anyPatternMatchesHalf(cfg.offset, getTrackIdx(ctx_.orig_first_track_->codec_.name_));
			logg(V, "wouldMatch(", cfg, ") -> ", r, "  // via anyPatternMatchesHalf(",
			     ctx_.orig_first_track_->codec_.name_, ")\n");
			return r;
		}

		if (last_idx < 0) {
			logg(V, "wouldMatch(", cfg, ") -> no  // last_idx (", last_idx, ") < 0\n");
			return false;
		}
		if (wouldMatchDyn(cfg.offset, last_idx)) {
			logg(V, "wouldMatch(", cfg, ") -> yes\n");
			return true;
		}
	}

	logg(V, "wouldMatch(", cfg, ") -> no\n");
	return false;
}

bool Mp4::wouldMatch2(const uchar *start) {
	for (auto &t : tracks_)
		if (t.codec_.matchSample(start)) return true;
	return false;
}

FrameInfo Mp4::predictSize(const uchar *start, int track_idx, off_t offset) {
	auto &track = tracks_[track_idx];
	Codec &c = track.codec_;
	// logg(V, "Track codec: ", c.name_, '\n');

	uint length;
	if (track.constant_size_ > 0) {
		length = track.constant_size_;
	} else {
		int length_signed = c.getSize(start, ctx_.current_maxlength_, offset);
		length = to_uint(length_signed);

		logg(V, "part-length: ", length_signed, '\n');
		if (length_signed < 1) {
			logg(V, "Invalid length: part-length is ", length_signed, '\n');
			return FrameInfo();
		}
		if (length > ctx_.current_maxlength_) {
			logg(V, "limit: ", min(ctx_.max_part_size_, ctx_.current_maxlength_), "\n");
			logg(E, "Invalid length: ", length, " - too big (track: ", track_idx, ")\n");
			return FrameInfo();
		}
		if (length < 6 && c.name_ == "avc1") { // very short NALs are ok, short frames aren't
			logg(W2, "Invalid length: ", length, " - too small (track: ", track_idx, ")\n");
			return FrameInfo();
		}
		if (ctx_.unknown_length_ && track.ss_stats_.likelyTooSmall(length)) {
			return FrameInfo();
		}
	}

	if (c.was_bad_) {
		logg(W, "Codec::was_bad_ = 1\n");
		//			logg(V, "Codec::was_bad_ = 1 -> skipping\n");
		//			continue;
	}

	if (track.has_duplicates_ && !isExpectedTrackIdx(track_idx)) {
		return FrameInfo();
	}

	auto r = FrameInfo(track_idx, c, offset, length);

	if (c.name_ == "jpeg" && track.end_off_gcd_) {
		r.length_ += track.stepToNextOtherChunk(offset + length);
	} else if (track.pkt_sz_gcd_ > 1) {
		r.pad_afterwards_ = (track.pkt_sz_gcd_ - (length % track.pkt_sz_gcd_)) % track.pkt_sz_gcd_;
		logg(V, "r.pad_afterwards_: ", r.pad_afterwards_, "\n");
	}

	return r;
}

FrameInfo Mp4::getMatch(off_t offset, bool force_strict) {
	auto start = loadFragment(offset);

	// hardcoded match
	if (ctx_.pkt_idx_ == 4 && hasCodec("tmcd") && getTrack("tmcd").is_tmcd_hardcoded_) {
		if (!wouldMatch(WMCfg{offset, "", true, ctx_.last_track_idx_})) {
			logg(V, "using hardcoded 'tmcd' packet (len=4)\n");
			return FrameInfo(getTrackIdx("tmcd"), false, 0, offset, 4);
		} else {
			logg(W2, "hardcoded tmcd as 4th packet seems wrong..\n");
		}
	}

	for (uint i = 0; i < tracks_.size(); i++) {
		auto &track = tracks_[i];
		Codec &c = track.codec_;
		logg(V, "Track codec: ", c.name_, '\n');
		if (g_options.use_chunk_stats && (to_uint(ctx_.last_track_idx_) != i || ctx_.unknown_length_) &&
		    !track.isChunkOffsetOk(offset)) {
			logg(V, "offset not accepted as start-of-chunk offset by '", track.codec_.name_, "'\n");
			continue;
		}

		bool be_strict = force_strict || shouldBeStrict(offset, i);

		if (be_strict && !c.matchSampleStrict(start)) continue;
		if (!be_strict && !c.matchSample(start)) continue;

		auto m = predictSize(start, i, offset);
		if (!m) continue;
		return m;
	}

	return FrameInfo();
}

bool Mp4::calcTransitionIsUnclear(int track_idx_a, int track_idx_b) {
	auto &ta = tracks_[track_idx_a], &tb = tracks_[track_idx_b];
	if (tb.isSupported()) return false;
	if (ta.dyn_patterns_[track_idx_b].empty() &&                     // no clear pattern
	    chunk_transitions_[{track_idx_a, track_idx_b}].size() > 5) { // transition happens
		return true;
	}
	return false;
}

bool Mp4::wouldMatchDyn(off_t offset, int last_idx) {
	auto new_track = tracks_[last_idx].useDynPatterns(offset);
	if (new_track >= 0) {
		logg(V, "wouldMatchDyn(", offset, ", ", last_idx, ") -> yes (", getCodecName(last_idx), "_",
		     getCodecName(new_track), ")\n");
		return true;
	}
	logg(V, "wouldMatchDyn(", offToStr(offset), ", ", last_idx, ") -> no\n");
	return false;
}

bool Mp4::anyPatternMatchesHalf(off_t offset, uint track_idx_to_try) {
	auto buff = ctx_.current_mdat_->getFragment(offset, Mp4::pat_size_ / 2);
	if (!buff) return false;

	for (auto &t : tracks_) {
		for (auto &p : t.dyn_patterns_[track_idx_to_try]) {
			if (g_options.log_mode >= V) {
				cout << string(36, ' ');
				printBuffer(buff, Mp4::pat_size_);
				cout << p << '\n';
			}
			if (p.doesMatchHalf(buff)) return true;
		}
	}
	return false;
}
int64_t Mp4::calcStep(off_t offset) {
	if (idx_free_ > 0 && ctx_.dummy_do_padding_skip_) { // next chunk is probably padded with random data..
		int len = tracks_[idx_free_].stepToNextOtherChunk(offset);
		dbgg("calcStep: freeTrack.stepToNextOtherChunk:", offset, len);
		if (len) return len;
	}

	int64_t step = numeric_limits<int64_t>::max();
	if (g_options.use_chunk_stats) {
		step = numeric_limits<int64_t>::max();
		for (auto &t : tracks_) {
			if (t.is_dummy_) continue;
			step = min(step, t.stepToNextOwnChunk(offset));
		}

		step = min(step, ctx_.current_mdat_->contentSize() - offset);
	} else {
		step = Mp4::step_;
	}
	return step;
}

bool Mp4::isAllZerosAt(off_t off, int n) {
	if (ctx_.current_mdat_->contentSize() - off < n) return false;
	auto buff = ctx_.current_mdat_->getFragment(off, n);
	if (isAllZeros(buff, n)) {
		logg(V, "isAllZerosAt: found ", n, " zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}
