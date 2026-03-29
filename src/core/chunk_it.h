// Iterators for walking chunks from all tracks in mdat order.
// Included at the bottom of mp4.h (after Mp4 is fully defined)
// so inline bodies can access Mp4 members.

#pragma once
#include "track/track.h" // Track::Chunk
#include "util/common.h" // uchar, assert (custom multi-arg version), etc.

#include <vector>
#include <limits>

class ChunkIt {
  public:
	class Chunk : public IndexedChunk {
	  public:
		Chunk() = default;
		Chunk(off_t off, int sz, int ns, int track_idx) : IndexedChunk(off, sz, ns, track_idx) {}
		Chunk(Track::Chunk t_chunk, int track_idx) : IndexedChunk(t_chunk, track_idx) {}
		bool should_ignore_ = false;
	};

	const Mp4 *mp4_;
	ChunkIt::Chunk current_;

	ChunkIt(const Mp4 *mp4, bool do_filter, bool exclude_dummy);
	static ChunkIt mkEndIt() { return ChunkIt(true); }

	void operator++();

	ChunkIt::Chunk &operator*() { return current_; }
	bool operator!=(ChunkIt rhs) {
		return current_.track_idx_ != rhs.current_.track_idx_ || current_.off_ != rhs.current_.off_;
	}

  private:
	off_t mdat_end_;
	int bad_tmcd_idx_ = -1;
	size_t next_chunk_idx_ = -1;
	std::vector<uint> cur_next_chunk_idx_;
	bool do_filter_;

	ChunkIt(bool is_end_it_) {
		assertt(is_end_it_);
		becomeEndIt();
	}
	void becomeEndIt() { current_ = ChunkIt::Chunk(-1, -1, -1, -1); }
};

/* Iterable range that lists all chunks in mdat order. */
class AllChunksIn {
  public:
	ChunkIt it_, end_it_;
	AllChunksIn(Mp4 *mp4, bool do_filter, bool exclude_dummy = true)
	    : it_(mp4, do_filter, exclude_dummy), end_it_(ChunkIt::mkEndIt()) {}
	ChunkIt begin() { return it_; }
	ChunkIt end() { return end_it_; }
};
