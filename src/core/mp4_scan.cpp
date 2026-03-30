// Scan navigation: offset advancement, skipping zeros/atoms, step calculation.

#include <iostream>
#include <iomanip>
#include <cstring>

#include "mp4.h"

using namespace std;

int Mp4::skipZeros(off_t &offset, const uchar *start) {
	int start_val;
	memcpy(&start_val, start, sizeof(start_val));
	if (start_val != 0) return 0;

	if (g_options.use_chunk_stats) {
		for (auto &cn : {"sowt", "twos"}) {
			if (hasCodec(cn) && getTrack(cn).isChunkOffsetOk(offset)) {
				logg(V, "won't skip zeros at: ", offToStr(offset), "\n");
				return 0;
			}
		}

		if (ctx_.dummy_.has_zero_transitions_) {
			auto c = getChunkPrediction(offset, true);
			if (c) {
				logg(V, "won't skip zeros at: ", offToStr(offset), " (perfect chunk-match)\n");
				return 0;
			}
		}
	}

	if (idx_free_ > 0 && ctx_.dummy_.do_padding_skip_) { // next chunk is probably padded with random data..
		int len = tracks_[idx_free_].stepToNextOtherChunk(offset);
		logg(V, "used free's end_off_gcd -> ", len, "\n");
		if (len) {
			return len;
		} else
			logg(W, "end_off_gcd method failed..\n");
	}

	int64_t step = 4;
	if (ctx_.scan_.unknown_length_ || g_options.use_chunk_stats) step = calcStep(offset);
	return step;
}

int Mp4::skipAtomHeaders(off_t offset, const uchar *start) {
	int sz = mdatHeaderSkipSize(start);
	if (sz) loggF(V, "Skipping 'mdat' header: ", offToStr(offset), '\n');
	return sz;
}

int Mp4::skipAtoms(off_t offset, const uchar *start) {
	int64_t remaining = mdatContentSize() - offset;
	int atom_len = atomSkipSize(start, remaining);
	if (atom_len) {
		string s(start + 4, start + 8);
		loggF(!contains({"free", "iidx"}, s) ? W : V, "Skipping ", s, " atom: ", atom_len, '\n');
		return atom_len;
	}
	if (isValidAtomName(start + 4)) {
		uint raw_len;
		memcpy(&raw_len, start, sizeof(raw_len));
		raw_len = swap32(raw_len);
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
		if (offset >= mdatContentSize()) {
			return false;
		}

		offset += padding;
		ctx_.scan_.done_padding_ = true;
	}

	int skipped = 0;
	while (true) {
		offset += skipped;

		if (offset >= mdatContentSize()) { // at end?
			return false;
		}

		auto start = loadFragment(offset);

		// Progress is reported via RepairReport::onProgress() in the repair loop.

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
				ctx_.scan_.atoms_skipped_.emplace_back(skipped);
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
	const int64_t kPeekBytes = 8;
	auto s = ctx_.file_.mdat_->getFragment(offset, min(kPeekBytes, mdatContentSize() - offset));

	uint begin, next;
	memcpy(&begin, s, sizeof(begin));
	memcpy(&next, s + 4, sizeof(next));
	begin = swap32(begin);
	next = swap32(next);
	logg(V, "Offset: ", offToStr(offset), " : ", setfill('0'), setw(8), hex, begin, " ", setw(8), next, dec,
	     setfill(' '), '\n');
}

int64_t Mp4::calcStep(off_t offset) {
	if (idx_free_ > 0 && ctx_.dummy_.do_padding_skip_) { // next chunk is probably padded with random data..
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

		step = min(step, mdatContentSize() - offset);
	} else {
		step = Mp4::step_;
	}
	return step;
}

bool Mp4::pointsToZeros(off_t off) {
	if (mdatContentSize() - off < 4) return false;
	auto buff = ctx_.file_.mdat_->getFragment(off, 4);
	int tmp;
	memcpy(&tmp, buff, sizeof(tmp));
	if (tmp == 0) {
		logg(V, "pointsToZeros: found 4 zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}

bool Mp4::isAllZerosAt(off_t off, int n) {
	if (mdatContentSize() - off < n) return false;
	auto buff = ctx_.file_.mdat_->getFragment(off, n);
	if (isAllZeros(buff, n)) {
		logg(V, "isAllZerosAt: found ", n, " zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}
