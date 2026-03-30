// Dynamic statistics entry point for Mp4, plus remaining stats helpers.

#include <iostream>
#include <iomanip>

#include "mp4.h"
#include "dyn_stats_builder.h"
#include "track/track_order.h"

using namespace std;

void Mp4::genTrackOrder() {
	if (!ctx_.file_.mdat_) findMdat(*ctx_.file_.file_);

	vector<pair<int, int>> order;
	for (auto &cur_chunk : AllChunksIn(tracks_, mdatEnd(), true)) {
		auto track_idx = cur_chunk.track_idx_;
		if (order.size() > 100) break;
		order.emplace_back(make_pair(track_idx, cur_chunk.n_samples_));
	}

	ctx_.order_.track_order_simple_ = findOrderSimple(order);

	if (g_options.log_mode >= LogMode::V) {
		string line = ss("order ( ", order.size(), "):");
		for (auto &p : order)
			line += ss(" (", p.first, ", ", p.second, ")");
		logg(V, line, "\n");
	}
	if (findOrder(order)) ctx_.order_.track_order_ = order;

	for (auto &[i, n_samples] : order) {
		ctx_.order_.cycle_size_ += tracks_[i].ss_stats_.averageSize() * n_samples;
	}
}

void Mp4::genDynStats(bool /*force_patterns*/) {
	if (chunk_transitions_.size()) return; // already generated
	if (!ctx_.file_.mdat_) findMdat(*ctx_.file_.file_);

	DynStatsBuilder builder{DynStatsBuilder::Config{
	    tracks_,
	    chunk_transitions_,
	    free_seqs_,
	    idx_free_,
	    twos_track_idx_,
	    ctx_.file_,
	    ctx_.scan_,
	    ctx_.order_,
	    ctx_.dummy_,
	    [this] { afterTrackRealloc(); },
	    [this](off_t &off, bool sim) -> bool { return advanceOffset(off, sim); },
	    [this] { genTrackOrder(); },
	}};
	builder.generate();

	// Populate off_to_chunk_ for free sequences (verification mode only).
	// analyze(true) runs before genDynStats, so free-sequence chunks are not yet known
	// at that point. Reconstruct from tracks_[idx_free_].chunks_ now that they exist.
	if (ctx_.scan_.use_offset_map_ && idx_free_ >= 0) {
		for (auto &c : tracks_[idx_free_].chunks_) {
			auto relOff = c.off_ - ctx_.file_.mdat_->contentStart();
			off_to_chunk_[relOff] = Mp4::Chunk(relOff, c.size_, idx_free_, 1);
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
