#include "codec.h"

#include <iostream>
#include <vector>

extern "C" {
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include "ff_internal.h"
}

#include "util/common.h"
#include "atom/atom.h"
#include "avc1/avc1.h"
#include "avc1/avc-config.h"
#include "hvc1/hvc1.h"
#include "core/mp4.h"

using namespace std;

extern map<string, bool (*)(Codec *, const uchar *, uint)> dispatch_match;
extern map<string, bool (*)(Codec *, const uchar *, uint)> dispatch_strict_match;
extern map<string, int (*)(Codec *, const uchar *, uint)> dispatch_get_size;

bool Codec::twos_is_sowt = false;
Codec::Codec() = default;
Codec::Codec(AVCodecParameters *c) : av_codec_params_(c) {}

Codec::~Codec() {
	av_packet_free(&decode_packet_);
	av_frame_free(&decode_frame_);
}

Codec::Codec(Codec &&o) noexcept
    : name_(std::move(o.name_)), av_codec_params_(o.av_codec_params_),
      av_codec_context_(std::exchange(o.av_codec_context_, nullptr)), avc_config_(std::move(o.avc_config_)),
      was_keyframe_(o.was_keyframe_), was_bad_(o.was_bad_), audio_duration_(o.audio_duration_),
      should_dump_(o.should_dump_), chk_for_twos_(o.chk_for_twos_), strictness_lvl_(o.strictness_lvl_),
      cur_off_(o.cur_off_), ss_stats_(o.ss_stats_), track_idx_(o.track_idx_), fdsc_pkt_idx_(o.fdsc_pkt_idx_),
      mp4_(o.mp4_), decode_packet_(std::exchange(o.decode_packet_, nullptr)),
      decode_frame_(std::exchange(o.decode_frame_, nullptr)), match_fn_(o.match_fn_),
      match_strict_fn_(o.match_strict_fn_), get_size_fn_(o.get_size_fn_) {}

Codec &Codec::operator=(Codec &&o) noexcept {
	if (this != &o) {
		av_packet_free(&decode_packet_);
		av_frame_free(&decode_frame_);
		name_ = std::move(o.name_);
		av_codec_params_ = o.av_codec_params_;
		av_codec_context_ = std::exchange(o.av_codec_context_, nullptr);
		avc_config_ = std::move(o.avc_config_);
		was_keyframe_ = o.was_keyframe_;
		was_bad_ = o.was_bad_;
		audio_duration_ = o.audio_duration_;
		should_dump_ = o.should_dump_;
		chk_for_twos_ = o.chk_for_twos_;
		strictness_lvl_ = o.strictness_lvl_;
		cur_off_ = o.cur_off_;
		ss_stats_ = o.ss_stats_;
		track_idx_ = o.track_idx_;
		mp4_ = o.mp4_;
		match_fn_ = o.match_fn_;
		match_strict_fn_ = o.match_strict_fn_;
		get_size_fn_ = o.get_size_fn_;
		decode_packet_ = std::exchange(o.decode_packet_, nullptr);
		decode_frame_ = std::exchange(o.decode_frame_, nullptr);
	}
	return *this;
}

void Codec::initAVCodec() {
	auto c = av_codec_params_;
	auto av_codec = avcodec_find_decoder(c->codec_id);

	if (!av_codec) {
		auto codec_type = av_get_media_type_string(c->codec_type);
		auto codec_name = avcodec_get_name(c->codec_id);
		logg(V, "FFmpeg does not support codec: <", codec_type, ", ", codec_name, ">\n");
		return;
	}

	av_codec_context_ = avcodec_alloc_context3(av_codec);
	avcodec_parameters_to_context(av_codec_context_, c);

	if (avcodec_open2(av_codec_context_, av_codec, nullptr) < 0) throw std::runtime_error("Could not open codec: ?");
}

void Codec::initOnce() {
	static bool did_once = false;
	if (did_once) return;
	did_once = true;

	assertt(dispatch_get_size.count("mp4a"));
	dispatch_get_size["sawb"] = dispatch_get_size["mp4a"];
	map<string, vector<string>> alias = {
	    {"hvc1", {"hev1"}},
	    {"ap4x", {"apch", "apcn", "apcs", "apco", "ap4h"}}, // Apple ProRes
	};
	for (auto &p : alias) {
		for (auto &alias : p.second) {
			dispatch_get_size[alias] = dispatch_get_size[p.first];
			dispatch_match[alias] = dispatch_match[p.first];
		}
	}
}

Track *Codec::getTrack() {
	assertt(track_idx_ >= 0, track_idx_);
	auto r = &mp4_->tracks_[track_idx_];
	assertt(r->codec_.name_ == name_, track_idx_, r->codec_.name_, name_);
	return r;
}

void Codec::onTrackRealloc(int track_idx) {
	track_idx_ = track_idx;
	auto t = getTrack();
	ss_stats_ = &t->ss_stats_; // hopefully Track t does not reallocate anymore
}

void Codec::parseOk(Atom *trak) {
	Atom *stsd = trak->atomByName("stsd");
	int entries = stsd->readInt(4);
	if (entries != 1) throw std::runtime_error("Multiplexed stream! Not supported");

	name_ = stsd->getString(12, 4);
	name_ = name_.c_str(); // might be smaller than 4

	if (contains({"mp4a", "sawb", "mp4v"}, name_)) initAVCodec();

	match_fn_ = dispatch_match[name_];
	match_strict_fn_ = dispatch_strict_match[name_];
	get_size_fn_ = dispatch_get_size[name_];

	if (name_ == "avc1") {
		avc_config_ = std::make_unique<AvcConfig>(stsd);
		if (!avc_config_->is_ok)
			logg(W, "avcC was not decoded correctly\n");
		else
			logg(V, "avcC got decoded\n");
	} else if (name_ == "sowt")
		Codec::twos_is_sowt = true;
}

bool Codec::isSupported() {
	return match_fn_ && get_size_fn_;
}

// AVC Access Unit Delimiter (NAL type 9) patterns: size=2 bytes (0x00000002),
// followed by NAL byte 0x09 and primary_pic_type field in the high bits.
static const int AVC_AUD_IDR = 0x09300000;     // primary_pic_type 3: IDR/SP slices
static const int AVC_AUD_NON_IDR = 0x09100000; // primary_pic_type 1: non-IDR slices

// clang-format off
#define MATCH_FN(codec)  {codec, [](Codec* self __attribute__((unused)), \
	const uchar* start __attribute__((unused)), uint s __attribute__((unused))) -> bool

map<string, bool(*) (Codec*, const uchar*, uint)> dispatch_strict_match {
	MATCH_FN("avc1") {
		int s2 = swap32(((int *)start)[1]);
		if (self->strictness_lvl_ > 0) {
			return s == 0x00000002 && (s2 == AVC_AUD_IDR || s2 == AVC_AUD_NON_IDR);
		}
		int s1 = s;
//		return s1 == 0x01 || s1 == 0x02 || s1 == 0x03;
		if (s1 == 0x01 || (s1>>8) == 0x01 || s1 == 0x02 || s1 == 0x03) return true;

		// vlc 2.1.5 stream output
		if (s1 >> 16 == 0 && s2 >> 16 == 0x619a) return true;
		if (s1 == 0x00000017 && s2 == 0x674d0020) return true;
		return false;
	}},
    MATCH_FN("hvc1") {
		if (start[0] != 0x00 || start[5] != 0x01) return false;
		if (start[4] != 0x02 && start[4] != 0x26 && start[4] != 0x00) return false;
		return true;
	}},
	MATCH_FN("fdsc") {
		if (string((char*)start, 2) != "GP") return false;
		if (start[8] || start[9]) return false;
		return true;
	}},
	MATCH_FN("mp4a") {
		return false;
//		return (s>>16) == 0x210A;  // this needs to be improved
	}},
	// MATCH_FN("tmcd") { return false; }},
	MATCH_FN("mebx") { return false; }},
};
// clang-format on

// only match if we are certain -> less false positives
// you might want to tweak these values..
bool Codec::matchSampleStrict(const uchar *start) {
	uint s_raw;
	memcpy(&s_raw, start, sizeof(s_raw));
	uint s = swap32(s_raw); // big endian
	if (!match_strict_fn_) return matchSample(start);
	return match_strict_fn_(this, start, s);
}

bool Codec::looksLikeTwosOrSowt(const uchar *start) {
	if (Codec::twos_is_sowt) start += 1;
	int d1 = abs(start[4] - start[2]), d2 = abs(start[6] - start[4]), d3 = abs(start[8] - start[6]),
	    d4 = abs(start[10] - start[8]), d5 = abs(start[12] - start[10]);
	int cnt = (8 < d1 && d1 < 0xf0) + (8 < d2 && d2 < 0xf0) + (8 < d3 && d3 < 0xf0) + (8 < d4 && d4 < 0xf0) +
	          (8 < d5 && d5 < 0xf0);
	//	if (cnt <= 1) {
	if (cnt == 0) {
		if (g_options.log_mode >= LogMode::V) {
			if (Codec::twos_is_sowt) start -= 1;
			printBuffer(start, 16);
			cout << "avc1: detected sowt..\n";
		}
		return true;
	}
	return false;
}

// clang-format off
map<string, bool(*) (Codec*, const uchar*, uint)> dispatch_match {
	MATCH_FN("avc1") {
		//this works only for a very specific kind of video
		//#define SPECIAL_VIDEO
#ifdef SPECIAL_VIDEO
		int s2 = swap32(((int *)start)[1]);
		return (s != 0x00000002 || (s2 != 0x09300000 && s2 != 0x09100000)) return false;
		return true;
#endif

		if (self->chk_for_twos_ && Codec::looksLikeTwosOrSowt(start)) return false;

		//TODO use the first byte of the nal: forbidden bit and type!
		int nal_type = (start[4] & 0x1f);
		//the other values are really uncommon on cameras...
		if(nal_type > 21
		   && nal_type != 31  // sometimes used for metadata
		   ) {
			//		if(nal_type != 1 && nal_type != 5 && nal_type != 6 && nal_type != 7 &&
			//				nal_type != 8 && nal_type != 9 && nal_type != 10 && nal_type != 11 && nal_type != 12) {
			logg(V, "avc1: no match because of nal type: ", nal_type, '\n');
			return false;
		}
		//if nal is equal 7, the other fragments (starting with nal type 7)
		//should be part of the same packet
		//(we cannot recover time information, remember)
		if(start[0] == 0) {
			logg(V, "avc1: Match with 0 header\n");
			return true;
		}
		logg(V, "avc1: failed for no particular reason\n");
		return false;
	}},

    MATCH_FN("mp4a") {
		if(s > 1000000) {
			logg(V, "mp4a: Success because of large s value\n");
			return true;
		}
		//horrible hack... these values might need to be changed depending on the file
		if((start[4] == 0xee && start[5] == 0x1b) ||
		   (start[4] == 0x3e && start[5] == 0x64)) {
			logg(W, "mp4a: Success because of horrible hack.\n");
			return true;
		}

		if(start[0] == 0) {
			logg(V, "Failure because of NULL header\n");
			return false;
		}
		logg(V, "Success for no particular reason....\n");
		return true;

		/* THIS is true for mp3...
		//from http://www.mp3-tech.org/ programmers corner
		//first 11 bits as 1,
		//       2 bits as version (mpeg 1, 2 etc)
		//       2 bits layer (I, II, III)
		//       1 bit crc protection
		//       4 bits bitrate (could be variable if VBR)
		//       2 bits sample rate
		//       1 bit padding (might be variable)
		//       1 bit privete (???)
		//       2 bit channel mode (constant)
		//       2 bit mode extension
		//       1 bit copyright
		//       1 bit original
		//       2 bit enfasys.
		//in practice we have
		// 11111111111 11 11 1 0000 11 0 0 11 11 1 1 11 mask
		// 1111 1111 1111 1111 0000 1100 1111 1111 or 0xFFFF0CFF
		reverse(s);
		if(s & 0xFFE00000 != 0xFFE0000)
			return false;
		return true; */
	}},

	MATCH_FN("mp4v") { //as far as I know keyframes are 1b3, frames are 1b6 (ISO/IEC 14496-2, 6.3.4 6.3.5)
		return s == 0x1b3 || s == 0x1b6;
	}},

	MATCH_FN("alac") {
		int t = swap32(*(int *)(start + 4));
		t &= 0xffff0000;

		if(s == 0 && t == 0x00130000) return true;
		if(s == 0x1000 && t == 0x001a0000) return true;
		return false;
	}},

	MATCH_FN("samr") {
		return start[0] == 0x3c;
	}},
	MATCH_FN("apcn") {
		return memcmp(start, "icpf", 4) == 0;
	}},
	MATCH_FN("sawb") {
		return start[0] == 0x44;
	}},
// 	MATCH_FN("tmcd") {  // GoPro timecode .. hardcoded in Mp4::GetMatches
// 		return false;
// //		return !tmcd_seen_ && start[0] == 0 && mp4_->wouldMatch(start+4, "tmcd");
// 	}},
	MATCH_FN("gpmd") {  // GoPro timecode, 4 bytes (?)
		vector<string> fourcc = {"DEVC", "DVID", "DVNM", "STRM", "STNM", "RMRK",  "SCAL",
		                         "SIUN", "UNIT", "TYPE", "TSMP", "TIMO", "EMPT"};
		return contains(fourcc, string((char*)start, 4));
	}},
	MATCH_FN("fdsc") {  // GoPro recovery.. anyone knows more?
		return string((char*)start, 2) == "GP";
	}},
	MATCH_FN("hvc1") {
		// no idea if this generalizes well..
		// 00...... ..01....
		return start[0] == 0x00 && start[5] == 0x01;
	}},
	MATCH_FN("mebx") {
//		return s == 8 || s == 10 || s == 100;
		return s < 200;
	}},
	MATCH_FN("icod") {
		// 0116....
		return start[0] == 1 && start[1] == 22;
	}},
	MATCH_FN("ap4x") {
		return string((char*)start+4, 4) == "icpf";
	}},
	MATCH_FN("camm") {
		return (start[0] == 0 && start[1] == 0) || (start[3] == 0 && start[2] < 7);
	}},
	MATCH_FN("jpeg") {
		return s >> 16 == 0xFFD8;
	}},

	/*
	MATCH_FN("twos") {
		//weird audio codec: each packet is 2 signed 16b integers.
		cerr << "This audio codec is EVIL, there is no hope to guess it.\n";
		return true;
	}},
	MATCH_FN("in24") { //it's a codec id, in a case I found a pcm_s24le (little endian 24 bit) No way to know it's length.
		return true;
	}},
	MATCH_FN("sowt") {
		cerr << "Sowt is just  raw data, no way to guess length (unless reliably detecting the other codec start)\n";
		return false;
	}},
	MATCH_FN("lpcm") {
		// This is not trivial to detect because it is just
		// the audio waveform encoded as signed 16-bit integers.
		// For now, just test that it is not "apcn" video:
		return memcmp(start, "icpf", 4) != 0;
	}},
	*/
};
// clang-format on

bool Codec::matchSample(const uchar *start) {
	if (match_fn_) {
		uint s_raw;
		memcpy(&s_raw, start, sizeof(s_raw));
		uint s = swap32(s_raw); // big endian
		return match_fn_(this, start, s);
	}
	return false;
}

// clang-format off
#define GET_SZ_FN(codec)  {codec, [](Codec* self __attribute__((unused)), \
	const uchar* start __attribute__((unused)), uint maxlength __attribute__((unused))) -> int

inline int untr_decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt) {
	const FFCodec *c = ffcodec(avctx->codec);
	// FF_CODEC_CB_TYPE_RECEIVE_FRAME codecs use the send/receive API. Decoding here
	// with pkt->size = maxlength would be misleading because maxlength carries no
	// frame-boundary information. The consumed byte count is always -1 regardless.
	// Callers that need the frame size must use the candidate-loop approach (size
	// from reference stats) or chunk-level prediction. Returning -1 with
	// got_frame=0 is the safe path.
	if (c->cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME) {
		*got_frame = 0;
		return -1;
	}
	return c->cb.decode(avctx, frame, got_frame, pkt);
}

map<string, int(*) (Codec*, const uchar*, uint maxlength)> dispatch_get_size {
	GET_SZ_FN("mp4a") {
		// In dynamic stats (chunk) mode, audio is handled at chunk level by fitChunk.
		// Calling the decoder here with arbitrary (possibly non-AAC) data corrupts
		// the decoder state over repeated calls. Return -1 so tryMatch skips mp4a
		// and chunk prediction handles audio exclusively.
		if (g_options.use_chunk_stats) return -1;

		maxlength = min(g_options.max_buf_sz_needed, maxlength);

		if (!self->decode_packet_) self->decode_packet_ = av_packet_alloc();
		if (!self->decode_frame_) self->decode_frame_ = av_frame_alloc();
		auto *packet = self->decode_packet_;
		auto *frame = self->decode_frame_;

		packet->data = const_cast<uchar*>(start);
		packet->size = maxlength;
		int got_frame = 0;

		int consumed = untr_decode(self->av_codec_context_, frame, &got_frame, packet);

		// Reset decoder state after every attempt. The scanner calls getSize for
		// nearly every mdat offset (mp4a matchSample is permissive), so most calls
		// land on non-AAC data. Without a flush, accumulated bad state from those
		// failed attempts prevents the decoder from recognising valid frames.
		if (self->av_codec_context_) avcodec_flush_buffers(self->av_codec_context_);

		self->audio_duration_ = frame->nb_samples;
		logg(V, "nb_samples: ", self->audio_duration_, '\n');

		self->was_bad_ = (!got_frame || nb_channels(self->av_codec_params_) != nb_channels(frame));
		if (self->was_bad_) {
			logg(V, "got_frame: ", got_frame, '\n');
			logg(V, "channels: ", nb_channels(self->av_codec_params_), ", ", nb_channels(frame), '\n');
		}

		av_frame_unref(frame);

		if (consumed >= 1) return consumed;

		// Fallback for FFmpeg 8+ where cb.decode always returns consumed=0.
		// For each candidate frame size from the reference file, use
		// avcodec_send_packet + NULL drain + avcodec_receive_frame to validate.
		// findSizeWithContinuation is NOT used here because mp4a matchSample is
		// permissive (returns true for almost any input), which makes the
		// continuation check vacuously true and produces massive false-positive counts.
		// Instead, the decoder itself is used as the gatekeeper: only valid AAC
		// bitstreams produce output. Returns -1 when no candidate validates.
		auto* track = self->getTrack();
		if (self->av_codec_context_ && !track->likely_sample_sizes_.empty()) {
			for (int sz : track->likely_sample_sizes_) {
				if (sz <= 0 || static_cast<uint>(sz) > maxlength) continue;
				avcodec_flush_buffers(self->av_codec_context_);
				packet->data = const_cast<uchar*>(start);
				packet->size = sz;
				if (avcodec_send_packet(self->av_codec_context_, packet) != 0) continue;
				// Send NULL to drain the one-frame decode delay.
				if (avcodec_send_packet(self->av_codec_context_, nullptr) != 0) {
					av_frame_unref(frame);
					avcodec_flush_buffers(self->av_codec_context_);
					continue;
				}
				if (avcodec_receive_frame(self->av_codec_context_, frame) == 0) {
					bool channels_ok = nb_channels(self->av_codec_params_) == nb_channels(frame);
					self->audio_duration_ = frame->nb_samples;
					self->was_bad_ = !channels_ok;
					av_frame_unref(frame);
					avcodec_flush_buffers(self->av_codec_context_);
					if (channels_ok) {
						logg(V, "mp4a: validated frame via send/drain at size ", sz, '\n');
						return sz;
					}
				} else {
					av_frame_unref(frame);
					avcodec_flush_buffers(self->av_codec_context_);
				}
			}
			logg(V, "mp4a: all candidates rejected by decoder\n");
			self->was_bad_ = true;
			return -1;
		}

		return consumed;
	}},

    {"avc1", getSizeAvc1},
    {"hvc1", getSizeHvc1},
//    GET_SZ_FN("avc1") {
		//        AVFrame *frame = av_frame_alloc();
		//        if(!frame)
		//            throw "Could not create AVFrame";
		//        AVPacket avp;
		//        av_init_packet(&avp);

		//        int got_frame;
		//        avp.data=(uchar *)(start);
		//        avp.size = maxlength;

		/*
		int consumed = avcodec_decode_video2(context, frame, &got_frame, &avp);
		cout << "Consumed: " << consumed << endl;
		av_freep(&frame);
		cout << "Consumed: " << consumed << endl;

		return consumed;
		*/
		//first 4 bytes are the length, then the nal starts.
		//ref_idc !=0 per unit_type = 5
		//ref_idc == 0 per unit_type = 6, 9, 10, 11, 12
//	}},

	GET_SZ_FN("samr") { //lenght is multiple of 32, we split packets.
		return 32;
	}},
	GET_SZ_FN("apcn") {
		return swap32(*(int *)start);
	}},
// 	GET_SZ_FN("tmcd") {  // GoPro timecode, always 4 bytes, only pkt-idx 4 (?)
// //		tmcd_seen_ = true;
// 		return 4;
// 	}},
	GET_SZ_FN("fdsc") {  // GoPro recovery
		// TODO: How is this track used for recovery?

		self->fdsc_pkt_idx_++;
		if (self->fdsc_pkt_idx_ == 0) {
			for(auto pos=start+4; maxlength; pos+=4, maxlength-=4) {
				if (string((char*)pos, 2) == "GP") return pos-start;
			}
		}
		else if (self->fdsc_pkt_idx_ == 1) return self->mp4_->getTrack("fdsc").getOrigSize(1);  // probably 152 ?
		return 16;
	}},
	GET_SZ_FN("gpmd") {  // GoPro meta data, see 'gopro/gpmf-parser'
		int s2 = swap32(((int *)start)[1]);
		int num = (s2 & 0x0000ffff);
		return num + 8;
	}},
	GET_SZ_FN("mp4v") {
		if (!self->decode_packet_) self->decode_packet_ = av_packet_alloc();
		if (!self->decode_frame_) self->decode_frame_ = av_frame_alloc();
		auto *packet = self->decode_packet_;
		auto *frame = self->decode_frame_;

		packet->data = const_cast<uchar*>(start);
		maxlength = min(g_options.max_buf_sz_needed, maxlength);
		packet->size = maxlength;
		int got_frame = 0;

		int consumed = untr_decode(self->av_codec_context_, frame, &got_frame, packet);

		self->was_keyframe_ = frame->pict_type == AV_PICTURE_TYPE_I;
		self->was_bad_ = !got_frame;

		return consumed;
	}},
	GET_SZ_FN("mebx") {
		return swap32(*(int *)start);
	}},
	GET_SZ_FN("icod") {
		return swap32(*(int *)(start+2));
	}},
	GET_SZ_FN("ap4x") {
		return swap32(*(int *)start);
	}},
	GET_SZ_FN("camm") {
		// https://developers.google.com/streetview/publish/camm-spec
		// https://github.com/ponchio/untrunc/blob/2f4de8aa/codec_camm.cpp#L12
		int lengths[] = { 12, 8, 12, 12, 12, 24, 14*4, 12 };
		int type = start[2];
		if (type < 0 || type >= (int)(sizeof(lengths) / sizeof(lengths[0]))) return -1;
		return lengths[type] + 4;
	}},
	GET_SZ_FN("jpeg") {
		int n = 0, last_load = 0;
		auto p = start;
		while (1) {
			// Reload when half the buffer is consumed, to ensure enough lookahead.
			if (to_uint(n) - last_load > g_options.max_buf_sz_needed / 2) {
				p = self->loadAfter(n);
				last_load = n;
			}

			uchar t = p[1];  // https://www.disktuna.com/list-of-jpeg-markers/
			if (p[0] == 0xff && !(t <= 0x01) && !(0xd0 <= t && t <= 0xd8)) {
				if (t == 0xd9) return n + 2;
				int16_t len_raw;
			memcpy(&len_raw, p + 2, sizeof(len_raw));
			int len = swap16(len_raw);
				n += len - 2;
				p += len - 2;
			}
			else {
				n++;
				p++;
			}
		}
	}},

	/* if codec is not found in map,
	 * untrunc will try to generate common features (chunk_size, sample_size, patterns) per track.
	 * this is why these are commented out
	 *
	GET_SZ_FN("twos") { //lenght is multiple of 32, we split packets.
		return 4;
	}},
	GET_SZ_FN("in24") {
		return -1;
	}},
	GET_SZ_FN("lpcm") {
		// Use hard-coded values for now....
		const int num_samples      = 4096; // Empirical
		const int num_channels     =    2; // Stereo
		const int bytes_per_sample =    2; // 16-bit
		return num_samples * num_channels * bytes_per_sample;
	}},
	*/
};
// clang-format on

int Codec::getSize(const uchar *start, uint maxlength, off_t offset) {
	cur_off_ = offset;
	return get_size_fn_ ? get_size_fn_(this, start, maxlength) : -1;
}

bool Codec::needsDynStatsForSizing() {
	if (name_ != "mp4a" || !av_codec_context_) return false;
	// Probe whether the internal decode path reports consumed bytes.
	// FFmpeg 8+ changed AAC decoder internals: cb.decode now always returns
	// consumed=0 regardless of input (the decoder switched to a buffered model).
	// When consumed is always 0, frame-size determination via decode is impossible
	// and dynamic chunk stats are needed to use reference sample sizes instead.
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frm = av_frame_alloc();
	static uint8_t probe[64] = {}; // all-zero dummy payload
	pkt->data = probe;
	pkt->size = sizeof(probe);
	int got = 0;
	int consumed = ffcodec(av_codec_context_->codec)->cb.decode(av_codec_context_, frm, &got, pkt);
	avcodec_flush_buffers(av_codec_context_);
	av_frame_unref(frm);
	av_frame_free(&frm);
	av_packet_free(&pkt);
	// consumed == 0 with no frame means the decoder silently buffered the input
	// without producing output or an error: consumed-byte reporting is broken.
	// Note: for cb_type==2 codecs (FF_CODEC_CB_TYPE_RECEIVE_FRAME), cb.decode is
	// actually receive_frame. Calling it without a prior send_packet returns
	// AVERROR(EAGAIN) = -11, so consumed != 0 and this function returns false.
	// That is correct: those codecs do not use this probe path.
	return consumed == 0 && !got;
}

const uchar *Codec::loadAfter(off_t length) {
	return mp4_->loadFragment(cur_off_ + length, false);
}
