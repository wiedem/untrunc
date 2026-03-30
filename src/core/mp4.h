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

#include "repair_report.h"
#include "analyze_report.h"
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

#include "free_seq.h"

class Mp4 : public HasHeaderAtom {
  public:
	Mp4() = default;
	~Mp4();

	// --- Public API ---

	// Parsing
	void parseOk(const std::string &filename, bool accept_unhealthy = false);

	// Repair & analysis
	void repair(const std::string &filename, RepairReport &report);
	void analyze(bool gen_off_map = false, AnalyzeReport *report = nullptr);
	void analyzeOffset(const std::string &filename, off_t offset);
	void dumpSamples();

	// Output
	void printTracks();
	void printTrackStats();
	void printAtoms();
	void printStats();
	void printMediaInfo();
	void makeStreamable(const std::string &ok, const std::string &output);
	void saveVideo(const std::string &filename);
	std::string getPathRepaired(const std::string &ok, const std::string &corrupt);
	bool alreadyRepaired(const std::string &ok, const std::string &corrupt);

	// Track lookup
	bool hasCodec(const std::string &codec_name);
	uint getTrackIdx(const std::string &codec_name);
	int getTrackIdx2(const std::string &codec_name) const;
	std::string getCodecName(uint track_idx);
	Track &getTrack(const std::string &codec_name);

	// Offset helpers
	off_t toAbsOff(off_t offset);
	std::string offToStr(off_t offset);

	off_t mdatEnd() const { return ctx_.file_.mdat_->start_ + ctx_.file_.mdat_->length_; }

	int64_t mdatContentSize() const { return ctx_.file_.mdat_->contentSize(); }

	// --- Data ---

	static uint64_t step_;
	std::vector<Track> tracks_;
	static constexpr int pat_size_ = kPatternSize;
	int idx_free_ = kNoFreeTrack;
	std::vector<FreeSeq> free_seqs_;
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

	// --- Semi-public (used by Track, Codec, Mp4Tools) ---

	const uchar *loadFragment(off_t offset, bool update_cur_maxlen = true);
	const uchar *getBuffAround(off_t offset, int64_t n);
	void addUnknownSequence(off_t start, uint64_t length);
	int findSizeWithContinuation(off_t off, std::vector<int> sizes);
	int twos_track_idx_ = -1;
	std::map<std::pair<int, int>, std::vector<off_t>> chunk_transitions_;
	static BufferedAtom *mdatFromRange(FileRead &file_read, BufferedAtom &mdat);
	static bool findAtom(FileRead &file_read, std::string atom_name, Atom &atom);

  private:
	// --- Parsing ---

	std::unique_ptr<Atom> root_atom_;
	std::unique_ptr<AVFormatContext, AVFormatContextDeleter> context_;
	std::string filename_ok_;
	const std::vector<std::string> ignore_duration_ = {"tmcd", "fdsc"};

	FileRead &openFile(const std::string &filename);
	BufferedAtom *findMdat(FileRead &file_read);
	void parseHealthy();
	void parseTracksOk();
	void chkStretchFactor();
	void setDuration();
	void checkForBadTracks();
	void afterTrackRealloc();

	// --- Matching ---

	// Dynamic pattern matching: determines the next track index via transition patterns.
	// Moved from Track::useDynPatterns to avoid the Track->Mp4 back-pointer.
	int useDynPatternsForTrack(int track_idx, off_t offset);
	bool doesMatchTransitionForTrack(int track_idx, const uchar *buff, int target_idx);

	bool wouldMatch(const WouldMatchCfg &cfg);
	bool wouldMatchDyn(off_t offset, int last_idx);
	FrameInfo predictSize(const uchar *start, int track_idx, off_t offset);
	FrameInfo getMatch(off_t offset, bool force_strict = false);
	bool shouldBeStrict(off_t off, int track_idx);
	bool anyPatternMatchesHalf(off_t offset, uint track_idx_to_try);

	// --- Scan navigation ---

	int skipZeros(off_t &offset, const uchar *start);
	int skipAtomHeaders(off_t offset, const uchar *start);
	int skipAtoms(off_t offset, const uchar *start);
	bool advanceOffset(off_t &offset, bool just_simulate = false);
	void printOffset(off_t offset);
	int64_t calcStep(off_t offset);
	bool pointsToZeros(off_t offset);
	bool isAllZerosAt(off_t offset, int n);

	// --- Chunk prediction ---

	Mp4::Chunk fitChunk(off_t offset, uint track_idx, uint known_n_samples = 0);
	bool predictChunkViaOrder(off_t offset, Mp4::Chunk &c);
	bool chunkStartLooksInvalid(off_t offset, const Mp4::Chunk &c);
	Chunk getChunkPrediction(off_t offset, bool only_perfect_fit = false);
	int getLikelyNextTrackIdx(int *n_samples = nullptr);
	int getNextTrackViaDynPatterns(off_t offset);
	int skipNextZeroCave(off_t off, int max_sz, int n_zeros);
	int calcFallbackTrackIdx();

	// Chunk state queries
	bool nearEnd(off_t offset) { return offset > (mdatContentSize() - ctx_.order_.cycle_size_); }

	bool amInFreeSequence() { return ctx_.scan_.last_track_idx_ == idx_free_; }

	bool currentChunkFinished(int add_extra = 0);
	bool currentChunkIsDone();
	int getChunkPadding(off_t &offset);

	// Chunk ordering
	void correctChunkIdx(int track_idx);
	void correctChunkIdxSimple(int track_idx);
	void onFirstChunkFound(int track_idx);
	bool isExpectedTrackIdx(int i);
	bool setDuplicateInfo();
	bool isTrackOrderEnough();
	void genTrackOrder();

	// --- Statistics ---

	void genDynStats(bool force_patterns = false);
	bool needDynStats();

	// --- Scan-loop state mutation ---

	void addFrame(const FrameInfo &frame_info);
	void addChunk(const Mp4::Chunk &chunk);
	void setLastTrackIdx(int track_idx);
	void addToExclude(off_t start, uint64_t length, bool force = false);
	bool chkUnknownSequenceEnded(off_t offset);

	// --- Verification ---

	void chkUntrunc(FrameInfo &fi, Codec &c, int i, AnalyzeReport *report = nullptr);
	std::map<off_t, FrameInfo> off_to_frame_;
	std::map<off_t, Mp4::Chunk> off_to_chunk_;
	void chkDetectionAtImpl(FrameInfo *detectedFramePtr, Mp4::Chunk *detectedChunkPtr, off_t off);
	void chkFrameDetectionAt(FrameInfo &detected, off_t off);
	void chkChunkDetectionAt(Mp4::Chunk &detected, off_t off);
	void chkExpectedOff(off_t *expected_off, off_t real_off, uint sz, int idx, std::ostream &out);
	void dumpMatch(const FrameInfo &fi, int idx, off_t *expected_off = nullptr, std::ostream &out = std::cout);
	void dumpChunk(const Mp4::Chunk &chunk, int &idx, off_t *expected_off = nullptr, std::ostream &out = std::cout);
	void dumpIdxAndOff(off_t off, int idx, std::ostream &out = std::cout);
	std::vector<FrameInfo> to_dump_;

	// --- Output helpers ---

	std::string getOutputSuffix();

	// --- Constants ---

	static const int kNoFreeTrack = -2;
	static const int kMinAvc1FrameLen = 6;
	static const int kProgressInterval = 2000;
	static const int kMatchLookaheadBuf = 1024;

	// --- Friends & context ---

	friend class Mp4Repairer;
	friend class RsvRepairer;
	RepairContext ctx_;
};

bool operator==(const Mp4::Chunk &lhs, const Mp4::Chunk &rhs);
bool operator!=(const Mp4::Chunk &lhs, const Mp4::Chunk &rhs);
std::ostream &operator<<(std::ostream &out, const Mp4::Chunk &c);
