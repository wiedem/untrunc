#include "chunk_it.h"
#include "track/track_list.h"

ChunkIt::ChunkIt(const std::vector<Track> &tracks, off_t mdat_end, bool do_filter, bool exclude_dummy)
    : tracks_(&tracks), do_filter_(do_filter) {
	int n_real_tracks = tracks_->size();
	if (exclude_dummy)
		if (tracks_->back().is_dummy_) n_real_tracks--;
	cur_next_chunk_idx_.resize(n_real_tracks);
	mdat_end_ = mdat_end;

	bad_tmcd_idx_ = TrackList::findIdx(*tracks_, "tmcd");
	if (bad_tmcd_idx_ >= 0 &&
	    (!(*tracks_)[bad_tmcd_idx_].chunks_.empty() && (*tracks_)[bad_tmcd_idx_].chunks_[0].size_ > 4))
		bad_tmcd_idx_ = -1;
	operator++();
}

void ChunkIt::operator++() {
	next_chunk_idx_++;
	int track_idx = -1;
	auto off = std::numeric_limits<off_t>::max();
	for (uint i = 0; i < cur_next_chunk_idx_.size(); i++) {
		if (cur_next_chunk_idx_[i] >= (*tracks_)[i].chunks_.size()) continue;
		auto toff = (*tracks_)[i].chunks_[cur_next_chunk_idx_[i]].off_;
		if (toff < off) {
			track_idx = i;
			off = toff;
		}
	}
	if (track_idx < 0) {
		becomeEndIt();
		return;
	}
	if (off >= mdat_end_) {
		assertt(g_options.ignore_out_of_bound_chunks);
		logg(W, "reached premature end of mdat\n");
		becomeEndIt();
		return;
	}
	current_ = ChunkIt::Chunk((*tracks_)[track_idx].chunks_[cur_next_chunk_idx_[track_idx]], track_idx);
	cur_next_chunk_idx_[track_idx]++;

	if (next_chunk_idx_ < 10 && track_idx == bad_tmcd_idx_) {
		current_.should_ignore_ = true;
		if (do_filter_) operator++();
	}
}
