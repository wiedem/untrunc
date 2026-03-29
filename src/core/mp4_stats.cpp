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
			if (ctx_.first_off_abs_ < 0) {
				ctx_.first_off_abs_ = cur_chunk.off_;
				ctx_.first_off_rel_ = ctx_.first_off_abs_ - ctx_.current_mdat_->contentStart();
				ctx_.orig_first_track_ = &tracks_[track_idx];
			}

			if (last_chunk) {
				auto last_end = last_chunk.off_ + last_chunk.size_;
				if (off - ctx_.current_mdat_->contentStart() < pat_size_ / 2) {
					logg(W, "rel_off(cur_chunk) < pat_size/2 .. ", tracks_[track_idx].codec_.name_, " ", cur_chunk,
					     "\n");
				} else {
					assertt(off >= last_end);
					if (off != last_end) {
						auto relOff = last_end - ctx_.current_mdat_->contentStart();
						auto sz = off - last_end;

						chunk_transitions_[{last_chunk.track_idx_, idx_free_}].emplace_back(last_end);
						chunk_transitions_[{idx_free_, track_idx}].emplace_back(off);

						if (ctx_.use_offset_map_) {
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
	if (!ctx_.current_mdat_) findMdat(*ctx_.current_file_);

	vector<pair<int, int>> order;
	for (auto &cur_chunk : AllChunksIn(this, true)) {
		auto track_idx = cur_chunk.track_idx_;
		if (order.size() > 100) break;
		order.emplace_back(make_pair(track_idx, cur_chunk.n_samples_));
	}

	ctx_.track_order_simple_ = findOrderSimple(order);

	if (g_options.log_mode >= LogMode::V) {
		std::string line = ss("order ( ", order.size(), "):");
		for (auto &p : order)
			line += ss(" (", p.first, ", ", p.second, ")");
		logg(V, line, "\n");
	}
	if (findOrder(order)) ctx_.track_order_ = order;

	for (auto &[i, n_samples] : order) {
		ctx_.cycle_size_ += tracks_[i].ss_stats_.averageSize() * n_samples;
	}
}

buffs_t Mp4::offsToBuffs(const offs_t &offs, const string &load_prefix) {
	int cnt = 0;
	buffs_t buffs;
	for (auto off : offs) {
		if (g_options.log_mode == I) outProgress(cnt++, offs.size(), load_prefix);
		auto buff = ctx_.current_file_->getFragment(off - pat_size_ / 2, pat_size_);
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
		ctx_.has_zero_transitions_ = ctx_.has_zero_transitions_ || t.hasZeroTransitions();
	}
	logg(V, "ctx_.has_zero_transitions_: ", ctx_.has_zero_transitions_, '\n');
	ctx_.using_dyn_patterns_ = true;
}

void Mp4::genLikelyAll() {
	for (auto &t : tracks_)
		t.genLikely();
}

void Mp4::genDynStats(bool /*force_patterns*/) {
	if (chunk_transitions_.size()) return; // already generated
	if (!ctx_.current_mdat_) findMdat(*ctx_.current_file_);
	genChunks();

	genChunkTransitions();
	analyzeFree();

	genTrackOrder();
	genLikelyAll();

	genDynPatterns();
	setHasUnclearTransition();

	setDummyIsSkippable();
}
