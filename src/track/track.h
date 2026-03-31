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

#pragma once
#include <functional>
#include <iomanip> // setw
#include <vector>

#include "codec/codec.h"
#include "atom/header_atom.h"
#include "track/sample_stats.h"
#include "util/mutual_pattern.h"

class Track : public HasHeaderAtom {
  public:
	Track(Atom *trak, AVCodecParameters *c, int mp4_timescale);
	Track(const std::string &codec_name);

	Atom *trak_ = nullptr;
	Codec codec_;
	int mp4_timescale_;
	double stretch_factor_ = 1; // stretch video by via stts entries
	bool do_stretch_ = false;
	std::string handler_type_; // 'soun' OR 'vide'
	std::string handler_name_; // encoder used when created

	//mp4a can be decoded and repors the number of samples (duration in samplerate scale).
	//in some videos the duration (stts) can be variable and we can rebuild them using these values.
	std::vector<int> times_; // sample durations
	int constant_duration_ = -1;
	bool is_tmcd_hardcoded_ = false;

	std::vector<int> sizes_;
	int constant_size_ = 0;
	std::vector<int> keyframes_; //used for 'avc1', 0 based!
	size_t num_samples_ = 0;
	int64_t getNumSamples() const;
	int getSize(size_t idx);
	int getTime(size_t idx);

	int getOrigSize(uint idx);

	void parseOk();
	void writeToAtoms(bool broken_is_64);
	void clear();
	void fixTimes();

	int64_t getDurationInTimescale(); // in movie timescale, not track timescale

	void getSampleTimes();
	void getKeyframes();
	void getSampleSizes();
	void getChunkOffsets();
	void parseSampleToChunk();
	void getCompositionOffsets();
	void genPatternPerm(int twos_track_idx, std::function<std::string(uint)> get_codec_name = nullptr);

	void saveSampleTimes();
	void saveKeyframes();
	void saveSampleToChunk();
	void saveSampleSizes();
	void saveChunkOffsets();
	void saveCompositionOffsets();
	void mergeChunks();
	void splitChunks();

	bool isChunkTrack();

	std::string getCodecNameSlow();

	SampleSizeStats ss_stats_;

	int pkt_sz_gcd_ = 1;

	int alignPktLength(int length) {
		length += (pkt_sz_gcd_ - (length % pkt_sz_gcd_)) % pkt_sz_gcd_;
		return length;
	}

	int getSizeWithGcd(size_t idx) { return alignPktLength(getSize(idx)); }

	int pad_after_chunk_ = -1;
	int last_pad_after_chunk_ = -1;               // helper so we can skip last chunk (padding might be different there)
	std::vector<uint8_t> has_unclear_transition_; // track_idx -> has_unclear_transition

	void adjustPadAfterChunk(int new_pad) {
		if (last_pad_after_chunk_ != -1) {
			auto x = last_pad_after_chunk_;
			if (pad_after_chunk_ == -1) {
				pad_after_chunk_ = x;
			} else if (pad_after_chunk_ != x) {
				pad_after_chunk_ = 0;
			}
		}
		last_pad_after_chunk_ = new_pad;
	}

	struct Chunk {
		Chunk() = default;
		Chunk(off_t off, int64_t size, int ns);
		off_t off_ = 0; // absolute offset
		int64_t already_excluded_ = 0;
		int64_t size_ = 0; // only updated for 'free'
		int n_samples_ = 0;
	};

	// we try to predict next track_idx with these
	std::vector<std::vector<MutualPattern>> dyn_patterns_; // track_idx -> MutualPatterns
	int predictable_start_cnt_ = 0;
	int unpredictable_start_cnt_ = 0;

	std::vector<Chunk> chunks_;
	std::vector<int> likely_n_samples_; // per chunk
	std::vector<int> likely_sample_sizes_;
	double likely_n_samples_p = 0;
	double likely_samples_sizes_p = 0;
	bool hasPredictableChunks();
	bool shouldUseChunkPrediction();
	int64_t chunk_distance_gcd_; // to next chunk of same track

	// these offsets are absolute (to file begin)
	int64_t start_off_gcd_;
	int64_t end_off_gcd_; // sometimes 'free' sequences are used for padding to absolute n*32kb offsets

	bool isChunkOffsetOk(off_t off);
	int64_t stepToNextOwnChunk(off_t off);
	int64_t stepToNextOwnChunkAbs(off_t off);
	int64_t stepToNextOtherChunk(off_t off);
	bool is_dummy_ = false;
	bool dummyIsUsedAsPadding();

	Track::Chunk current_chunk_;
	bool chunkProbablyAtAnd();

	void printStats();
	// Prints dynamic patterns. When show_percentage is true, transition_count and get_codec_name
	// must be provided to display counts and names; otherwise they are optional.
	void printDynPatterns(bool show_percentage = false, std::function<size_t(int, int)> transition_count = nullptr,
	                      std::function<std::string(uint)> get_codec_name = nullptr);
	void genLikely();

	bool isSupported() { return codec_.isSupported() || is_tmcd_hardcoded_; }

	bool hasZeroTransitions();

	void genChunkSizes();

	// Appends current_chunk_ to chunks_ and resets it. For dummy tracks the caller
	// is responsible for calling Mp4::addUnknownSequence() first when n_samples_ > 0.
	void finalizeCurrentChunk();
	void applyExcludedToOffs();

	std::vector<int> orig_comp_offs_; // from ctts
	int dump_idx_ = 0;
	bool chunkReachedSampleLimit();
	int has_duplicates_ = false;

	off_t mdat_content_start_ = 0; // absolute file offset of mdat content start; set by findMdat()

	// set by genPatternPerm; accessed by Mp4::useDynPatternsForTrack
	std::vector<uint> dyn_patterns_perm_;
	int use_looks_like_twos_idx_ = -1;

  private:
	// from healthy file
	std::vector<int> orig_sizes_;
	std::vector<int> orig_times_;
	std::vector<std::pair<int, int>> orig_ctts_;

	uint ownTrackIdx();
	void calcAvgSampleSize();
};

std::ostream &operator<<(std::ostream &out, const Track::Chunk &fi);

std::vector<std::pair<int, int>> expandCtts(const std::vector<std::pair<int, int>> &pattern, size_t num_samples);
void updateElstDuration(Atom *trak, int64_t track_duration);

// Common base for chunks that carry a track index alongside the base chunk data.
// Both Mp4::Chunk and ChunkIt::Chunk inherit from this to avoid duplicating track_idx_.
struct IndexedChunk : Track::Chunk {
	IndexedChunk() = default;

	IndexedChunk(off_t off, int64_t size, int ns, int track_idx) : Track::Chunk(off, size, ns), track_idx_(track_idx) {}

	IndexedChunk(Track::Chunk base, int track_idx) : Track::Chunk(base), track_idx_(track_idx) {}

	explicit operator bool() const { return track_idx_ >= 0; }

	int track_idx_ = -1;
};
