/*
	Untrunc - track.h

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio

														*/

#include <string>
#include <iostream>
#include <iomanip> // setprecision
#include <algorithm>
#include <fstream>

extern "C" {
#include <stdint.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "mp4.h"
#include "mp4_repairer.h"
#include "atom/atom.h"
#include "io/file.h"
#include "rsv.h"

using namespace std;

void Mp4::addUnknownSequence(off_t start, uint64_t length) {
	assertt(length);
	addToExclude(start, length);
	ctx_.unknown_lengths_.emplace_back(length);
}

void Mp4::addToExclude(off_t start, uint64_t length, bool force) {
	if (g_options.dont_exclude && !force) return;
	if (ctx_.current_mdat_->sequences_to_exclude_.size()) {
		auto [last_start, last_length] = ctx_.current_mdat_->sequences_to_exclude_.back();
		off_t last_end = last_start + last_length;
		assertt(start >= last_end, start, last_end, last_start, last_length, start, length);
	}

	if (start + length > to_uint64(ctx_.current_mdat_->contentSize())) {
		logg(V, start, " + ", length, " > ", ctx_.current_mdat_->contentSize(), "\n");
		logg(W, "addToExclude: sequence goes beyond EOF\n");
		length = ctx_.current_mdat_->contentSize() - start;
	}

	ctx_.current_mdat_->sequences_to_exclude_.emplace_back(start, length);
	ctx_.current_mdat_->total_excluded_yet_ += length;
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
				found_ok = 1;
				continue;
			} // padding might was enough
			advanceOffset(off_i, true);
			if (off_i == off_end) {
				found_ok = 1;
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

	ctx_.dummy_is_skippable_ = false;
	if (canSkipFree()) {
		logg(V, "yes, via canSkipFree\n");
		ctx_.dummy_is_skippable_ = true;
	}
	if (t.dummyIsUsedAsPadding()) {
		logg(V, "yes, by using padding-skip strategy\n");
		ctx_.dummy_is_skippable_ = true;
		ctx_.dummy_do_padding_skip_ = true;
	}

	if (!ctx_.dummy_is_skippable_) {
		logg(V, "no, seems not to be skippable\n");
	}
}

void Mp4::correctChunkIdx(int track_idx) {
	assertt(track_idx >= 0 && track_idx != idx_free_);
	if (!ctx_.track_order_.size()) {
		correctChunkIdxSimple(track_idx);
		return;
	}

	// TODO: call correctChunkIdx once we know n_samples
	//       ATM result could be wrong if 'chunk::ĺikely_n_samples_.size() > 1'
	while (ctx_.track_order_[ctx_.next_chunk_idx_ % ctx_.track_order_.size()].first != track_idx)
		ctx_.next_chunk_idx_++;

	if (tracks_[track_idx].likely_n_samples_.size() > 1) logg(W, "correctChunkIdx(", track_idx, ") could be wrong\n");
}

void Mp4::correctChunkIdxSimple(int track_idx) {
	assertt(track_idx != idx_free_);
	auto order_sz = ctx_.track_order_simple_.size();
	if (!order_sz) return;

	int off_ok = -1;
	for (uint off = 0; off < order_sz; off++) {
		if (ctx_.track_order_simple_[(ctx_.next_chunk_idx_ + off) % order_sz] == track_idx) {
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
		ctx_.next_chunk_idx_ += off_ok;
	}
}

bool Mp4::isTrackOrderEnough() {
	auto isEnough = [&]() {
		if (!ctx_.track_order_.size()) return false;
		for (auto &t : tracks_) {
			if (t.isSupported()) continue;
			if (t.is_dummy_ && ctx_.dummy_is_skippable_) continue; // not included in ctx_.track_order_
			if (!t.is_dummy_ && t.likely_sample_sizes_.size() == 1) continue;

			logg(W, "ctx_.track_order_ found, but not sufficient\n");
			ctx_.track_order_.clear();
			return false;
		}
		return true;
	};

	bool is_enough = isEnough();
	logg(V, "isTrackOrderEnough: ", is_enough, "  (sz=", ctx_.track_order_.size(), ")\n");
	if (!is_enough && ctx_.track_order_.size()) {
		// in theory this could probably still be used somehow..
		logg(W, "ctx_.track_order_ found, but not sufficient\n");
		ctx_.track_order_.clear();
	}
	return is_enough;
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

const uchar *Mp4::loadFragment(off_t offset, bool update_cur_maxlen) {
	if (update_cur_maxlen)
		ctx_.current_maxlength_ = min((int64_t)ctx_.max_part_size_, ctx_.current_mdat_->contentSize() - offset);
	auto buf_sz = min((int64_t)g_options.max_buf_sz_needed, ctx_.current_mdat_->contentSize() - offset);
	return ctx_.current_mdat_->getFragment(offset, buf_sz);
}

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

// currently only used to distinguish duplicate tracks (e.g. 2x mp4a)
bool Mp4::isExpectedTrackIdx(int i) {
	if (ctx_.track_order_simple_.empty()) return true;
	int expected_idx = ctx_.track_order_simple_[ctx_.next_chunk_idx_ % ctx_.track_order_simple_.size()];
	if (expected_idx == i) return true;

	if (getCodecName(i) != getCodecName(expected_idx)) {
		logg(W, "expected codec ", getCodecName(expected_idx), " but found ", getCodecName(i), "\n");
		correctChunkIdx(i);
		return true;
	}
	return false;
}

const uchar *Mp4::getBuffAround(off_t offset, int64_t n) {
	auto &mdat = ctx_.current_mdat_;
	if (offset - n / 2 < 0 || offset + n / 2 > mdat->contentSize()) return nullptr;
	return mdat->getFragment(offset - n / 2, n);
}

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

// returns length skipped
int Mp4::skipZeros(off_t &offset, const uchar *start) {
	if (*(int *)start != 0) return 0;

	if (g_options.use_chunk_stats) {
		for (auto &cn : {"sowt", "twos"}) {
			if (hasCodec(cn) && getTrack(cn).isChunkOffsetOk(offset)) {
				logg(V, "won't skip zeros at: ", offToStr(offset), "\n");
				return 0;
			}
		}

		if (ctx_.has_zero_transitions_) {
			auto c = getChunkPrediction(offset, true);
			if (c) {
				logg(V, "won't skip zeros at: ", offToStr(offset), " (perfect chunk-match)\n");
				return 0;
			}
		}
	}

	if (idx_free_ > 0 && ctx_.dummy_do_padding_skip_) { // next chunk is probably padded with random data..
		int len = tracks_[idx_free_].stepToNextOtherChunk(offset);
		logg(V, "used free's end_off_gcd -> ", len, "\n");
		if (len) {
			return len;
		} else
			logg(W, "end_off_gcd method failed..\n");
	}

	int64_t step = 4;
	if (ctx_.unknown_length_ || g_options.use_chunk_stats) step = calcStep(offset);
	return step;
}

int Mp4::skipAtomHeaders(off_t offset, const uchar *start) {
	int sz = mdatHeaderSkipSize(start);
	if (sz) loggF(V, "Skipping 'mdat' header: ", offToStr(offset), '\n');
	return sz;
}

int Mp4::skipAtoms(off_t offset, const uchar *start) {
	int64_t remaining = ctx_.current_mdat_->contentSize() - offset;
	int atom_len = atomSkipSize(start, remaining);
	if (atom_len) {
		string s(start + 4, start + 8);
		loggF(!contains({"free", "iidx"}, s) ? W : V, "Skipping ", s, " atom: ", atom_len, '\n');
		return atom_len;
	}
	if (isValidAtomName(start + 4)) {
		uint raw_len = swap32(*(const uint *)start);
		string s(start + 4, start + 8);
		// tmcd is deliberately excluded by atomSkipSize: suppress the "NOT skipping" noise
		if (s != "tmcd") loggF(W, "NOT skipping ", s, " atom: ", raw_len, " (at ", offToStr(offset), ")\n");
	}
	return 0;
}

// has minimal side effects - returns true if not EOF
bool Mp4::advanceOffset(off_t &offset, bool just_simulate) {
	auto padding = getChunkPadding(offset);
	if (padding > 0) {
		if (offset >= ctx_.current_mdat_->contentSize()) {
			return false;
		}

		offset += padding;
		ctx_.done_padding_ = true;
	}

	int skipped = 0;
	while (true) {
		offset += skipped;

		if (offset >= ctx_.current_mdat_->contentSize()) { // at end?
			return false;
		}

		auto start = loadFragment(offset);

		static uint loop_cnt = 0;
		if (g_options.log_mode == I && loop_cnt++ % 2000 == 0) outProgress(offset, ctx_.current_mdat_->file_end_);

		if ((skipped = skipZeros(offset, start))) {
			if (padding > 0) { // we assume that if file uses padding, zeros might be part of payload
				logg(V, "Ignoring zero skip (", skipped, "), since already done padding\n");
			} else {
				continue;
			}
		}

		if ((skipped = skipAtomHeaders(offset, start))) {
			if (!just_simulate) {
				chkUnknownSequenceEnded(offset);
			}
			continue;
		}

		if ((skipped = skipAtoms(offset, start))) {
			if (!just_simulate) {
				ctx_.atoms_skipped_.emplace_back(skipped);
				chkUnknownSequenceEnded(offset);
			}
			continue;
		}

		break;
	}

	if (g_options.log_mode >= LogMode::V) {
		if (just_simulate) {
			logg(V, "\n(reading element from mdat - simulate)\n");
		} else {
			logg(V, "\n(reading element from mdat)\n");
		}
		printOffset(offset);
	}

	return true;
}

void Mp4::printOffset(off_t offset) {
	auto s = ctx_.current_mdat_->getFragment(offset, min((int64_t)8, ctx_.current_mdat_->contentSize() - offset));

	uint begin = swap32(*(uint *)s);
	uint next = swap32(*(uint *)(s + 4));
	logg(V, "Offset: ", offToStr(offset), " : ", setfill('0'), setw(8), hex, begin, " ", setw(8), next, dec,
	     setfill(' '), '\n');
}

off_t Mp4::toAbsOff(off_t offset) {
	return ctx_.current_mdat_->contentStart() + offset;
}

string Mp4::offToStr(off_t offset) {
	return ss(hexIf(offset), " / ", hexIf(toAbsOff(offset)));
}

void Mp4::onFirstChunkFound(int track_idx) {
	if (track_idx == idx_free_) return;
	ctx_.first_chunk_found_ = true;
	assertt(ctx_.next_chunk_idx_ == 0);
	correctChunkIdx(track_idx);
	if (ctx_.next_chunk_idx_)
		logg(W, "different start chunk: ", track_idx, " instead of ", ctx_.track_order_simple_[0], "\n");
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

void Mp4::repair(const string &filename) {
	Mp4Repairer(*this).repair(filename);
}

int Mp4::findSizeWithContinuation(off_t off, vector<int> sizes) {
	if (!ctx_.track_order_.size() || amInFreeSequence() || currentChunkFinished(1)) {
		return sizes.back();
	}
	auto &t = tracks_[ctx_.last_track_idx_];
	for (int i = sizes.size() - 1; i >= 0; --i) {
		auto sz = t.alignPktLength(sizes[i]);
		const uchar *buf = ctx_.current_mdat_->getFragmentIf(off + sz, 1024);
		if (!buf) continue;
		bool matches = t.codec_.matchSample(buf);
		dbgg("findSizeWithContinuation", i, sizes[i], matches);
		if (matches) {
			return sizes[i];
		}
	}
	return sizes.back();
}

void Mp4::setLastTrackIdx(int track_idx) {
	ctx_.last_track_idx_ = track_idx;
	ctx_.done_padding_ = false;
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

bool Mp4::chkUnknownSequenceEnded(off_t offset) {
	if (!ctx_.unknown_length_) return false;
	g_logger->disableNoiseSuppression();
	addToExclude(offset - ctx_.unknown_length_, ctx_.unknown_length_);
	ctx_.unknown_lengths_.emplace_back(ctx_.unknown_length_);
	ctx_.unknown_length_ = 0;
	return true;
}
