// Core Mp4 infrastructure: Chunk struct, fragment loading, track lookup,
// offset helpers, repair entry point.

#include <string>
#include <iostream>

extern "C" {
#include <stdint.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "mp4.h"
#include "mp4_repairer.h"
#include "atom/atom.h"
#include "io/file.h"

using namespace std;

// Chunk struct

Mp4::Chunk::Chunk(off_t off, int ns, int track_idx, int sample_size)
    : IndexedChunk(off, ns * sample_size, ns, track_idx), sample_size_(sample_size) {}

ostream &operator<<(ostream &out, const Mp4::Chunk &c) {
	return out << ss("[", c.track_idx_, "] (", c.sample_size_, " x", c.n_samples_, ")");
}

bool operator==(const Mp4::Chunk &a, const Mp4::Chunk &b) {
	return a.off_ == b.off_ && a.n_samples_ == b.n_samples_ && a.track_idx_ == b.track_idx_ &&
	       a.sample_size_ == b.sample_size_ && a.size_ == b.size_;
}
bool operator!=(const Mp4::Chunk &a, const Mp4::Chunk &b) {
	return !(a == b);
}

// Fragment loading

const uchar *Mp4::loadFragment(off_t offset, bool update_cur_maxlen) {
	if (update_cur_maxlen)
		ctx_.file_.current_maxlength_ = min((int64_t)ctx_.file_.max_part_size_, mdatContentSize() - offset);
	auto buf_sz = min((int64_t)g_options.max_buf_sz_needed, mdatContentSize() - offset);
	return ctx_.file_.mdat_->getFragment(offset, buf_sz);
}

const uchar *Mp4::getBuffAround(off_t offset, int64_t n) {
	auto &mdat = ctx_.file_.mdat_;
	if (offset - n / 2 < 0 || offset + n / 2 > mdat->contentSize()) return nullptr;
	return mdat->getFragment(offset - n / 2, n);
}

// Offset helpers

off_t Mp4::toAbsOff(off_t offset) {
	return ctx_.file_.mdat_->contentStart() + offset;
}

string Mp4::offToStr(off_t offset) {
	return ss(hexIf(offset), " / ", hexIf(toAbsOff(offset)));
}

// Track lookup

bool Mp4::hasCodec(const string &codec_name) {
	return TrackList::has(tracks_, codec_name);
}

uint Mp4::getTrackIdx(const string &codec_name) {
	int r = TrackList::findIdx(tracks_, codec_name);
	if (r < 0) throw std::runtime_error("asked for nonexistent track");
	return r;
}

int Mp4::getTrackIdx2(const string &codec_name) const {
	return TrackList::findIdx(tracks_, codec_name);
}

string Mp4::getCodecName(uint track_idx) {
	return string(TrackList::nameOf(tracks_, (int)track_idx));
}

Track &Mp4::getTrack(const string &codec_name) {
	auto *t = TrackList::find(tracks_, codec_name);
	if (!t) throw std::runtime_error("asked for nonexistent track");
	return *t;
}

// Track management

void Mp4::afterTrackRealloc() {
	for (int i = 0; i < (int)tracks_.size(); i++) {
		tracks_[i].mp4_ = this;
		tracks_[i].codec_.mp4_ = this;
		tracks_[i].codec_.onTrackRealloc(i);
	}
}

// Scan-loop state mutation

void Mp4::addUnknownSequence(off_t start, uint64_t length) {
	assertt(length);
	addToExclude(start, length);
	ctx_.scan_.unknown_lengths_.emplace_back(length);
}

void Mp4::addToExclude(off_t start, uint64_t length, bool force) {
	if (g_options.dont_exclude && !force) return;
	if (ctx_.file_.mdat_->sequences_to_exclude_.size()) {
		auto [last_start, last_length] = ctx_.file_.mdat_->sequences_to_exclude_.back();
		off_t last_end = last_start + last_length;
		assertt(start >= last_end, start, last_end, last_start, last_length, start, length);
	}

	if (start + length > to_uint64(mdatContentSize())) {
		logg(V, start, " + ", length, " > ", mdatContentSize(), "\n");
		logg(W, "addToExclude: sequence goes beyond EOF\n");
		length = mdatContentSize() - start;
	}

	ctx_.file_.mdat_->sequences_to_exclude_.emplace_back(start, length);
	ctx_.file_.mdat_->total_excluded_yet_ += length;
}

void Mp4::addFrame(const FrameInfo &fi) {
	Track &track = tracks_[fi.track_idx_];
	track.num_samples_++;

	if (fi.keyframe_) track.keyframes_.push_back(track.sizes_.size());

	if (fi.should_dump_) to_dump_.emplace_back(fi);

	if (fi.audio_duration_ && track.constant_duration_ == -1) track.times_.push_back(fi.audio_duration_);
	if (!track.constant_size_) track.sizes_.push_back(fi.length_);

	setLastTrackIdx(fi.track_idx_);
}

void Mp4::addChunk(const Mp4::Chunk &chunk) {
	FrameInfo match(chunk.track_idx_, 0, 0, chunk.off_, chunk.sample_size_);
	for (uint n = chunk.n_samples_; n--;) {
		addFrame(match);
		match.offset_ += chunk.sample_size_;
	}

	setLastTrackIdx(chunk.track_idx_);
}

void Mp4::setLastTrackIdx(int track_idx) {
	ctx_.scan_.last_track_idx_ = track_idx;
	ctx_.scan_.done_padding_ = false;
}

bool Mp4::chkUnknownSequenceEnded(off_t offset) {
	if (!ctx_.scan_.unknown_length_) return false;
	g_logger->disableNoiseSuppression();
	addToExclude(offset - ctx_.scan_.unknown_length_, ctx_.scan_.unknown_length_);
	ctx_.scan_.unknown_lengths_.emplace_back(ctx_.scan_.unknown_length_);
	ctx_.scan_.unknown_length_ = 0;
	return true;
}

int Mp4::findSizeWithContinuation(off_t off, vector<int> sizes) {
	if (!ctx_.order_.track_order_.size() || amInFreeSequence() || currentChunkFinished(1)) {
		return sizes.back();
	}
	auto &t = tracks_[ctx_.scan_.last_track_idx_];
	for (int i = sizes.size() - 1; i >= 0; --i) {
		auto sz = t.alignPktLength(sizes[i]);
		const uchar *buf = ctx_.file_.mdat_->getFragmentIf(off + sz, kMatchLookaheadBuf);
		if (!buf) continue;
		bool matches = t.codec_.matchSample(buf);
		dbgg("findSizeWithContinuation", i, sizes[i], matches);
		if (matches) {
			return sizes[i];
		}
	}
	return sizes.back();
}

// Repair entry point

void Mp4::repair(const string &filename) {
	Mp4Repairer(*this).repair(filename);
}
