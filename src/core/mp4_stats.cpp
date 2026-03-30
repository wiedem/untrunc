// Dynamic statistics generation for Mp4 (chunk transitions, patterns, track order)

#include <iostream>

#include "mp4.h"
#include "io/file.h"
#include "track/track_order.h"

using namespace std;

void Mp4::genChunks() {
	for (auto &t : tracks_)
		t.genChunkSizes();
}

void Mp4::resetChunkTransitions() {
	tracks_.pop_back();
	idx_free_ = kNoFreeTrack;
	chunk_transitions_.clear();
	free_seqs_.clear();

	for (auto &t : tracks_) {
		t.pad_after_chunk_ = -1;
		t.last_pad_after_chunk_ = -1;
	}
}

void Mp4::genChunkTransitions() {
	ChunkIt::Chunk last_chunk;

	tracks_.emplace_back("free");
	idx_free_ = tracks_.size() - 1;
	logg(V, "created dummy track 'free'\n");

	for (auto &cur_chunk : AllChunksIn(this, false)) {
		auto track_idx = cur_chunk.track_idx_;
		auto off = cur_chunk.off_;

		if (!cur_chunk.should_ignore_) {
			if (ctx_.scan_.first_off_abs_ < 0) {
				ctx_.scan_.first_off_abs_ = cur_chunk.off_;
				ctx_.scan_.first_off_rel_ = ctx_.scan_.first_off_abs_ - ctx_.file_.mdat_->contentStart();
				ctx_.scan_.orig_first_track_ = &tracks_[track_idx];
			}

			if (last_chunk) {
				auto last_end = last_chunk.off_ + last_chunk.size_;
				if (off - ctx_.file_.mdat_->contentStart() < pat_size_ / 2) {
					logg(W, "rel_off(cur_chunk) < pat_size/2 .. ", tracks_[track_idx].codec_.name_, " ", cur_chunk,
					     "\n");
				} else {
					assertt(off >= last_end);
					if (off != last_end) {
						auto relOff = last_end - ctx_.file_.mdat_->contentStart();
						auto sz = off - last_end;

						chunk_transitions_[{last_chunk.track_idx_, idx_free_}].emplace_back(last_end);
						chunk_transitions_[{idx_free_, track_idx}].emplace_back(off);

						if (ctx_.scan_.use_offset_map_) {
							off_to_chunk_[relOff] = Mp4::Chunk(relOff, sz, idx_free_, 1);
						}
						tracks_[idx_free_].chunks_.emplace_back(last_end, sz, off - last_end);
						free_seqs_.emplace_back(FreeSeq{relOff, sz, last_chunk.track_idx_, last_chunk.size_,
						                                getCodecName(last_chunk.track_idx_)});

						tracks_[last_chunk.track_idx_].adjustPadAfterChunk(sz);
					} else {
						chunk_transitions_[{last_chunk.track_idx_, track_idx}].emplace_back(off);
						tracks_[last_chunk.track_idx_].adjustPadAfterChunk(0);
					}
				}
			}
		}

		last_chunk = cur_chunk;
	}

	if (!tracks_.back().chunks_.size()) {
		tracks_.pop_back();
		idx_free_ = kNoFreeTrack;
		logg(V, "removed dummy track 'free'\n");
	}

	afterTrackRealloc();
}

struct TrackGcdInfo {
	int prev_pkt_idx = -1;
	int combined_size_gcd = -1;
	int cnt = 0;

	void adjustGcd(size_t combined_sz) {
		cnt++;
		if (combined_size_gcd == -1) {
			combined_size_gcd = combined_sz;
		} else {
			combined_size_gcd = gcd(combined_size_gcd, combined_sz);
		}
	}
};

// Find tracks that seem to pad their packet's sizes (e.g. always n*8)
void Mp4::collectPktGcdInfo(map<int, TrackGcdInfo> &track_to_info) {
	ChunkIt::Chunk last_real;
	int last_padding = 0;

	for (auto &cur_chunk : AllChunksIn(this, false, false)) {
		auto track_idx = cur_chunk.track_idx_;

		if (track_idx == idx_free_) {
			last_padding = cur_chunk.size_;
			continue;
		}

		auto &t = tracks_[cur_chunk.track_idx_];
		if (t.isChunkTrack()) {
			last_real = cur_chunk;
			continue;
		}

		auto &info = track_to_info[cur_chunk.track_idx_];

		if (last_real.track_idx_ == cur_chunk.track_idx_ && last_padding) {
			size_t combined_sz = t.getSize(info.prev_pkt_idx++) + last_padding;
			info.adjustGcd(combined_sz);
		} else {
			info.prev_pkt_idx++;
		}

		last_padding = 0;

		for (int i = 0; i < cur_chunk.n_samples_ - 1; i++) {
			info.adjustGcd(t.getSize(info.prev_pkt_idx++));
		}

		last_real = cur_chunk;
	}
}

void Mp4::analyzeFree() {
	if (idx_free_ == kNoFreeTrack) return;

	map<int, TrackGcdInfo> track_to_info;
	collectPktGcdInfo(track_to_info);

	bool doneMerge = false;
	for (const auto &[idx, info] : track_to_info) {
		if (info.cnt < 3) continue;
		if (info.combined_size_gcd <= 1) continue;
		logg(V, "found pkt_sz_gcd_: ", getCodecName(idx), " ", info.combined_size_gcd, "\n");
		tracks_[idx].pkt_sz_gcd_ = info.combined_size_gcd;
		tracks_[idx].mergeChunks();
		doneMerge = true;
	}

	if (doneMerge) {
		resetChunkTransitions();
		genChunkTransitions();
	}
}

void Mp4::genTrackOrder() {
	if (!ctx_.file_.mdat_) findMdat(*ctx_.file_.file_);

	vector<pair<int, int>> order;
	for (auto &cur_chunk : AllChunksIn(this, true)) {
		auto track_idx = cur_chunk.track_idx_;
		if (order.size() > 100) break;
		order.emplace_back(make_pair(track_idx, cur_chunk.n_samples_));
	}

	ctx_.order_.track_order_simple_ = findOrderSimple(order);

	if (g_options.log_mode >= LogMode::V) {
		std::string line = ss("order ( ", order.size(), "):");
		for (auto &p : order)
			line += ss(" (", p.first, ", ", p.second, ")");
		logg(V, line, "\n");
	}
	if (findOrder(order)) ctx_.order_.track_order_ = order;

	for (auto &[i, n_samples] : order) {
		ctx_.order_.cycle_size_ += tracks_[i].ss_stats_.averageSize() * n_samples;
	}
}

buffs_t Mp4::offsToBuffs(const offs_t &offs, const string &load_prefix) {
	int cnt = 0;
	buffs_t buffs;
	for (auto off : offs) {
		if (g_options.log_mode == I) outProgress(cnt++, offs.size(), load_prefix);
		auto buff = ctx_.file_.file_->getFragment(off - pat_size_ / 2, pat_size_);
		buffs.emplace_back(buff, buff + pat_size_);
	}
	if (g_options.log_mode == I) cout << string(20, ' ') << '\r';
	return buffs;
}

patterns_t Mp4::offsToPatterns(const offs_t &all_offs, const string &load_prefix) {
	auto offs_to_consider = choose100(all_offs);
	auto buffs = offsToBuffs(offs_to_consider, load_prefix);
	auto patterns = genRawPatterns(buffs);

	auto offs_to_check = choose100(all_offs);
	auto buffs2 = offsToBuffs(offs_to_check, load_prefix);
	countPatternsSuccess(patterns, buffs2);

	//	for (auto& p : patterns) cout << p.successRate() << " " << p << '\n';

	filterBySuccessRate(patterns, load_prefix);
	return patterns;
}

void Mp4::genDynPatterns() {
	for (auto &t : tracks_)
		t.dyn_patterns_.resize(tracks_.size());

	for (auto const &kv : chunk_transitions_) {
		auto &patterns = tracks_[kv.first.first].dyn_patterns_[kv.first.second];
		string prefix = ss(kv.first.first, "->", kv.first.second, ": ");

		patterns = offsToPatterns(kv.second, prefix);
	}

	for (auto &t : tracks_) {
		t.genPatternPerm();
		ctx_.dummy_.has_zero_transitions_ = ctx_.dummy_.has_zero_transitions_ || t.hasZeroTransitions();
	}
	logg(V, "ctx_.dummy_.has_zero_transitions_: ", ctx_.dummy_.has_zero_transitions_, '\n');
	ctx_.order_.using_dyn_patterns_ = true;
}

void Mp4::genLikelyAll() {
	for (auto &t : tracks_)
		t.genLikely();
}

void Mp4::genDynStats(bool /*force_patterns*/) {
	if (chunk_transitions_.size()) return; // already generated
	if (!ctx_.file_.mdat_) findMdat(*ctx_.file_.file_);
	genChunks();

	genChunkTransitions();
	analyzeFree();

	genTrackOrder();
	genLikelyAll();

	genDynPatterns();
	setHasUnclearTransition();

	setDummyIsSkippable();
}

std::ostream &operator<<(std::ostream &out, const FreeSeq &x) {
	return out << "{" << x.codec_name << "_free "
	           << "at " << hexIf(x.offset) << " "
	           << "sz=" << x.sz << " "
	           << "last_chunk_sz=" << x.last_chunk_sz << "}";
}

vector<FreeSeq> Mp4::chooseFreeSeqs() {
	map<int, int> last_transition;
	for (int i = 0; i < (int)free_seqs_.size(); i++) {
		auto &x = free_seqs_[i];
		last_transition[x.prev_track_idx] = i;
	}

	vector<int> idxsToDel;
	for (const auto &pair : last_transition) {
		idxsToDel.push_back(pair.second);
	}
	sort(idxsToDel.begin(), idxsToDel.end(), std::greater<int>());
	dbgg("", vecToStr(idxsToDel));

	for (int idx : idxsToDel) {
		dbgg("Removing free_seq ..", idx, free_seqs_[idx]);
		free_seqs_.erase(free_seqs_.begin() + idx);
	}

	return choose100(free_seqs_);
}

bool Mp4::canSkipFree() {
	vector<FreeSeq> free_seqs = chooseFreeSeqs();
	logg(V, "chooseFreeSeqs:  ", free_seqs.size(), '\n');

	for (auto &free_seq : free_seqs) {
		off_t off = free_seq.offset, off_end = free_seq.offset + free_seq.sz;
		string last_track_name = "<start>";
		vector<off_t> possibleOffs;
		if (free_seq.prev_track_idx >= 0) {
			auto &t = tracks_[free_seq.prev_track_idx];
			last_track_name = t.codec_.name_;
			auto to_pad = t.alignPktLength(free_seq.last_chunk_sz) - free_seq.last_chunk_sz;
			logg(V, "to_pad:", to_pad, "\n");
			possibleOffs.emplace_back(off + to_pad);
			if (t.pad_after_chunk_) { // we dont know (here) if chunk was at end, so check both
				possibleOffs.emplace_back(off + to_pad + t.pad_after_chunk_);
			}
		} else {
			possibleOffs.emplace_back(off);
		}

		bool found_ok = false;
		for (auto off_i : possibleOffs) {
			if (off_i == off_end) {
				found_ok = true;
				continue;
			} // padding might was enough
			advanceOffset(off_i, true);
			if (off_i == off_end) {
				found_ok = true;
				continue;
			};
		}
		if (found_ok) continue;

		logg(V, "canSkipFree ", last_track_name, "_free at ", offToStr(free_seq.offset), " failed: ", off_end,
		     " not in ", vecToStr(possibleOffs), "\n");
		return false;
	}

	return true;
}

void Mp4::setDummyIsSkippable() {
	if (idx_free_ < 0) return;
	auto &t = tracks_[idx_free_];
	logg(V, "running setDummyIsSkippable() ... \n");

	ctx_.dummy_.is_skippable_ = false;
	if (canSkipFree()) {
		logg(V, "yes, via canSkipFree\n");
		ctx_.dummy_.is_skippable_ = true;
	}
	if (t.dummyIsUsedAsPadding()) {
		logg(V, "yes, by using padding-skip strategy\n");
		ctx_.dummy_.is_skippable_ = true;
		ctx_.dummy_.do_padding_skip_ = true;
	}

	if (!ctx_.dummy_.is_skippable_) {
		logg(V, "no, seems not to be skippable\n");
	}
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

void Mp4::setHasUnclearTransition() {
	for (int i = 0; i < (int)tracks_.size(); i++) {
		auto &t = tracks_[i];
		t.has_unclear_transition_.clear();
		for (int j = 0; j < (int)tracks_.size(); j++) {
			t.has_unclear_transition_.push_back(calcTransitionIsUnclear(i, j));
		}
	}
}

bool Mp4::setDuplicateInfo() {
	map<string, int> cnt;
	bool foundAny = false;
	for (auto &t : tracks_) {
		if (!t.isSupported()) continue;
		cnt[t.codec_.name_]++;
	}
	for (auto &t : tracks_) {
		if (cnt[t.codec_.name_] > 1) {
			foundAny = true;
			t.has_duplicates_ = true;
			t.genLikely();
		}
	}
	return foundAny;
}

bool Mp4::isTrackOrderEnough() {
	auto isEnough = [&]() {
		if (!ctx_.order_.track_order_.size()) return false;
		for (auto &t : tracks_) {
			if (t.isSupported()) continue;
			if (t.is_dummy_ && ctx_.dummy_.is_skippable_) continue; // not included in ctx_.order_.track_order_
			if (!t.is_dummy_ && t.likely_sample_sizes_.size() == 1) continue;

			logg(W, "ctx_.order_.track_order_ found, but not sufficient\n");
			ctx_.order_.track_order_.clear();
			return false;
		}
		return true;
	};

	bool is_enough = isEnough();
	logg(V, "isTrackOrderEnough: ", is_enough, "  (sz=", ctx_.order_.track_order_.size(), ")\n");
	if (!is_enough && ctx_.order_.track_order_.size()) {
		// in theory this could probably still be used somehow..
		logg(W, "ctx_.order_.track_order_ found, but not sufficient\n");
		ctx_.order_.track_order_.clear();
	}
	return is_enough;
}

bool Mp4::needDynStats() {
	if (g_options.use_chunk_stats) return true;
	for (auto &t : tracks_) {
		if (!t.isSupported()) {
			logg(I, "unknown track '", t.codec_.name_, "' found -> fallback to dynamic stats\n");
			return true;
		}
		if (t.codec_.needsDynStatsForSizing()) {
			logg(I, "mp4a: FFmpeg decoder cannot report frame size -> enabling dynamic stats\n");
			return true;
		}
	}
	return false;
}
