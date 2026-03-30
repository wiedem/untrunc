#include "chunk_it.h"
#include "mp4.h"

ChunkIt::ChunkIt(const Mp4 *mp4, bool do_filter, bool exclude_dummy) : mp4_(mp4), do_filter_(do_filter) {
	int n_real_tracks = mp4_->tracks_.size();
	if (exclude_dummy)
		if (mp4_->tracks_.back().is_dummy_) n_real_tracks--;
	cur_next_chunk_idx_.resize(n_real_tracks);
	mdat_end_ = mp4->mdatEnd();

	bad_tmcd_idx_ = mp4_->getTrackIdx2("tmcd");
	if (bad_tmcd_idx_ >= 0 &&
	    (!mp4->tracks_[bad_tmcd_idx_].chunks_.empty() && mp4->tracks_[bad_tmcd_idx_].chunks_[0].size_ > 4))
		bad_tmcd_idx_ = -1;
	operator++();
}

void ChunkIt::operator++() {
	next_chunk_idx_++;
	int track_idx = -1;
	auto off = std::numeric_limits<off_t>::max();
	for (uint i = 0; i < cur_next_chunk_idx_.size(); i++) {
		if (cur_next_chunk_idx_[i] >= mp4_->tracks_[i].chunks_.size()) continue;
		auto toff = mp4_->tracks_[i].chunks_[cur_next_chunk_idx_[i]].off_;
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
	current_ = ChunkIt::Chunk(mp4_->tracks_[track_idx].chunks_[cur_next_chunk_idx_[track_idx]], track_idx);
	cur_next_chunk_idx_[track_idx]++;

	if (next_chunk_idx_ < 10 && track_idx == bad_tmcd_idx_) {
		current_.should_ignore_ = true;
		if (do_filter_) operator++();
	}
}
