// Repair scan loop implementation for Mp4.
// Mp4::repair() delegates to Mp4Repairer::repair().

#include <iostream>
#include <iomanip>

#include "mp4_repairer.h"
#include "rsv.h" // isPointingAtRtmdHeader, RsvRepairer

using namespace std;

Mp4Repairer::Mp4Repairer(Mp4 &mp4, RepairReport &report) : mp4_(mp4), report_(report) {}

bool Mp4Repairer::shouldPreferChunkPrediction() {
	return g_options.use_chunk_stats &&
	       ((mp4_.ctx_.scan_.last_track_idx_ >= 0 &&
	         mp4_.tracks_[mp4_.ctx_.scan_.last_track_idx_].chunkProbablyAtAnd()) ||
	        (mp4_.ctx_.scan_.last_track_idx_ == -1 && mp4_.ctx_.scan_.orig_first_track_->shouldUseChunkPrediction()));
}

void Mp4Repairer::pushBackLastChunk() {
	if (mp4_.ctx_.scan_.last_track_idx_ < 0) return;
	auto &t = mp4_.tracks_[mp4_.ctx_.scan_.last_track_idx_];
	if (t.is_dummy_ && t.current_chunk_.n_samples_ && t.current_chunk_.size_)
		mp4_.addUnknownSequence(t.current_chunk_.off_, t.current_chunk_.size_);
	t.finalizeCurrentChunk();
}

void Mp4Repairer::onNewChunkStarted(int new_track_idx) {
	if (mp4_.ctx_.scan_.ignored_chunk_order_) {
		dbgg("Ignored chunk order previously, calling correctChunkIdx", new_track_idx,
		     mp4_.getCodecName(new_track_idx));
		mp4_.correctChunkIdx(new_track_idx);
		mp4_.ctx_.scan_.ignored_chunk_order_ = false;
	}

	pushBackLastChunk();
	mp4_.ctx_.scan_.done_padding_after_ = false;
	if (new_track_idx != mp4_.idx_free_) {
		if (!mp4_.ctx_.scan_.first_chunk_found_) mp4_.onFirstChunkFound(new_track_idx);
		mp4_.ctx_.order_.next_chunk_idx_++;
	}
}

void Mp4Repairer::chkExcludeOverlap(off_t &start, int64_t &length) {
	auto last_end = (long long)mp4_.ctx_.file_.mdat_->excludedEndOff();
	auto already_skipped = std::max(0LL, last_end - start);
	if (already_skipped) {
		start = last_end;
		length -= already_skipped;
	}
	assertt(length >= 0, length);
}

bool Mp4Repairer::chkOffset(off_t &offset) {
	off_t orig_off = offset;
	bool r = mp4_.advanceOffset(offset);
	auto skipped = offset - orig_off;
	if (skipped && !mp4_.ctx_.scan_.unknown_length_) {
		dbgg("chkOffset ", skipped);
		chkExcludeOverlap(orig_off, skipped);
		mp4_.addToExclude(orig_off, skipped);
	}
	if (!r) { // at end
		pushBackLastChunk();
		mp4_.chkUnknownSequenceEnded(offset);
	}
	return r;
}

void Mp4Repairer::addMatch(off_t &offset, FrameInfo &match) {
	auto &t = mp4_.tracks_[match.track_idx_];

	if (mp4_.ctx_.scan_.use_offset_map_) mp4_.chkFrameDetectionAt(match, offset);

	if (mp4_.chkUnknownSequenceEnded(offset)) {
		logg(V, "found healthy packet again: ", match, "\n");
		mp4_.correctChunkIdx(match.track_idx_);
	}

	if (!mp4_.ctx_.scan_.first_chunk_found_) mp4_.onFirstChunkFound(match.track_idx_);
	if (mp4_.ctx_.scan_.last_track_idx_ != match.track_idx_) {
		onNewChunkStarted(match.track_idx_);
		t.current_chunk_.off_ = offset;
		t.current_chunk_.already_excluded_ = mp4_.ctx_.file_.mdat_->total_excluded_yet_;
	}

	if (t.has_duplicates_ && t.chunkReachedSampleLimit()) {
		onNewChunkStarted(match.track_idx_);
	}

	if (!t.is_dummy_) {
		mp4_.addFrame(match);
		mp4_.ctx_.scan_.pkt_idx_++;
	}

	t.current_chunk_.n_samples_++;
	logg(V, t.current_chunk_.n_samples_, "th sample in ", t.chunks_.size() + 1, "th ", t.codec_.name_, "-chunk\n");
	offset += match.length_;

	if (match.pad_afterwards_) {
		mp4_.addToExclude(offset, match.pad_afterwards_);
		offset += match.pad_afterwards_;
		mp4_.ctx_.scan_.done_padding_ = true;
	}
}

bool Mp4Repairer::tryMatch(off_t &offset) {
	FrameInfo match = mp4_.getMatch(offset);
	if (match) {
		addMatch(offset, match);
		return true;
	}
	return false;
}

bool Mp4Repairer::tryChunkPrediction(off_t &offset) {
	if (!g_options.use_chunk_stats) return false;

	Mp4::Chunk chunk = mp4_.getChunkPrediction(offset);
	if (chunk) {
		auto &t = mp4_.tracks_[chunk.track_idx_];

		if (mp4_.ctx_.scan_.use_offset_map_) mp4_.chkChunkDetectionAt(chunk, offset);

		if (mp4_.ctx_.scan_.unknown_length_ && t.is_dummy_) {
			logg(V, "found '", t.codec_.name_, "' chunk inside unknown sequence: ", chunk, "\n");
			mp4_.ctx_.scan_.unknown_length_ += chunk.size_;
		} else if (mp4_.chkUnknownSequenceEnded(offset)) {
			logg(V, "found healthy chunk again: ", chunk, "\n");
			mp4_.correctChunkIdx(chunk.track_idx_);
		}

		onNewChunkStarted(chunk.track_idx_);

		t.current_chunk_ = chunk;
		t.current_chunk_.already_excluded_ = mp4_.ctx_.file_.mdat_->total_excluded_yet_;

		if (!t.is_dummy_) {
			mp4_.addChunk(chunk);
			mp4_.ctx_.scan_.pkt_idx_ += chunk.n_samples_;
		}

		assertt(chunk.size_ >= 0);
		offset += chunk.size_;

		return true;
	}
	return false;
}

bool Mp4Repairer::tryAll(off_t &offset) {
	if (shouldPreferChunkPrediction()) {
		logg(V, "trying chunkPredict first.. \n");
		if (tryChunkPrediction(offset) || tryMatch(offset)) return true;
	} else {
		if (tryMatch(offset) || tryChunkPrediction(offset)) return true;
	}
	return false;
}

std::vector<int> Mp4Repairer::collectLikelySizes(const std::vector<int> &sizes, double min_freq) {
	if (sizes.empty()) return {};
	map<int, int> freq;
	for (int sz : sizes)
		freq[sz]++;
	int n = (int)sizes.size();
	std::vector<int> result;
	for (auto &p : freq)
		if ((double)p.second / n >= min_freq) result.push_back(p.first);
	sort(result.begin(), result.end());
	return result;
}

bool Mp4Repairer::handleSpecialModes(const string &filename) {
	if (g_options.rsv_ben_mode) {
		FileRead file_read(filename);
		if (!isPointingAtRtmdHeader(file_read)) {
			logg(W, "'-rsv-ben' specified but file does not start with rtmd header\n");
		}
		RsvRepairer(mp4_).repair(filename);
		return true;
	}
	return false;
}

void Mp4Repairer::prepareStats(const string &filename) {
	mp4_.ctx_.scan_.use_offset_map_ = mp4_.ctx_.scan_.use_offset_map_ || filename == mp4_.filename_ok_;
	if (mp4_.ctx_.scan_.use_offset_map_) mp4_.analyze(true);

	if (mp4_.needDynStats()) {
		g_options.use_chunk_stats = true;
		mp4_.genDynStats();
		mp4_.checkForBadTracks();
		logg(I, "using dynamic stats, use '-is' to see them\n");
	} else if (mp4_.setDuplicateInfo()) {
		mp4_.genTrackOrder();
		if (mp4_.ctx_.order_.track_order_simple_.empty()) {
			logg(W, "duplicate codecs found, but no (simple) track order found\n");
		}
	}

	if (g_options.log_mode >= LogMode::V) mp4_.printStats();

	if (!g_options.ignore_unknown && mp4_.ctx_.file_.max_part_size_ < g_options.max_partsize_default) {
		double x = (double)mp4_.ctx_.file_.max_part_size_ / g_options.max_partsize_default;
		logg(V, "ss: reset to default (from ", mp4_.ctx_.file_.max_part_size_, " ~= ", setprecision(2), x,
		     "*default)\n");
		mp4_.ctx_.file_.max_part_size_ = g_options.max_partsize_default;
	}
	logg(V, "ss: max_part_size_: ", mp4_.ctx_.file_.max_part_size_, "\n");
}

BufferedAtom *Mp4Repairer::setupFile(const string &filename) {
	mp4_.ctx_.scan_.fallback_track_idx_ = mp4_.calcFallbackTrackIdx();
	logg(V, "fallback: ", mp4_.ctx_.scan_.fallback_track_idx_, "\n");

	auto &file_read = mp4_.openFile(filename);

	// TODO: What about multiple mdat?
	logg(V, "calling findMdat on truncated file..\n");
	auto mdat = mp4_.findMdat(file_read);
	logg(V, "reading mdat from truncated file ...\n");

	if (file_read.length() > (1LL << 32)) {
		mp4_.ctx_.file_.broken_is_64_ = true;
		logg(I, "using 64-bit offsets for the broken file\n");
	}
	return mdat;
}

void Mp4Repairer::precomputeAudioSampleSizes() {
	// Precompute audio sample size candidates before clearing reference data.
	// GET_SZ_FN("mp4a") needs these as a fallback when the FFmpeg decoder cannot
	// determine the consumed-byte count (e.g. receive_frame codecs in FFmpeg 7+).
	// likely_sample_sizes_ is not cleared by Track::clear(), so it survives below.
	for (auto &t : mp4_.tracks_) {
		if (t.codec_.name_ != "mp4a" || !t.likely_sample_sizes_.empty()) continue;
		if (t.constant_size_) {
			t.likely_sample_sizes_.push_back(t.constant_size_);
		} else {
			t.likely_sample_sizes_ = collectLikelySizes(t.sizes_);
		}
	}
}

off_t Mp4Repairer::calcStartOffset(BufferedAtom *mdat) {
	if (!g_options.use_chunk_stats) return 0; // offset, not bool

	off_t start_off = 0;
	off_t offset = 0;

	auto first_off_abs = mp4_.ctx_.scan_.first_off_abs_ - mdat->contentStart();
	if (first_off_abs > 0 && mp4_.wouldMatch(WMCfg{.offset = first_off_abs})) {
		dbgg("set start offset via", mp4_.ctx_.scan_.first_off_abs_, first_off_abs);
		offset = first_off_abs;
	} else if (mp4_.ctx_.scan_.first_off_rel_) {
		if (mp4_.wouldMatch(WMCfg{.offset = mp4_.ctx_.scan_.first_off_rel_, .very_first = true})) {
			dbgg("set start offset via", mp4_.ctx_.scan_.first_off_rel_);
			offset = mp4_.ctx_.scan_.first_off_rel_;
		} else {
			mp4_.advanceOffset(start_off, true); // some atom (e.g. wide) might get skipped
			if (start_off &&
			    mp4_.wouldMatch(WMCfg{.offset = start_off + mp4_.ctx_.scan_.first_off_rel_, .very_first = true})) {
				dbgg("set start offset via rel2", start_off, mp4_.ctx_.scan_.first_off_rel_);
				offset = start_off + mp4_.ctx_.scan_.first_off_rel_;
			}
		}
	}

	if (offset) {
		logg(V, "beginning at offset ", mp4_.offToStr(offset), " instead of 0\n");
		mp4_.addUnknownSequence(start_off, offset);
	}
	return offset;
}

void Mp4Repairer::repair(const string &filename) {
	if (handleSpecialModes(filename)) return;

	prepareStats(filename);

	if (mp4_.alreadyRepaired(mp4_.filename_ok_, filename)) return;

	auto mdat = setupFile(filename);

	precomputeAudioSampleSizes();

	mp4_.duration_ = 0;
	for (uint i = 0; i < mp4_.tracks_.size(); i++)
		mp4_.tracks_[i].clear();

	off_t offset = calcStartOffset(mdat);

	while (chkOffset(offset)) {
		if (tryAll(offset)) continue;

		if (!mp4_.ctx_.scan_.unknown_length_) {
			pushBackLastChunk();
			mp4_.setLastTrackIdx(mp4_.idx_free_);
		}

		mp4_.chkDetectionAtImpl(nullptr, nullptr, offset);

		if (g_options.ignore_unknown) {
			if (!g_options.muted && g_options.log_mode < LogMode::V) {
				mute();
			} else if (!g_logger->isNoiseSuppressed() && g_options.log_mode >= LogMode::V && !g_options.dont_omit) {
				logg(V, "unknown sequence -> enabling noise buffer ..\n");
				g_logger->enableNoiseSuppression();
				mute(); // ffmpeg warnings are mostly noise and unhelpful without knowing file offset
			}

			auto step = mp4_.calcStep(offset);
			mp4_.ctx_.scan_.unknown_length_ += step;
			offset += step;
		} else {
			if (g_options.muted) unmute();
			double percentage = (double)100 * offset / mdat->contentSize();
			mdat->file_end_ = mp4_.toAbsOff(offset);
			mdat->length_ = offset + 8;

			logg(E, "unable to find correct codec -> premature end", " (~", setprecision(4), percentage, "%)\n",
			     "       try '-s' to skip unknown sequences\n\n");
			logg(V, "mdat->file_end: ", mdat->file_end_, '\n');

			mp4_.premature_percentage_ = percentage;
			mp4_.premature_end_ = true;
			break;
		}
	}

	if (g_options.muted) unmute();

	if (mp4_.premature_end_)
		report_.onPrematureEnd(mp4_.premature_percentage_);

	if (!mp4_.ctx_.scan_.unknown_sequences_.empty()) {
		std::vector<UnknownSeqDetail> seqs;
		int64_t total_bytes = 0;
		for (auto &[start, length] : mp4_.ctx_.scan_.unknown_sequences_) {
			seqs.push_back({mp4_.offToStr(start), length});
			total_bytes += length;
		}
		double pct = 100.0 * total_bytes / mdat->contentSize();
		report_.onUnknownSequences(std::move(seqs), pct);
	}

	// Collect per-track stats before saveVideo may modify tracks
	{
		std::vector<TrackStat> stats;
		for (const auto &t : mp4_.tracks_) {
			if (t.is_dummy_) continue;
			int64_t kf = t.keyframes_.size();
			stats.push_back({t.codec_.name_, (int64_t)t.getNumSamples(), kf});
		}
		report_.onTrackStats(std::move(stats));
	}

	for (auto &track : mp4_.tracks_)
		track.fixTimes();

	auto filename_fixed = mp4_.getPathRepaired(mp4_.filename_ok_, filename);
	report_.onOutputFile(filename_fixed);
	mp4_.saveVideo(filename_fixed);
}
