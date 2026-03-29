/*
	Untrunc - mp4.h

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
#include <vector>
#include <string>
#include <stdio.h>
#include <memory>

#include "util/common.h"
#include "atom/header_atom.h"
#include "track/track.h"
#include "atom/atom.h"
#include "track/frame_info.h"
#include "track/track_list.h"
#include "mp4_scan.h"
#include "chunk_it.h"
#include "repair_context.h"
class FileRead;
class AVFormatContext;

struct AVFormatContextDeleter {
	void operator()(AVFormatContext *ctx) const;
};
struct TrackGcdInfo;

struct WouldMatchCfg {
	off_t offset;
	std::string skip;
	bool force_strict = false;
	int last_track_idx = -1;
	bool very_first = false;
};
typedef WouldMatchCfg WMCfg;

inline std::ostream &operator<<(std::ostream &out, const WouldMatchCfg &cfg) {
	return out << cfg.offset
	           << ", "
	              "skip=\""
	           << cfg.skip << "\", " << "force_strict=" << cfg.force_strict << ", " << "very_first=" << cfg.very_first;
}

struct FreeSeq {
	off_t offset; // relative
	int64_t sz;
	int prev_track_idx;
	int64_t last_chunk_sz;
	std::string codec_name; // for display

	bool operator<(const FreeSeq &other) const { return offset < other.offset; }
};

std::ostream &operator<<(std::ostream &out, const FreeSeq &x);

class Mp4 : public HasHeaderAtom {
  public:
	Mp4() = default;
	~Mp4();

	void parseOk(const std::string &filename, bool accept_unhealthy = false); // parse the first file

	void printTracks();
	void printTrackStats();
	void printAtoms();
	void printStats();
	void printMediaInfo();

	void makeStreamable(const std::string &ok, const std::string &output);
	void saveVideo(const std::string &filename);

	void dumpSamples();
	void analyze(bool gen_off_map = false);
	void repair(const std::string &filename);
	void repairRsvBen(const std::string &filename);

	bool wouldMatch(const WouldMatchCfg &cfg);
	bool wouldMatch2(const uchar *start);
	bool wouldMatchDyn(off_t offset, int last_idx);
	FrameInfo predictSize(const uchar *start, int track_idx, off_t offset);
	FrameInfo getMatch(off_t offset, bool force_strict = false);
	void analyzeOffset(const std::string &filename, off_t offset);

	bool hasCodec(const std::string &codec_name);
	uint getTrackIdx(const std::string &codec_name);
	int getTrackIdx2(const std::string &codec_name) const;
	std::string getCodecName(uint track_idx);
	Track &getTrack(const std::string &codec_name);
	off_t toAbsOff(off_t offset);
	std::string offToStr(off_t offset);
	// Returns absolute end offset of the current mdat (start + length).
	// Used by ChunkIt to determine the mdat boundary without direct field access.
	off_t mdatEnd() const { return ctx_.current_mdat_->start_ + ctx_.current_mdat_->length_; }
	std::string getPathRepaired(const std::string &ok, const std::string &corrupt);
	bool alreadyRepaired(const std::string &ok, const std::string &corrupt);

	static uint64_t step_; // step_size in unknown sequence
	std::vector<Track> tracks_;
	//	static const int pat_size_ = 64;
	static const int pat_size_ = 32;
	int idx_free_ = kNoFreeTrack; // idx of dummy track

	std::vector<FreeSeq> free_seqs_; // for testing if 'free' is skippable
	std::vector<FreeSeq> chooseFreeSeqs();
	bool canSkipFree();

	bool has_moov_ = false;

	std::string ftyp_;
	off_t orig_mdat_start_;
	off_t orig_mdat_end_;

	bool premature_end_ = false;
	double premature_percentage_ = 0;

	// Chunks with constant sample size
	class Chunk : public IndexedChunk {
	  public:
		Chunk() = default;
		Chunk(off_t off, int ns, int track_idx, int sample_size);
		int sample_size_ = 0;
	};

	bool setDuplicateInfo();
	bool isExpectedTrackIdx(int i);
	void onFirstChunkFound(int track_idx);
	void correctChunkIdxSimple(int track_idx);

	// Note: An backtrack algo across multiple (e.g. track_order.size()) matches would be better than this, since it would work even if currentChunkFinished + we wouldn't have to check upfront
	int findSizeWithContinuation(off_t off, std::vector<int> sizes);

	// Low-level access used by Track, Codec, and Mp4Tools.
	// These are not intended as part of the primary public API.
	const uchar *loadFragment(off_t offset, bool update_cur_maxlen = true);
	const uchar *getBuffAround(off_t offset, int64_t n);
	void addUnknownSequence(off_t start, uint64_t length);
	int twos_track_idx_ = -1;
	std::map<std::pair<int, int>, std::vector<off_t>> chunk_transitions_;
	static BufferedAtom *mdatFromRange(FileRead &file_read, BufferedAtom &mdat);
	static bool findAtom(FileRead &file_read, std::string atom_name, Atom &atom);

  private:
	std::unique_ptr<Atom> root_atom_;
	BufferedAtom *findMdat(FileRead &file_read);
	std::unique_ptr<AVFormatContext, AVFormatContextDeleter> context_;

	void parseHealthy();
	void parseTracksOk();
	void chkStretchFactor();
	void setDuration();
	void chkUntrunc(FrameInfo &fi, Codec &c, int i);
	void addFrame(const FrameInfo &frame_info);
	void addChunk(const Mp4::Chunk &chunk);

	int skipZeros(off_t &offset, const uchar *start);
	int skipAtomHeaders(off_t offset, const uchar *start);
	int skipAtoms(off_t offset, const uchar *start);
	bool advanceOffset(off_t &offset, bool just_simulate = false);

	void setLastTrackIdx(int track_idx);

	std::string filename_ok_;
	std::map<off_t, FrameInfo> off_to_frame_;
	std::map<off_t, Mp4::Chunk> off_to_chunk_;
	void chkDetectionAtImpl(FrameInfo *detectedFramePtr, Mp4::Chunk *detectedChunkPtr, off_t off);
	void chkFrameDetectionAt(FrameInfo &detected, off_t off);
	void chkChunkDetectionAt(Mp4::Chunk &detected, off_t off);
	void chkExpectedOff(off_t *expected_off, off_t real_off, uint sz, int idx);
	void dumpMatch(const FrameInfo &fi, int idx, off_t *predicted_off = nullptr);
	void dumpChunk(const Mp4::Chunk &chunk, int &idx, off_t *predicted_off = nullptr);
	void dumpIdxAndOff(off_t off, int idx);
	std::vector<FrameInfo> to_dump_;

	void genDynStats(bool force_patterns = false);
	void genChunks();
	void resetChunkTransitions();
	void genChunkTransitions();
	void collectPktGcdInfo(std::map<int, TrackGcdInfo> &track_to_info);
	void analyzeFree();
	void genDynPatterns();
	void genLikelyAll();

	buffs_t offsToBuffs(const offs_t &offs, const std::string &load_prefix);
	patterns_t offsToPatterns(const offs_t &offs, const std::string &load_prefix);

	bool calcTransitionIsUnclear(int track_idx_a, int track_idx_b);
	void setHasUnclearTransition();

	bool anyPatternMatchesHalf(off_t offset, uint track_idx_to_try);
	Mp4::Chunk fitChunk(off_t offset, uint track_idx, uint known_n_samples = 0);

	void addToExclude(off_t start, uint64_t length, bool force = false);

	bool chkUnknownSequenceEnded(off_t offset);
	int64_t calcStep(off_t offset);

	const std::vector<std::string> ignore_duration_ = {"tmcd", "fdsc"};

	FileRead &openFile(const std::string &filename);

	bool predictChunkViaOrder(off_t offset, Mp4::Chunk &c);
	bool chunkStartLooksInvalid(off_t offset, const Mp4::Chunk &c);
	Chunk getChunkPrediction(off_t offset, bool only_perfect_fit = false);

	bool nearEnd(off_t offset);
	bool amInFreeSequence();
	bool currentChunkFinished(int add_extra = 0);
	void afterTrackRealloc();
	int getNextTrackViaDynPatterns(off_t offset);

	void printOffset(off_t offset);

	bool pointsToZeros(off_t offset);
	bool isAllZerosAt(off_t offset, int n);

	bool needDynStats();
	bool shouldBeStrict(off_t off, int track_idx);
	void checkForBadTracks();

	std::string getOutputSuffix();

	bool currentChunkIsDone();
	int getChunkPadding(off_t &offset);

	int getLikelyNextTrackIdx(int *n_samples = nullptr);
	bool isTrackOrderEnough();
	void genTrackOrder();
	void setDummyIsSkippable();
	void correctChunkIdx(int track_idx);

	int skipNextZeroCave(off_t off, int max_sz, int n_zeros);

	int calcFallbackTrackIdx();

	static const int kNoFreeTrack = -2;

	friend class Mp4Repairer;
	RepairContext ctx_;
};

// ChunkIt and AllChunksIn are defined in chunk_it.h (included below after Mp4).

bool operator==(const Mp4::Chunk &lhs, const Mp4::Chunk &rhs);
bool operator!=(const Mp4::Chunk &lhs, const Mp4::Chunk &rhs);
std::ostream &operator<<(std::ostream &out, const Mp4::Chunk &c);
