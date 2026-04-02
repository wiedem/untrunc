// Repair scan loop implementation for Mp4.
// Mp4::repair() delegates to Mp4Repairer::repair().

#include <iostream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstring>

#include "mp4_repairer.h"
#include "rsv.h" // isPointingAtRtmdHeader, RsvRepairer
#include "codec/avc1/avc-config.h"
#include "codec/avc1/nal.h"
#include "codec/hvc1/hvc-config.h"
#include "codec/hvc1/nal.h"

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

std::optional<std::pair<int, int>> Mp4Repairer::findIdrInAvcc(const uchar *data, int size, int nal_length_size) {
	// Mdat is interleaved: audio chunks precede the first video chunk, and video
	// access units may start with SEI NALs before the IDR slice. Strategy:
	//   - Skip bytes one at a time when the current position does not look like a
	//     valid AVCC NAL start (audio data, misaligned windows).
	//   - When a valid, known H.264 NAL type is found, advance by its full length
	//     to maintain AVCC alignment (so the SEI before the IDR is skipped cleanly).
	//   - For unknown NAL types (audio bytes that happen to parse as valid NAL headers),
	//     advance only 1 byte to avoid jumping past the real IDR.
	int offset = 0;
	while (offset + nal_length_size < size) {
		if (nal_length_size == 4 && data[offset] != 0) {
			offset++;
			continue;
		}
		NalInfo nal(data + offset, size - offset, nal_length_size);
		if (!nal.is_ok || (int)nal.length_ <= 0) {
			offset++;
			continue;
		}
		if (nal.nal_type_ == NAL_IDR_SLICE) return std::make_pair(offset, (int)nal.length_);
		// Standard H.264 NAL types 1-13 and 19: trusted, advance by full NAL length.
		// Unknown types (audio data misparsed as NAL): advance 1 byte only.
		bool known_type = (nal.nal_type_ >= 1 && nal.nal_type_ <= 13) || nal.nal_type_ == 19;
		offset += known_type ? (int)nal.length_ : 1;
	}
	return std::nullopt;
}

std::optional<std::pair<int, int>> Mp4Repairer::findIdrInHvcc(const uchar *data, int size, int nal_length_size) {
	// Same two-tier strategy as findIdrInAvcc, adapted for H.265 two-byte NAL headers.
	// H265NalInfo validates the forbidden_zero_bit, nal_type (0-40), and temporal_id,
	// which makes false positives from audio data rare. All passing non-IDR NALs (VPS,
	// SPS, PPS, SEI, non-IRAP slices) advance by full length; bytes that fail parsing
	// advance one byte at a time.
	int offset = 0;
	while (offset + nal_length_size < size) {
		if (nal_length_size == 4 && data[offset] != 0) {
			offset++;
			continue;
		}
		H265NalInfo nal(data + offset, size - offset, nal_length_size);
		if (!nal.is_ok || (int)nal.length_ <= 0) {
			offset++;
			continue;
		}
		if (nal.nal_type_ == NAL_IDR_W_RADL || nal.nal_type_ == NAL_IDR_N_LP)
			return std::make_pair(offset, (int)nal.length_);
		// All valid H.265 NAL types (0-40) advance by full length.
		offset += (int)nal.length_;
	}
	return std::nullopt;
}

std::optional<std::pair<int, int>> Mp4Repairer::findSpsInHvcc(const uchar *data, int size, int nal_length_size) {
	// Per ISO 14496-15, hev1 streams may carry parameter sets in-band in the mdat.
	// Use the same two-tier scan strategy as findIdrInHvcc: skip invalid positions
	// byte-by-byte, advance by full NAL length for valid H.265 NALs.
	// Stop at the first slice NAL: parameter sets must precede slices in each access unit.
	int offset = 0;
	while (offset + nal_length_size < size) {
		if (nal_length_size == 4 && data[offset] != 0) {
			offset++;
			continue;
		}
		H265NalInfo nal(data + offset, size - offset, nal_length_size);
		if (!nal.is_ok || (int)nal.length_ <= 0) {
			offset++;
			continue;
		}
		if (nal.nal_type_ == H265_NAL_SPS) return std::make_pair(offset, (int)nal.length_);
		if (h265IsSlice(nal.nal_type_)) break;
		offset += (int)nal.length_;
	}
	return std::nullopt;
}

std::optional<std::pair<int, int>> Mp4Repairer::parseSpsH265ProfileLevel(const uchar *nal, int nal_bytes) {
	// H.265 SPS RBSP layout (after the 2-byte NAL header):
	//   byte 0: sps_video_parameter_set_id(4)+sps_max_sublayers_minus1(3)+sps_temporal_id_nesting(1)
	//   byte 1: general_profile_space(2)+general_tier_flag(1)+general_profile_idc(5)
	//   bytes 2-5: general_profile_compatibility_flags
	//   bytes 6-11: constraint flags
	//   byte 12: general_level_idc
	// Emulation prevention bytes (00 00 03 -> 00 00) are stripped before reading fixed positions.
	if (nal_bytes < 15) return std::nullopt;
	constexpr int kRbspNeeded = 13;
	uint8_t rbsp[kRbspNeeded];
	int n = 0;
	for (int i = 2; i < nal_bytes && n < kRbspNeeded; i++) {
		if (n >= 2 && rbsp[n - 2] == 0 && rbsp[n - 1] == 0 && nal[i] == 0x03 && i + 1 < nal_bytes && nal[i + 1] <= 0x03)
			continue; // skip emulation prevention byte
		rbsp[n++] = nal[i];
	}
	if (n < kRbspNeeded) return std::nullopt;
	return std::make_pair(rbsp[1] & 0x1F, (int)rbsp[12]);
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

static std::string fmtH264Profile(int profile_idc) {
	const char *name = avcodec_profile_name(AV_CODEC_ID_H264, profile_idc);
	return name ? name : std::to_string(profile_idc);
}

static std::string fmtH264Level(int level_idc) {
	return "L" + std::to_string(level_idc / 10) + "." + std::to_string(level_idc % 10);
}

static std::string fmtH265Profile(int profile_idc) {
	const char *name = avcodec_profile_name(AV_CODEC_ID_HEVC, profile_idc);
	return name ? name : std::to_string(profile_idc);
}

static std::string fmtH265Level(int level_idc) {
	// H.265 level_idc = level * 30 (e.g. 93 = L3.1, 120 = L4.0, 150 = L5.0)
	return "L" + std::to_string(level_idc / 30) + "." + std::to_string((level_idc % 30) / 3);
}

void Mp4Repairer::checkRefCompatibility() {
	for (const auto &t : mp4_.tracks_) {
		if (t.codec_.name_ != "avc1") continue;
		const AvcConfig *ref_cfg = t.codec_.avc_config_.get();
		if (!ref_cfg || !ref_cfg->is_ok) continue;

		// Scan the broken file's bytes for an avcC box. This works when the moov atom
		// precedes the mdat (faststart format), which keeps the moov intact even if the
		// mdat is truncated. Non-faststart files with a truncated moov cannot be checked.
		FileRead &file = *mp4_.ctx_.file_.file_;
		int scan_limit = (int)std::min(file.length(), (off_t)(128 << 10)); // 128 KB
		if (scan_limit < 16) continue;
		const uchar *data = file.getFragment(0, scan_limit);
		if (!data) continue;

		const uchar *avcc_payload = nullptr;
		for (int i = 0; i + 8 < scan_limit; i++) {
			if (data[i] == 'a' && data[i + 1] == 'v' && data[i + 2] == 'c' && data[i + 3] == 'C') {
				avcc_payload = data + i + 4;
				break;
			}
		}
		auto broken_cfg = avcc_payload
		                      ? AvcConfig::fromAvcCPayload(avcc_payload, scan_limit - (int)(avcc_payload - data))
		                      : std::nullopt;

		if (!broken_cfg) {
			logg(V, "checkRefCompatibility: no avcC found in broken file\n");
			continue;
		}

		if (broken_cfg->profile_idc == ref_cfg->profile_idc && broken_cfg->level_idc == ref_cfg->level_idc) continue;

		auto *par = t.codec_.av_codec_params_;
		string ref_res = par ? (std::to_string(par->width) + "x" + std::to_string(par->height) + " ") : "";
		string ref_str =
		    ref_res + "H.264 " + fmtH264Profile(ref_cfg->profile_idc) + " " + fmtH264Level(ref_cfg->level_idc);
		string broken_str =
		    "H.264 " + fmtH264Profile(broken_cfg->profile_idc) + " " + fmtH264Level(broken_cfg->level_idc);
		logg(ET, "reference and broken file are incompatible (wrong reference file?):\n", "  reference:   ", ref_str,
		     "\n", "  broken file: ", broken_str, "\n");
	}

	for (const auto &t : mp4_.tracks_) {
		if (t.codec_.name_ != "hvc1" && t.codec_.name_ != "hev1") continue;
		const HvcConfig *ref_cfg = t.codec_.hvc_config_.get();
		if (!ref_cfg || !ref_cfg->is_ok) continue;

		// Scan first 128 KB of broken file for hvcC box (same strategy as H.264 avcC scan).
		// Works for faststart files (moov before mdat). Non-faststart files skip the check.
		FileRead &file = *mp4_.ctx_.file_.file_;
		int scan_limit = (int)std::min(file.length(), (off_t)(128 << 10));
		if (scan_limit < 16) continue;
		const uchar *data = file.getFragment(0, scan_limit);
		if (!data) continue;

		const uchar *hvcc_payload = nullptr;
		for (int i = 0; i + 8 < scan_limit; i++) {
			if (data[i] == 'h' && data[i + 1] == 'v' && data[i + 2] == 'c' && data[i + 3] == 'C') {
				hvcc_payload = data + i + 4;
				break;
			}
		}
		auto broken_cfg = hvcc_payload
		                      ? HvcConfig::fromHvcCPayload(hvcc_payload, scan_limit - (int)(hvcc_payload - data))
		                      : std::nullopt;

		if (!broken_cfg) {
			logg(V, "checkRefCompatibility: no hvcC found in broken file\n");
			continue;
		}

		if (broken_cfg->profile_idc == ref_cfg->profile_idc && broken_cfg->level_idc == ref_cfg->level_idc) continue;

		auto *par = t.codec_.av_codec_params_;
		string ref_res = par ? (std::to_string(par->width) + "x" + std::to_string(par->height) + " ") : "";
		string ref_str =
		    ref_res + "H.265 " + fmtH265Profile(ref_cfg->profile_idc) + " " + fmtH265Level(ref_cfg->level_idc);
		string broken_str =
		    "H.265 " + fmtH265Profile(broken_cfg->profile_idc) + " " + fmtH265Level(broken_cfg->level_idc);
		logg(ET, "reference and broken file are incompatible (wrong reference file?):\n", "  reference:   ", ref_str,
		     "\n", "  broken file: ", broken_str, "\n");
	}
}

void Mp4Repairer::verifyCompatByDecode() {
	if (!g_options.verify_compat_mb) return;

	for (const auto &t : mp4_.tracks_) {
		if (t.codec_.name_ != "avc1") continue;
		const AvcConfig *ref_cfg = t.codec_.avc_config_.get();
		auto *ref_par = t.codec_.av_codec_params_;
		if (!ref_cfg || !ref_par) continue;

		auto *mdat = mp4_.ctx_.file_.mdat_.get();
		int64_t scan_limit = (int64_t)g_options.verify_compat_mb << 20;
		int scan_bytes = (int)std::min({mdat->contentSize(), scan_limit, (int64_t)INT_MAX});
		if (scan_bytes < ref_cfg->nal_length_size + 1) continue;

		const uchar *data = mdat->getFragmentIf(0, scan_bytes);
		if (!data) continue;

		auto idr = findIdrInAvcc(data, scan_bytes, ref_cfg->nal_length_size);
		if (!idr) {
			logg(W, "--verify-compat: no IDR frame found in first ", g_options.verify_compat_mb,
			     "MB of broken file -- reference compatibility not verified\n");
			continue;
		}

		// Decode the IDR using the reference's codec parameters (SPS/PPS in extradata).
		// If the broken file was encoded with different parameters, the decode will fail
		// because its slice data is inconsistent with the reference's SPS.
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) continue;
		AVCodecContext *avctx = avcodec_alloc_context3(codec);
		if (!avctx) continue;
		if (avcodec_parameters_to_context(avctx, ref_par) < 0) {
			avcodec_free_context(&avctx);
			continue;
		}
		if (avcodec_open2(avctx, codec, nullptr) < 0) {
			avcodec_free_context(&avctx);
			continue;
		}

		AVPacket *pkt = av_packet_alloc();
		if (!pkt) {
			avcodec_free_context(&avctx);
			continue;
		}
		if (av_new_packet(pkt, idr->second) < 0) {
			av_packet_free(&pkt);
			avcodec_free_context(&avctx);
			continue;
		}
		memcpy(pkt->data, data + idr->first, idr->second);
		pkt->flags = AV_PKT_FLAG_KEY;

		// Send IDR packet, then flush. H.264 can buffer frames, so a flush is needed
		// to ensure the decoder outputs any buffered frame.
		bool decode_ok = false;
		if (avcodec_send_packet(avctx, pkt) >= 0 && avcodec_send_packet(avctx, nullptr) >= 0) {
			AVFrame *frame = av_frame_alloc();
			if (frame) {
				decode_ok = avcodec_receive_frame(avctx, frame) >= 0;
				av_frame_free(&frame);
			}
		}

		av_packet_free(&pkt);
		avcodec_free_context(&avctx);

		if (!decode_ok) {
			string ref_str = to_string(ref_par->width) + "x" + to_string(ref_par->height) + " H.264 " +
			                 fmtH264Profile(ref_cfg->profile_idc) + " " + fmtH264Level(ref_cfg->level_idc);
			logg(ET, "--verify-compat: broken file is not compatible with reference\n", "  reference:    ", ref_str,
			     "\n", "  (IDR frame decode with reference parameters failed -- wrong reference file?)\n");
		} else {
			logg(V, "--verify-compat: IDR decoded OK, reference appears compatible\n");
		}
		break; // first H.264 track verified
	}

	// H.265: two-step check.
	//
	// Step 1 (in-band SPS): Per ISO 14496-15, hev1 streams may carry VPS/SPS/PPS
	//   in-band in the mdat. Many cameras (GoPro, DJI, Sony, etc.) repeat parameter
	//   sets before each IDR frame. If found, profile_idc and level_idc are extracted
	//   directly from the SPS RBSP and compared with the reference hvcC values.
	//
	// Step 2 (extended hvcC scan): fallback for files without in-band PSets. Scans the
	//   broken file from 128 KB to verify_compat_mb, catching large faststart files
	//   where the moov atom (containing hvcC) starts beyond checkRefCompatibility's
	//   128 KB limit.
	for (const auto &t : mp4_.tracks_) {
		if (t.codec_.name_ != "hvc1" && t.codec_.name_ != "hev1") continue;
		const HvcConfig *ref_cfg = t.codec_.hvc_config_.get();
		auto *ref_par = t.codec_.av_codec_params_;
		if (!ref_cfg || !ref_cfg->is_ok) continue;

		int64_t scan_limit = (int64_t)g_options.verify_compat_mb << 20;
		bool checked = false;

		// Step 1: scan mdat for in-band SPS (hev1 with repeat-headers)
		{
			auto *mdat = mp4_.ctx_.file_.mdat_.get();
			int scan_bytes = (int)std::min({mdat->contentSize(), scan_limit, (int64_t)INT_MAX});
			if (scan_bytes >= ref_cfg->nal_length_size + 2) {
				const uchar *data = mdat->getFragmentIf(0, scan_bytes);
				auto sps_loc = data ? findSpsInHvcc(data, scan_bytes, ref_cfg->nal_length_size) : std::nullopt;
				if (sps_loc) {
					const uchar *nal = data + sps_loc->first + ref_cfg->nal_length_size;
					int nal_bytes = sps_loc->second - ref_cfg->nal_length_size;
					auto pl = parseSpsH265ProfileLevel(nal, nal_bytes);
					if (pl) {
						checked = true;
						if (pl->first != ref_cfg->profile_idc || pl->second != ref_cfg->level_idc) {
							string ref_res =
							    ref_par ? (to_string(ref_par->width) + "x" + to_string(ref_par->height) + " ") : "";
							string ref_str = ref_res + "H.265 " + fmtH265Profile(ref_cfg->profile_idc) + " " +
							                 fmtH265Level(ref_cfg->level_idc);
							string broken_str = "H.265 " + fmtH265Profile(pl->first) + " " + fmtH265Level(pl->second);
							logg(ET, "--verify-compat: broken file is not compatible with reference:\n",
							     "  reference:    ", ref_str, "\n", "  broken file:  ", broken_str, "\n",
							     "  (H.265 SPS from broken file does not match reference -- wrong reference "
							     "file?)\n");
						} else {
							logg(V, "--verify-compat: H.265 SPS parsed OK, reference appears compatible\n");
						}
					}
				}
			}
		}

		// Step 2: extended hvcC scan (fallback when no in-band SPS found).
		// checkRefCompatibility() already scanned [0, kFastCheckBytes): if it found an hvcC
		// with a mismatch it would have aborted, so we only need to search beyond that boundary.
		if (!checked) {
			FileRead &file = *mp4_.ctx_.file_.file_;
			int scan_bytes = (int)std::min(file.length(), scan_limit);
			static constexpr int kFastCheckBytes = 128 << 10;
			if (scan_bytes > kFastCheckBytes) {
				const uchar *data = file.getFragment(0, scan_bytes);
				if (data) {
					const uchar *hvcc_payload = nullptr;
					for (int i = kFastCheckBytes; i + 8 < scan_bytes; i++) {
						if (data[i] == 'h' && data[i + 1] == 'v' && data[i + 2] == 'c' && data[i + 3] == 'C') {
							hvcc_payload = data + i + 4;
							break;
						}
					}
					if (!hvcc_payload) {
						logg(V, "--verify-compat: no hvcC found beyond 128 KB in broken file\n");
					} else {
						auto broken_cfg =
						    HvcConfig::fromHvcCPayload(hvcc_payload, scan_bytes - (int)(hvcc_payload - data));
						if (broken_cfg) {
							if (broken_cfg->profile_idc == ref_cfg->profile_idc &&
							    broken_cfg->level_idc == ref_cfg->level_idc) {
								logg(V, "--verify-compat: H.265 hvcC found beyond 128 KB, reference appears "
								        "compatible\n");
							} else {
								auto *par = t.codec_.av_codec_params_;
								string ref_res =
								    par ? (to_string(par->width) + "x" + to_string(par->height) + " ") : "";
								string ref_str = ref_res + "H.265 " + fmtH265Profile(ref_cfg->profile_idc) + " " +
								                 fmtH265Level(ref_cfg->level_idc);
								string broken_str = "H.265 " + fmtH265Profile(broken_cfg->profile_idc) + " " +
								                    fmtH265Level(broken_cfg->level_idc);
								logg(ET, "--verify-compat: broken file is not compatible with reference:\n",
								     "  reference:    ", ref_str, "\n", "  broken file:  ", broken_str, "\n",
								     "  (hvcC found beyond 128 KB -- wrong reference file?)\n");
							}
						}
					}
				}
			}
		}

		break; // first H.265 track verified
	}
}

void Mp4Repairer::repair(const string &filename) {
	if (handleSpecialModes(filename)) return;

	prepareStats(filename);

	if (mp4_.alreadyRepaired(mp4_.filename_ok_, filename)) return;

	auto mdat = setupFile(filename);

	checkRefCompatibility();
	verifyCompatByDecode();
	precomputeAudioSampleSizes();

	mp4_.duration_ = 0;
	for (uint i = 0; i < mp4_.tracks_.size(); i++)
		mp4_.tracks_[i].clear();

	off_t offset = calcStartOffset(mdat);

	auto content_size = mdat->contentSize();

	while (chkOffset(offset)) {
		report_.onProgress(offset, content_size);
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

	if (mp4_.premature_end_) report_.onPrematureEnd(mp4_.premature_percentage_);

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
