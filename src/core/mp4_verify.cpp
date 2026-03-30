// Diagnostic and verification functions for Mp4 (analyzeOffset, analyze, dumpSamples, etc.)

#include <iostream>
#include <iomanip>

#include "mp4.h"
#include "io/file.h"

using namespace std;

void Mp4::analyzeOffset(const string &filename, off_t real_offset) {
	FileRead file(filename);
	auto mdat = findMdat(file);
	if (real_offset < mdat->contentStart() || real_offset >= mdat->file_end_)
		throw std::runtime_error("given offset is not in 'mdat'");

	auto buff = file.getPtrAt(real_offset, 16);
	cerr << mkHexStr(buff, 16) << '\n';

	auto off = real_offset - mdat->contentStart();
	auto match = getMatch(off);
	dumpMatch(match, 0, nullptr, cerr);
}

void Mp4::chkUntrunc(FrameInfo &fi, Codec &c, int i, AnalyzeReport *report) {
	auto offset = fi.offset_;
	auto start = loadFragment(offset);
	bool verbose = g_options.log_mode >= LogMode::V;
	string off_str = offToStr(offset);

	if (verbose) {
		cerr << "\n(" << i << ") Size: " << fi.length_ << " offset: " << off_str
		     << "  begin: " << mkHexStr(start, 4) << " " << mkHexStr(start + 4, 4);
		auto end_off = offset + fi.length_ - 4;
		auto sz = min(to_int64(8), mdatContentSize() - end_off);
		auto end = ctx_.file_.mdat_->getFragment(end_off, sz);
		cerr << " end: " << mkHexStr(end, sz) << '\n';
		start = loadFragment(offset);
	}

	bool ok = true;
	bool matches = false;
	for (auto &t : tracks_)
		if (t.codec_.matchSample(start)) {
			if (t.codec_.name_ != c.name_) {
				if (verbose) cerr << "Matched wrong codec! '" << t.codec_.name_ << "' instead of '" << c.name_ << "'\n";
				if (report)
					report->onMismatch(fi.track_idx_, i, off_str, "wrong codec: '" + t.codec_.name_ + "' instead of '" + c.name_ + "'");
				ok = false;
			} else {
				matches = true;
				break;
			}
		}
	uint size = c.getSize(start, ctx_.file_.current_maxlength_, offset);
	uint duration = c.audio_duration_;
	//TODO check if duration is working with the stts duration.

	if (!matches) {
		if (verbose) cerr << "Match failed! '" << c.name_ << "' itself not detected\n";
		if (report)
			report->onMismatch(fi.track_idx_, i, off_str, "match failed for '" + c.name_ + "'");
		ok = false;
	}

	if (verbose) {
		cerr << "detected size: " << size << " true: " << fi.length_;
		if (size != fi.length_) cerr << "  <- WRONG";
		cerr << '\n';
	}
	if (size != fi.length_ && report)
		report->onMismatch(fi.track_idx_, i, off_str, "size: expected " + to_string(fi.length_) + ", detected " + to_string(size));

	if (c.name_ == "mp4a") {
		if (verbose) {
			cerr << "detected duration: " << duration << " true: " << fi.audio_duration_;
			if (duration != fi.audio_duration_) cerr << "  <- WRONG";
			cerr << '\n';
			if (c.was_bad_) cerr << "detected bad frame\n";
		}
		if (duration != fi.audio_duration_ && report)
			report->onMismatch(fi.track_idx_, i, off_str, "duration: expected " + to_string(fi.audio_duration_) + ", detected " + to_string(duration));
	}

	if (fi.keyframe_) {
		if (verbose) cerr << "detected keyframe: " << c.was_keyframe_ << " true: 1\n";
		if (!c.was_keyframe_) {
			if (verbose) cerr << "keyframe not detected!\n";
			if (report)
				report->onMismatch(fi.track_idx_, i, off_str, "keyframe not detected");
			ok = false;
		}
	}

	if (report) report->onFrameAnalyzed();
	if (!ok && verbose) hitEnterToContinue();
	//assertt(length == track.sizes[i]);
}

void Mp4::analyze(bool gen_off_map, AnalyzeReport *report) {
	auto &mdat = ctx_.file_.mdat_;
	if (!mdat) findMdat(*ctx_.file_.file_);
	assertt(mdat);

	for (uint idx = 0; idx < tracks_.size(); idx++) {
		Track &track = tracks_[idx];
		if (!gen_off_map && g_options.log_mode >= LogMode::V)
			cerr << "\nTrack " << idx << " codec: " << track.codec_.name_ << endl;

		if (track.isChunkTrack()) {
			track.genChunkSizes();
			for (uint i = 0; i < track.chunks_.size(); i++) {
				auto &c = track.chunks_[i];
				auto off = c.off_ - mdat->contentStart();
				assertt(c.size_ % c.n_samples_ == 0);
				off_to_chunk_[off] = Mp4::Chunk(off, c.n_samples_, idx, c.size_ / c.n_samples_);
			}
		} else {
			uint k = track.keyframes_.size() ? track.keyframes_[0] : -1, ik = 0;

			auto c_it = track.chunks_.begin();
			size_t sample_idx_in_chunk = 0;
			off_t cur_off = c_it->off_ - mdat->contentStart();
			for (uint i = 0; i < track.sizes_.size(); i++) {
				bool is_keyframe = k == i;

				if (is_keyframe && ++ik < track.keyframes_.size()) k = track.keyframes_[ik];

				auto sz = track.getSize(i);
				auto fi = FrameInfo(idx, is_keyframe, track.getTime(i), cur_off, sz);

				if (gen_off_map)
					off_to_frame_[cur_off] = fi;
				else {
					if (cur_off > mdat->contentSize()) {
						logg(W, "reached premature end of mdat\n");
						break;
					}
					chkUntrunc(fi, track.codec_, i, report);
				}

				if (++sample_idx_in_chunk >= to_uint(c_it->n_samples_)) {
					cur_off = (++c_it)->off_ - mdat->contentStart();
					sample_idx_in_chunk = 0;
				} else
					cur_off += sz;
			}
		}
	}
}

void Mp4::dumpIdxAndOff(off_t off, int idx, std::ostream &out) {
	auto real_off = ctx_.file_.mdat_->contentStart() + off;
	out << setw(15) << ss("(", idx++, ") ") << setw(12) << ss(hexIf(off), " / ") << setw(8) << hexIf(real_off)
	    << " : ";
}

void Mp4::chkExpectedOff(off_t *expected_off, off_t real_off, uint sz, int idx, std::ostream &out) {
	int v = real_off - *expected_off;
	if (v) {
		dumpIdxAndOff(*expected_off, --idx, out);
		out << "unknown " << v << "\n";
	}
	*expected_off = real_off + sz;
}

void Mp4::dumpMatch(const FrameInfo &fi, int idx, off_t *expected_off, std::ostream &out) {
	if (expected_off) chkExpectedOff(expected_off, fi.offset_, fi.length_, idx, out);
	dumpIdxAndOff(fi.offset_, idx, out);
	//	out << fi << '\n';
	out << fi;
	int comp_off = 0;
	auto &track = tracks_[fi.track_idx_];
	if (track.orig_comp_offs_.size()) comp_off = track.orig_comp_offs_[track.dump_idx_++];
	out << ", " << comp_off << '\n';
}

void Mp4::dumpChunk(const Mp4::Chunk &chunk, int &idx, off_t *expected_off, std::ostream &out) {
	if (expected_off) chkExpectedOff(expected_off, chunk.off_, chunk.size_, idx, out);
	dumpIdxAndOff(chunk.off_, idx, out);
	out << chunk << '\n';
	idx += chunk.n_samples_;
}

void Mp4::dumpSamples() {
	auto &mdat = ctx_.file_.mdat_;
	if (!mdat) {
		auto &file = openFile(filename_ok_);
		findMdat(file);
	}
	analyze(true);
	cout << filename_ok_ << '\n';

	int i = 0;
	if (to_dump_.size())
		for (auto const &x : to_dump_)
			dumpMatch(x, i++, nullptr, cout);
	else {
		auto frame_it = off_to_frame_.begin();
		auto chunk_it = off_to_chunk_.begin();
		auto frameItAtEnd = [&]() { return frame_it == off_to_frame_.end(); };
		auto chunkItAtEnd = [&]() { return chunk_it == off_to_chunk_.end(); };
		off_t expected_off = 0;
		while (true) {
			if (!chunkItAtEnd() && !frameItAtEnd()) {
				if (frame_it->second.offset_ < chunk_it->second.off_)
					dumpMatch((frame_it++)->second, i++, &expected_off, cout);
				else
					dumpChunk((chunk_it++)->second, i, &expected_off, cout);
			} else if (!frameItAtEnd())
				dumpMatch((frame_it++)->second, i++, &expected_off, cout);
			else if (!chunkItAtEnd())
				dumpChunk((chunk_it++)->second, i, &expected_off, cout);
			else
				break;
		}
	}
}

void Mp4::chkDetectionAtImpl(FrameInfo *detectedFramePtr, Mp4::Chunk *detectedChunkPtr, off_t off) {
	auto correctFrameIt = off_to_frame_.end();
	auto correctChunkIt = off_to_chunk_.end();
	if (detectedFramePtr) {
		if ((correctFrameIt = off_to_frame_.find(off)) == off_to_frame_.end()) correctChunkIt = off_to_chunk_.find(off);
	} else {
		if ((correctChunkIt = off_to_chunk_.find(off)) == off_to_chunk_.end()) correctFrameIt = off_to_frame_.find(off);
	}

	bool correct, correctFrameFound = correctFrameIt != off_to_frame_.end(),
	              correctChunkFound = correctChunkIt != off_to_chunk_.end();

	if (detectedFramePtr)
		correct = correctFrameFound && *detectedFramePtr == correctFrameIt->second;
	else if (detectedChunkPtr)
		correct = correctChunkFound && *detectedChunkPtr == correctChunkIt->second;
	else
		correct = !correctFrameFound && !correctChunkFound;

	if (correct) return;
	//	if (correctFrameFound && detectedFramePtr && detectedFramePtr->keyframe_) return;

	auto cerrExtraInfo = [&]() {
		if (correctFrameFound) {
			cerr << ", chunk " << tracks_[correctFrameIt->second.track_idx_].chunks_.size() << ", pkt_in_chunk "
			     << tracks_[correctFrameIt->second.track_idx_].current_chunk_.n_samples_;
		} else if (correctChunkFound) {
			cerr << ", chunk " << tracks_[correctChunkIt->second.track_idx_].chunks_.size();
		}
		cerr << "):\n";
	};

	cerr << "bad detection (at " << offToStr(off) << ", chunk " << ctx_.order_.next_chunk_idx_ << ", pkt "
	     << ctx_.scan_.pkt_idx_;
	cerrExtraInfo();

	if (detectedFramePtr)
		cerr << "  detected: " << *detectedFramePtr << '\n';
	else if (detectedChunkPtr)
		cerr << "  detected: " << *detectedChunkPtr << '\n';
	else
		cerr << "  detected: (none)\n";

	if (correctFrameFound)
		cerr << "  correct:  " << correctFrameIt->second << '\n';
	else if (correctChunkFound)
		cerr << "  correct:  " << correctChunkIt->second << '\n';
	else
		cerr << "   correct: (none)\n";

	hitEnterToContinue();
}

void Mp4::chkFrameDetectionAt(FrameInfo &detected, off_t off) {
	chkDetectionAtImpl(&detected, nullptr, off);
}

void Mp4::chkChunkDetectionAt(Mp4::Chunk &detected, off_t off) {
	chkDetectionAtImpl(nullptr, &detected, off);
}
