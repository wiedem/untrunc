#include "rsv.h"
#include "mp4.h"
#include "util/common.h"

#include <cstring>

using namespace std;

bool isRtmdHeader(const uchar *buff) {
	// Real rtmd packets have:
	//   - Bytes 0-3: 001c0100 (constant prefix)
	//   - Bytes 4-7: variable (camera metadata like timestamp)
	//   - Bytes 8-11: f0010010 (constant Sony tag)
	// False positives (random video data) match bytes 0-3 but NOT bytes 8-11

	// Check constant prefix (bytes 0-3): 00 1c 01 00
	if (buff[0] != 0x00 || buff[1] != 0x1c || buff[2] != 0x01 || buff[3] != 0x00) return false;

	// Check Sony tag (bytes 8-11): f0 01 00 10
	if (buff[8] != 0xf0 || buff[9] != 0x01 || buff[10] != 0x00 || buff[11] != 0x10) return false;

	return true;
}

bool isPointingAtRtmdHeader(FileRead &file) {
	if (file.atEnd()) return false;
	return isRtmdHeader(file.getPtr(12));
}

// AUD (Access Unit Delimiter) patterns with length prefix
// H.264: NAL type 9 (0x09), typically 2-byte payload: 00 00 00 02 09 XX
// HEVC:  NAL type 35 (0x23), in 2-byte header: 00 00 00 03 46 01 XX
//        HEVC NAL type = (first_byte >> 1) & 0x3F = (0x46 >> 1) & 0x3F = 35
static const uchar kAudPatternH264[] = {0x00, 0x00, 0x00, 0x02, 0x09};
static const uchar kAudPatternHevc[] = {0x00, 0x00, 0x00, 0x03, 0x46};
static const int kAudPatternLen = 5;

RsvRepairer::RsvRepairer(Mp4 &mp4) : mp4_(mp4) {}

void RsvRepairer::detectStructure(FileRead &file_read, off_t file_size, bool is_hevc, int &rtmd_packet_size,
                                  int &frames_per_gop) {
	vector<uchar> detect_buf(128 * 1024 * 1024); // 128MB buffer for detection
	file_read.seek(0);
	size_t detect_read = min((off_t)detect_buf.size(), file_size);
	file_read.readChar((char *)detect_buf.data(), detect_read);

	if (detect_read < 13) {
		logg(W, "RSV file too small for structure detection\n");
		return;
	}

	// Detect rtmd_packet_size by finding distance between first two rtmd patterns
	off_t first_rtmd = -1, second_rtmd = -1;
	size_t rtmd_scan_end = min(detect_read, (size_t)100000);
	for (size_t i = 0; i + 12 <= rtmd_scan_end; i++) {
		if (isRtmdHeader(detect_buf.data() + i)) {
			if (first_rtmd < 0) {
				first_rtmd = i;
			} else {
				second_rtmd = i;
				break;
			}
		}
	}

	if (first_rtmd >= 0 && second_rtmd > first_rtmd) {
		int detected_size = second_rtmd - first_rtmd;
		if (detected_size > 1000 && detected_size < 100000) {
			rtmd_packet_size = detected_size;
			logg(I, "rtmd packet size auto-detected from RSV: ", rtmd_packet_size, " bytes\n");
		}
	} else {
		logg(W, "could not detect rtmd packet size, using default: ", rtmd_packet_size, "\n");
	}

	// Detect frames_per_gop by counting frames in first GOP
	int first_rtmd_count = 0;
	off_t detect_pos = 0;
	while (detect_pos + 12 < (off_t)detect_read && first_rtmd_count < 100) {
		if (isRtmdHeader(detect_buf.data() + detect_pos)) {
			first_rtmd_count++;
			detect_pos += rtmd_packet_size;
		} else {
			break;
		}
	}

	if (first_rtmd_count > 0) {
		off_t video_start = first_rtmd_count * rtmd_packet_size;

		off_t next_rtmd = 0;
		for (size_t i = video_start + 500 * 1024; i + 12 <= detect_read; i++) {
			if (isRtmdHeader(detect_buf.data() + i)) {
				next_rtmd = i;
				break;
			}
		}

		if (next_rtmd > 0) {
			off_t video_end_approx = next_rtmd - 100000;
			int aud_count = 0;
			const uchar *aud_p = is_hevc ? kAudPatternHevc : kAudPatternH264;

			for (size_t i = video_start; video_end_approx > 0 && i + kAudPatternLen <= (size_t)video_end_approx; i++) {
				if (memcmp(detect_buf.data() + i, aud_p, kAudPatternLen) == 0) {
					aud_count++;
				}
			}

			if (aud_count >= 6 && aud_count <= 60) {
				frames_per_gop = aud_count;
				logg(I, "frames per GOP auto-detected from RSV: ", frames_per_gop, "\n");
			}
		}
	}
}

void RsvRepairer::processGops(FileRead &file_read, off_t file_size, bool is_hevc, int video_track_idx,
                              const vector<int> &audio_track_indices, int rtmd_track_idx, int rtmd_packet_size,
                              int frames_per_gop, int audio_sample_size, int total_audio_chunk_size) {
	int audio_track_idx = audio_track_indices.empty() ? -1 : audio_track_indices[0];
	int num_audio_tracks = audio_track_indices.size();
	int audio_chunk_size_per_track = num_audio_tracks > 0 ? total_audio_chunk_size / num_audio_tracks : 0;

	const uchar *aud_pattern = is_hevc ? kAudPatternHevc : kAudPatternH264;

	off_t pos = 0;
	int gop_count = 0;
	int total_video_frames = 0;
	int total_audio_chunks = 0;
	int total_rtmd_packets = 0;

	const size_t buf_size = 128 * 1024 * 1024;
	vector<uchar> buffer(buf_size);

	while (pos < file_size) {
		// Count rtmd packets
		int rtmd_count = 0;
		uchar rtmd_header[12];
		while (pos + rtmd_count * rtmd_packet_size < file_size && rtmd_count < 100) {
			file_read.seek(pos + rtmd_count * rtmd_packet_size);
			file_read.readChar((char *)rtmd_header, 12);
			if (isRtmdHeader(rtmd_header)) {
				rtmd_count++;
			} else {
				break;
			}
		}

		if (rtmd_count == 0) {
			logg(V, "no rtmd packets found at ", pos, ", ending\n");
			break;
		}

		off_t rtmd_end = pos + rtmd_count * rtmd_packet_size;
		off_t video_start = rtmd_end;

		logg(V, "GOP ", gop_count, ": rtmd at ", pos, " (", rtmd_count, " packets), video starts at ", video_start,
		     "\n");

		// Read buffer from video_start
		file_read.seek(video_start);
		size_t to_read = min((off_t)buf_size, file_size - video_start);
		file_read.readChar((char *)buffer.data(), to_read);

		// Find video frames via AUD patterns
		vector<off_t> frame_offsets;
		vector<uint> frame_sizes;

		const uchar *buf_ptr = buffer.data();
		const uchar *buf_end = to_read >= (size_t)kAudPatternLen ? buf_ptr + to_read - kAudPatternLen : buf_ptr;
		const uchar *p = buf_ptr;

		while (p < buf_end) {
			p = (const uchar *)memchr(p, 0x00, buf_end - p);
			if (!p) break;

			if (p + kAudPatternLen <= buf_end && memcmp(p, aud_pattern, kAudPatternLen) == 0) {
				frame_offsets.push_back(video_start + (p - buf_ptr));
				p += kAudPatternLen;
			} else {
				p++;
			}
		}

		// Find next rtmd block
		off_t next_rtmd_start = file_size;
		size_t min_search_offset = min((size_t)(500 * 1024), to_read / 4);

		p = buf_ptr + min_search_offset;
		buf_end = to_read >= 12 ? buf_ptr + to_read - 12 : buf_ptr;

		while (p < buf_end) {
			p = (const uchar *)memchr(p, 0x00, buf_end - p);
			if (!p) break;

			if (isRtmdHeader(p)) {
				next_rtmd_start = video_start + (p - buf_ptr);
				break;
			}
			p++;
		}

		// Audio region is before next rtmd
		off_t audio_boundary = next_rtmd_start - total_audio_chunk_size;

		// Filter out frames past audio boundary
		while (!frame_offsets.empty() && frame_offsets.back() >= audio_boundary) {
			frame_offsets.pop_back();
		}

		// Calculate frame sizes
		for (size_t i = 0; i < frame_offsets.size(); i++) {
			if (i + 1 < frame_offsets.size()) {
				frame_sizes.push_back(frame_offsets[i + 1] - frame_offsets[i]);
			} else {
				frame_sizes.push_back(audio_boundary - frame_offsets[i]);
			}
		}

		off_t video_end = video_start;
		if (!frame_offsets.empty() && !frame_sizes.empty()) {
			video_end = frame_offsets.back() + frame_sizes.back();
		}

		logg(V, "GOP ", gop_count, ": found ", frame_offsets.size(), " frames, video ends at ", video_end, "\n");

		off_t audio_start = audio_boundary;

		if (frame_offsets.empty()) {
			logg(V, "no more video frames found, ending\n");
			break;
		}

		// Process video frames
		int frames_in_gop = frame_offsets.size();
		for (int f = 0; f < frames_in_gop; f++) {
			off_t frame_off = frame_offsets[f];
			uint frame_sz = frame_sizes[f];
			bool is_keyframe = (f == 0);

			Track &video_track = mp4_.tracks_[video_track_idx];
			video_track.num_samples_++;
			if (is_keyframe) {
				video_track.keyframes_.push_back(video_track.sizes_.size());
			}
			video_track.sizes_.push_back(frame_sz);

			Track::Chunk chunk;
			chunk.off_ = frame_off;
			chunk.size_ = frame_sz;
			chunk.n_samples_ = 1;
			video_track.chunks_.push_back(chunk);

			total_video_frames++;
			mp4_.ctx_.scan_.pkt_idx_++;
		}

		logg(V, "GOP ", gop_count, ": ", frames_in_gop, " video frames, audio at ", audio_start, "\n");

		// Process audio chunks
		if (audio_track_idx >= 0 && audio_start + total_audio_chunk_size <= file_size) {
			int samples_in_chunk = audio_chunk_size_per_track / audio_sample_size;

			for (int t = 0; t < num_audio_tracks; t++) {
				Track &audio_track = mp4_.tracks_[audio_track_indices[t]];
				audio_track.num_samples_ += samples_in_chunk;

				Track::Chunk audio_chunk;
				audio_chunk.off_ = audio_start + t * audio_chunk_size_per_track;
				audio_chunk.size_ = audio_chunk_size_per_track;
				audio_chunk.n_samples_ = samples_in_chunk;
				audio_track.chunks_.push_back(audio_chunk);

				mp4_.ctx_.scan_.pkt_idx_ += samples_in_chunk;
			}

			total_audio_chunks++;
			pos = audio_start + total_audio_chunk_size;
		} else {
			pos = next_rtmd_start;
		}

		// Add rtmd packets
		if (rtmd_track_idx >= 0 && rtmd_count > 0) {
			Track &rtmd_track = mp4_.tracks_[rtmd_track_idx];
			off_t rtmd_block_start = video_start - rtmd_count * rtmd_packet_size;

			Track::Chunk rtmd_chunk;
			rtmd_chunk.off_ = rtmd_block_start;
			rtmd_chunk.size_ = rtmd_count * rtmd_packet_size;
			rtmd_chunk.n_samples_ = rtmd_count;
			rtmd_track.chunks_.push_back(rtmd_chunk);
			rtmd_track.num_samples_ += rtmd_count;

			total_rtmd_packets += rtmd_count;
			mp4_.ctx_.scan_.pkt_idx_ += rtmd_count;
		}

		gop_count++;
		if (gop_count % 10 == 0) {
			outProgress(pos, file_size);
		}
	}

	logg(I, "RSV recovery complete:\n");
	logg(I, "  GOPs processed: ", gop_count, "\n");
	logg(I, "  Video frames: ", total_video_frames, "\n");
	logg(I, "  Audio chunks: ", total_audio_chunks, "\n");
	logg(I, "  RTMD packets: ", total_rtmd_packets, "\n");
}

void RsvRepairer::repair(const string &filename) {
	logg(I, "using RSV Ben recovery mode\n");

	auto &file_read = mp4_.openFile(filename);

	// Set up ctx_.file_.mdat_ as if it were an mdat starting at offset 0
	mp4_.ctx_.file_.mdat_ = std::make_unique<BufferedAtom>(file_read);
	mp4_.ctx_.file_.mdat_->name_ = "mdat";
	mp4_.ctx_.file_.mdat_->start_ = -8; // Pretend mdat header is at -8 so content starts at 0
	mp4_.ctx_.file_.mdat_->file_end_ = file_read.length();

	int rtmd_packet_size = 19456; // default, auto-detected below
	int frames_per_gop = 12;      // default, auto-detected below

	// Find video and audio track indices BEFORE clearing tracks
	int video_track_idx = mp4_.getTrackIdx2("avc1");
	bool is_hevc = false;
	if (video_track_idx < 0) {
		video_track_idx = mp4_.getTrackIdx2("hvc1");
		is_hevc = true;
	}

	vector<int> audio_track_indices;
	string audio_codec_name;
	for (uint i = 0; i < mp4_.tracks_.size(); i++) {
		const string &codec = mp4_.tracks_[i].codec_.name_;
		if (codec == "twos" || codec == "sowt" || codec == "ipcm") {
			audio_track_indices.push_back(i);
			if (audio_codec_name.empty()) audio_codec_name = codec;
		}
	}
	int audio_track_idx = audio_track_indices.empty() ? -1 : audio_track_indices[0];
	int num_audio_tracks = audio_track_indices.size();

	int rtmd_track_idx = mp4_.getTrackIdx2("rtmd");

	if (video_track_idx < 0) {
		logg(ET, "no video track (avc1/hvc1) found in reference file\n");
		return;
	}
	logg(I, "video track: ", video_track_idx, " (", (is_hevc ? "HEVC" : "H.264"), "), audio tracks: ", num_audio_tracks,
	     ", rtmd track: ", rtmd_track_idx, "\n");

	// Get audio and video parameters from reference file BEFORE clearing
	int audio_sample_size = 4;
	int audio_sample_rate = 48000;
	int video_timescale = 25000;
	int video_duration_per_sample = 1000;

	if (video_track_idx >= 0) {
		Track &video_track = mp4_.tracks_[video_track_idx];
		video_timescale = video_track.timescale_;
		if (!video_track.times_.empty()) {
			video_duration_per_sample = video_track.times_[0];
		}
		logg(V, "video timescale: ", video_timescale, ", duration per sample: ", video_duration_per_sample, "\n");
	}

	if (audio_track_idx >= 0) {
		Track &audio_track = mp4_.tracks_[audio_track_idx];
		if (audio_track.constant_size_ > 0) {
			audio_sample_size = audio_track.constant_size_;
		}
		audio_sample_rate = audio_track.timescale_;
		logg(V, "audio sample size: ", audio_sample_size, ", sample rate: ", audio_sample_rate,
		     ", tracks: ", num_audio_tracks, "\n");
	}

	off_t file_size = file_read.length();
	logg(I, "RSV file size: ", file_size, " bytes\n");

	if (file_size > (1LL << 32)) {
		mp4_.ctx_.file_.broken_is_64_ = true;
		logg(I, "using 64-bit offsets for the RSV file\n");
	}

	// Auto-detect RSV structure parameters
	detectStructure(file_read, file_size, is_hevc, rtmd_packet_size, frames_per_gop);

	// Calculate audio chunk size based on GOP duration
	double fps = (double)video_timescale / video_duration_per_sample;
	double gop_duration_sec = (double)frames_per_gop / fps;
	int audio_samples_per_chunk = (int)(gop_duration_sec * audio_sample_rate);
	int audio_chunk_size_per_track = audio_samples_per_chunk * audio_sample_size;
	int total_audio_chunk_size = audio_chunk_size_per_track * max(1, num_audio_tracks);

	logg(I, "derived parameters: fps=", fps, ", GOP duration=", gop_duration_sec,
	     "s, audio chunk=", total_audio_chunk_size, " bytes (", num_audio_tracks, " tracks)\n");

	// Clear tracks for RSV parsing
	mp4_.duration_ = 0;
	for (uint i = 0; i < mp4_.tracks_.size(); i++)
		mp4_.tracks_[i].clear();

	// Process file GOP by GOP
	processGops(file_read, file_size, is_hevc, video_track_idx, audio_track_indices, rtmd_track_idx, rtmd_packet_size,
	            frames_per_gop, audio_sample_size, total_audio_chunk_size);

	// Fix track timing
	for (auto &track : mp4_.tracks_)
		track.fixTimes();

	// Save the recovered video
	auto filename_fixed = mp4_.getPathRepaired(mp4_.filename_ok_, filename);
	mp4_.saveVideo(filename_fixed);
}
