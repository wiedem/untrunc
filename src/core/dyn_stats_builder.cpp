// Implementation of DynStatsBuilder: dynamic statistics generation from the reference file.
// Produces chunk transitions, byte patterns, track order, and dummy-track properties.

#include "dyn_stats_builder.h"
#include "chunk_it.h"
#include "track/track_list.h"
#include "track/track_order.h"

#include <iostream>

using namespace std;

// File-local helpers

static off_t mdatEnd(const FileState &file) {
	return file.mdat_->start_ + file.mdat_->length_;
}

static string codecName(const vector<Track> &tracks, int i) {
	return string(TrackList::nameOf(tracks, i));
}

// TrackGcdInfo is an implementation detail of analyzeFree.
struct TrackGcdInfo {
	int prev_pkt_idx = -1;
	int combined_size_gcd = -1;
	int cnt = 0;

	void adjustGcd(size_t combined_sz) {
		cnt++;
		if (combined_size_gcd == -1) {
			combined_size_gcd = combined_sz;
		} else {
			combined_size_gcd = gcd(combined_size_gcd, (int)combined_sz);
		}
	}
};

// Find tracks that seem to pad their packet's sizes (e.g. always n*8)
static void collectPktGcdInfo(vector<Track> &tracks, int idx_free, off_t mdat_end,
                              map<int, TrackGcdInfo> &track_to_info) {
	ChunkIt::Chunk last_real;
	int last_padding = 0;

	for (auto &cur_chunk : AllChunksIn(tracks, mdat_end, false, false)) {
		auto track_idx = cur_chunk.track_idx_;

		if (track_idx == idx_free) {
			last_padding = cur_chunk.size_;
			continue;
		}

		auto &t = tracks[cur_chunk.track_idx_];
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

ostream &operator<<(ostream &out, const FreeSeq &x) {
	return out << "{" << x.codec_name << "_free "
	           << "at " << hexIf(x.offset) << " "
	           << "sz=" << x.sz << " "
	           << "last_chunk_sz=" << x.last_chunk_sz << "}";
}

// Constructor / generate

DynStatsBuilder::DynStatsBuilder(Config cfg) : cfg_(cfg) {}

void DynStatsBuilder::generate() {
	genChunks();
	genChunkTransitions();
	if (analyzeFree()) {
		resetChunkTransitions();
		genChunkTransitions();
	}
	cfg_.gen_track_order();
	genLikelyAll();
	genDynPatterns();
	setHasUnclearTransition();
	setDummyIsSkippable();
}

// Steps

void DynStatsBuilder::genChunks() {
	for (auto &t : cfg_.tracks)
		t.genChunkSizes();
}

void DynStatsBuilder::genChunkTransitions() {
	ChunkIt::Chunk last_chunk;
	auto &tracks = cfg_.tracks;
	auto &scan = cfg_.scan;
	auto &file = cfg_.file;

	tracks.emplace_back("free");
	cfg_.idx_free = tracks.size() - 1;
	logg(V, "created dummy track 'free'\n");

	for (auto &cur_chunk : AllChunksIn(tracks, mdatEnd(file), false)) {
		auto track_idx = cur_chunk.track_idx_;
		auto off = cur_chunk.off_;

		if (!cur_chunk.should_ignore_) {
			if (scan.first_off_abs_ < 0) {
				scan.first_off_abs_ = cur_chunk.off_;
				scan.first_off_rel_ = scan.first_off_abs_ - file.mdat_->contentStart();
				scan.orig_first_track_ = &tracks[track_idx];
			}

			if (last_chunk) {
				auto last_end = last_chunk.off_ + last_chunk.size_;
				if (off - file.mdat_->contentStart() < pat_size_ / 2) {
					logg(W, "rel_off(cur_chunk) < pat_size/2 .. ", tracks[track_idx].codec_.name_, " ", cur_chunk,
					     "\n");
				} else {
					assertt(off >= last_end);
					if (off != last_end) {
						auto relOff = last_end - file.mdat_->contentStart();
						auto sz = off - last_end;

						cfg_.chunk_transitions[{last_chunk.track_idx_, cfg_.idx_free}].emplace_back(last_end);
						cfg_.chunk_transitions[{cfg_.idx_free, track_idx}].emplace_back(off);

						tracks[cfg_.idx_free].chunks_.emplace_back(last_end, sz, off - last_end);
						cfg_.free_seqs.emplace_back(FreeSeq{relOff, sz, last_chunk.track_idx_, last_chunk.size_,
						                                    codecName(tracks, last_chunk.track_idx_)});

						tracks[last_chunk.track_idx_].adjustPadAfterChunk(sz);
					} else {
						cfg_.chunk_transitions[{last_chunk.track_idx_, track_idx}].emplace_back(off);
						tracks[last_chunk.track_idx_].adjustPadAfterChunk(0);
					}
				}
			}
		}

		last_chunk = cur_chunk;
	}

	if (!tracks.back().chunks_.size()) {
		tracks.pop_back();
		cfg_.idx_free = kNoFreeTrack;
		logg(V, "removed dummy track 'free'\n");
	}

	cfg_.after_track_realloc();
}

void DynStatsBuilder::resetChunkTransitions() {
	cfg_.tracks.pop_back();
	cfg_.idx_free = kNoFreeTrack;
	cfg_.chunk_transitions.clear();
	cfg_.free_seqs.clear();

	for (auto &t : cfg_.tracks) {
		t.pad_after_chunk_ = -1;
		t.last_pad_after_chunk_ = -1;
	}
}

bool DynStatsBuilder::analyzeFree() {
	if (cfg_.idx_free == kNoFreeTrack) return false;

	map<int, TrackGcdInfo> track_to_info;
	collectPktGcdInfo(cfg_.tracks, cfg_.idx_free, mdatEnd(cfg_.file), track_to_info);

	bool doneMerge = false;
	for (const auto &[idx, info] : track_to_info) {
		if (info.cnt < 3) continue;
		if (info.combined_size_gcd <= 1) continue;
		logg(V, "found pkt_sz_gcd_: ", codecName(cfg_.tracks, idx), " ", info.combined_size_gcd, "\n");
		cfg_.tracks[idx].pkt_sz_gcd_ = info.combined_size_gcd;
		cfg_.tracks[idx].mergeChunks();
		doneMerge = true;
	}

	return doneMerge;
}

void DynStatsBuilder::genLikelyAll() {
	for (auto &t : cfg_.tracks)
		t.genLikely();
}

void DynStatsBuilder::genDynPatterns() {
	auto &tracks = cfg_.tracks;

	for (auto &t : tracks)
		t.dyn_patterns_.resize(tracks.size());

	for (auto const &kv : cfg_.chunk_transitions) {
		auto &patterns = tracks[kv.first.first].dyn_patterns_[kv.first.second];
		string prefix = ss(kv.first.first, "->", kv.first.second, ": ");
		patterns = offsToPatterns(kv.second, prefix);
	}

	for (auto &t : tracks) {
		t.genPatternPerm(cfg_.twos_track_idx, [this](uint i) -> string { return codecName(cfg_.tracks, (int)i); });
		cfg_.dummy.has_zero_transitions_ = cfg_.dummy.has_zero_transitions_ || t.hasZeroTransitions();
	}
	logg(V, "cfg_.dummy.has_zero_transitions_: ", cfg_.dummy.has_zero_transitions_, '\n');
	cfg_.order.using_dyn_patterns_ = true;
}

void DynStatsBuilder::setHasUnclearTransition() {
	auto &tracks = cfg_.tracks;
	for (int i = 0; i < (int)tracks.size(); i++) {
		auto &t = tracks[i];
		t.has_unclear_transition_.clear();
		for (int j = 0; j < (int)tracks.size(); j++) {
			t.has_unclear_transition_.push_back(calcTransitionIsUnclear(i, j));
		}
	}
}

void DynStatsBuilder::setDummyIsSkippable() {
	if (cfg_.idx_free < 0) return;
	auto &t = cfg_.tracks[cfg_.idx_free];
	logg(V, "running setDummyIsSkippable() ... \n");

	cfg_.dummy.is_skippable_ = false;
	if (canSkipFree()) {
		logg(V, "yes, via canSkipFree\n");
		cfg_.dummy.is_skippable_ = true;
	}
	if (t.dummyIsUsedAsPadding()) {
		logg(V, "yes, by using padding-skip strategy\n");
		cfg_.dummy.is_skippable_ = true;
		cfg_.dummy.do_padding_skip_ = true;
	}

	if (!cfg_.dummy.is_skippable_) {
		logg(V, "no, seems not to be skippable\n");
	}
}

// Helpers

bool DynStatsBuilder::canSkipFree() {
	vector<FreeSeq> free_seqs = chooseFreeSeqs();
	logg(V, "chooseFreeSeqs:  ", free_seqs.size(), '\n');

	for (auto &free_seq : free_seqs) {
		off_t off = free_seq.offset, off_end = free_seq.offset + free_seq.sz;
		string last_track_name = "<start>";
		vector<off_t> possibleOffs;
		if (free_seq.prev_track_idx >= 0) {
			auto &t = cfg_.tracks[free_seq.prev_track_idx];
			last_track_name = t.codec_.name_;
			auto to_pad = t.alignPktLength(free_seq.last_chunk_sz) - free_seq.last_chunk_sz;
			logg(V, "to_pad:", to_pad, "\n");
			possibleOffs.emplace_back(off + to_pad);
			if (t.pad_after_chunk_) {
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
			}
			cfg_.advance_offset(off_i, true);
			if (off_i == off_end) {
				found_ok = true;
				continue;
			}
		}
		if (found_ok) continue;

		logg(V, "canSkipFree ", last_track_name, "_free at rel=", hexIf(free_seq.offset), " failed: ", off_end,
		     " not in ", vecToStr(possibleOffs), "\n");
		return false;
	}

	return true;
}

vector<FreeSeq> DynStatsBuilder::chooseFreeSeqs() {
	auto &free_seqs = cfg_.free_seqs;
	map<int, int> last_transition;
	for (int i = 0; i < (int)free_seqs.size(); i++) {
		auto &x = free_seqs[i];
		last_transition[x.prev_track_idx] = i;
	}

	vector<int> idxsToDel;
	for (const auto &pair : last_transition) {
		idxsToDel.push_back(pair.second);
	}
	sort(idxsToDel.begin(), idxsToDel.end(), std::greater<int>());
	dbgg("", vecToStr(idxsToDel));

	for (int idx : idxsToDel) {
		dbgg("Removing free_seq ..", idx, free_seqs[idx]);
		free_seqs.erase(free_seqs.begin() + idx);
	}

	return choose100(free_seqs);
}

bool DynStatsBuilder::calcTransitionIsUnclear(int track_idx_a, int track_idx_b) {
	auto &ta = cfg_.tracks[track_idx_a];
	auto &tb = cfg_.tracks[track_idx_b];
	if (tb.isSupported()) return false;
	if (ta.dyn_patterns_[track_idx_b].empty() && cfg_.chunk_transitions[{track_idx_a, track_idx_b}].size() > 5) {
		return true;
	}
	return false;
}

buffs_t DynStatsBuilder::offsToBuffs(const offs_t &offs, const string &load_prefix) {
	int cnt = 0;
	buffs_t buffs;
	for (auto off : offs) {
		if (g_options.log_mode == I) outProgress(cnt++, offs.size(), load_prefix);
		auto buff = cfg_.file.file_->getFragment(off - pat_size_ / 2, pat_size_);
		buffs.emplace_back(buff, buff + pat_size_);
	}
	if (g_options.log_mode == I) cerr << string(20, ' ') << '\r';
	return buffs;
}

patterns_t DynStatsBuilder::offsToPatterns(const offs_t &all_offs, const string &load_prefix) {
	auto offs_to_consider = choose100(all_offs);
	auto buffs = offsToBuffs(offs_to_consider, load_prefix);
	auto patterns = genRawPatterns(buffs);

	auto offs_to_check = choose100(all_offs);
	auto buffs2 = offsToBuffs(offs_to_check, load_prefix);
	countPatternsSuccess(patterns, buffs2);

	filterBySuccessRate(patterns, load_prefix);
	return patterns;
}
