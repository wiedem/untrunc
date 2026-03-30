// Parsing methods for Mp4.
// Responsible for reading the healthy reference file and populating tracks_.

#include <iostream>

extern "C" {
#include <stdint.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "mp4.h"
#include "rsv.h"

using namespace std;

uint64_t Mp4::step_ = 1;

Mp4::~Mp4() = default;

void AVFormatContextDeleter::operator()(AVFormatContext *ctx) const {
	avformat_close_input(&ctx);
}

void Mp4::parseHealthy() {
	header_atom_ = root_atom_->atomByNameSafe("mvhd");
	readHeaderAtom();

	unmute(); // sets AV_LOG_LEVEL
	AVFormatContext *raw_ctx = avformat_alloc_context();
	// Open video file: on failure avformat_open_input frees raw_ctx and sets it to null
	int error = avformat_open_input(&raw_ctx, filename_ok_.c_str(), nullptr, nullptr);

	if (error != 0) throw std::runtime_error("Could not parse AV file (" + to_string(error) + "): " + filename_ok_);
	context_.reset(raw_ctx);

	if (avformat_find_stream_info(context_.get(), nullptr) < 0) throw std::runtime_error("Could not find stream info");

	av_dump_format(context_.get(), 0, filename_ok_.c_str(), 0);

	parseTracksOk();

	//	if (g_options.show_tracks) return;  // show original track order

	// reduce false positives when matching
	map<string, int> certainty = {
	    {"gpmd", 4},
	    {"fdsc", 3},
	    {"mp4a", 2},
	    {"avc1", 1},
	    // default is 0
	};
	sort(tracks_.begin(), tracks_.end(),
	     [&](const Track &a, const Track &b) -> bool { return certainty[a.codec_.name_] > certainty[b.codec_.name_]; });

	afterTrackRealloc();

	if (hasCodec("fdsc") && hasCodec("avc1")) getTrack("avc1").codec_.strictness_lvl_ = 1;

	if (hasCodec("tmcd")) {
		auto &t = getTrack("tmcd");
		if (t.sizes_.size() == 1 && t.sizes_[0] == 4) {
			t.is_tmcd_hardcoded_ = true;
		}
	}

	for (uint i = 0; i < tracks_.size(); i++)
		if (contains({"twos", "sowt"}, tracks_[i].codec_.name_)) twos_track_idx_ = i;
	if (twos_track_idx_ >= 0 && hasCodec("avc1")) {
		getTrack("avc1").codec_.chk_for_twos_ = true;
	}

	if (g_options.log_mode >= LogMode::I) cout << '\n';
}
void Mp4::parseOk(const string &filename, bool accept_unhealthy) {
	filename_ok_ = filename;
	auto &file = openFile(filename);

	logg(I, "parsing healthy moov atom ... \n");
	root_atom_ = std::make_unique<Atom>();
	while (true) {
		auto atom = std::make_unique<Atom>();
		try {
			atom->parse(file);
		} catch (const std::exception &e) {
			logg(W, "failed decoding atom: ", e.what(), "\n");
			break;
		}
		root_atom_->children_.push_back(std::move(atom));
		if (file.atEnd()) break;
	}

	if (root_atom_->atomByName("ctts"))
		cerr << "Composition time offset atom found. Out of order samples possible." << endl;

	if (root_atom_->atomByName("sdtp"))
		cerr << "Sample dependency flag atom found. I and P frames might need to recover that info." << endl;

	auto ftyp = root_atom_->atomByName("ftyp", true);
	if (ftyp) {
		ftyp_ = ftyp->getString(0, 4);
		logg(V, "ftyp_ = '", ftyp_, "'\n");

		auto minor_ver = ftyp->readInt(4);
		if (minor_ver == 1) {
			for (int i = 8; i < (int)ftyp->content_.size(); i += 4) {
				auto brand = ftyp->getString(i, 4);
				if (brand == "CAEP") { // e.g. 'Canon R5'
					logg(V, "detected 'CAEP', deactivating 'g_options.strict_nal_frame_check'\n");
					g_options.strict_nal_frame_check = false;
					g_options.ignore_keyframe_mismatch = true;
					g_options.skip_nal_filler_data = true;
				}
			}
		}

	} else {
		logg(V, "no 'ftyp' atom found\n");
	}

	if (ftyp_ == "XAVC") {
		logg(V, "detected 'XAVC', deactivating 'g_options.strict_nal_frame_check'\n");
		g_options.strict_nal_frame_check = false;
		g_options.ignore_forbidden_nal_bit = false;
		g_options.allow_large_sample = true;
	}

	has_moov_ = root_atom_->atomByName("moov", true);

	if (has_moov_)
		parseHealthy();
	else if (accept_unhealthy)
		logg(W, "no 'moov' atom found\n");
	else
		logg(ET, "no 'moov' atom found\n");
}
void Mp4::parseTracksOk() {
	Codec::initOnce();
	auto mdats = root_atom_->atomsByName("mdat", true);
	if (mdats.empty()) logg(ET, "no 'mdat' atom found\n");
	if (mdats.size() > 1) logg(W, "multiple mdats detected, see '-ia'\n");

	orig_mdat_start_ = mdats.front()->start_;

	auto traks = root_atom_->atomsByName("trak");
	uint nb_streams = (uint)context_.get()->nb_streams;
	if (traks.size() != nb_streams)
		logg(W, "trak count (", traks.size(), ") differs from FFmpeg stream count (", nb_streams, ")\n");
	for (uint i = 0; i < traks.size(); i++) {
		if (i >= nb_streams) {
			logg(W, "no FFmpeg stream for trak ", i, ", skipping\n");
			break;
		}
		tracks_.emplace_back(traks[i], context_.get()->streams[i]->codecpar, timescale_);
		auto &track = tracks_.back();
		track.parseOk();

		assertt(track.chunks_.size());
		if (!g_options.ignore_out_of_bound_chunks) {
			assertt(track.chunks_.front().off_ >= mdats.front()->contentStart());
			assertt(track.chunks_.back().off_ < mdats.back()->start_ + mdats.back()->length_);
		}

		ctx_.file_.max_part_size_ = max(ctx_.file_.max_part_size_, track.ss_stats_.maxAllowedPktSz());
	}

	if (g_options.max_partsize > 0) {
		logg(V, "ss: using manually specified: ", g_options.max_partsize, "\n");
		ctx_.file_.max_part_size_ = g_options.max_partsize;
	}
}
void Mp4::chkStretchFactor() {
	int video = 0, sound = 0;
	for (Track &track : tracks_) {
		int msec = track.getDurationInMs();
		if (track.handler_type_ == "vide")
			video = msec;
		else if (track.handler_type_ == "soun")
			sound = msec;
	}
	if (!sound) return;

	double factor = (double)sound / video;
	double eps = 0.1;
	if (fabs(factor - 1) > eps) {
		if (!g_options.stretch_video)
			cout << "Tip: Audio and video seem to have different durations (" << factor << ").\n"
			     << "     If audio and video are not in sync, give `-sv` a try. See `--help`\n";
		else
			for (Track &track : tracks_) {
				if (track.handler_type_ == "vide") {
					track.stretch_factor_ = (double)sound / video;
					//					track.stretch_factor_ = 0.5;
					logg(I, "changing video speed by factor ", 1 / factor, "\n");
					track.duration_ *= factor;
					break;
				}
			}
	}
}

void Mp4::setDuration() {
	for (Track &track : tracks_) {
		if (contains(ignore_duration_, track.codec_.name_)) continue;
		duration_ = max(duration_, track.getDurationInTimescale());
	}
}
FileRead &Mp4::openFile(const string &filename) {
	ctx_.file_.file_ = std::make_unique<FileRead>(filename);
	if (!ctx_.file_.file_->length()) throw length_error(ss("zero-length file: ", filename));
	return *ctx_.file_.file_;
}
void Mp4::checkForBadTracks() {
	if (ctx_.order_.track_order_.size()) return; // we already checked via `isTrackOrderEnough()`
	for (auto &t : tracks_) {
		if (!t.isSupported() && !t.hasPredictableChunks() && !(t.is_dummy_ && ctx_.dummy_.is_skippable_)) {
			logg(W, "Bad track: '", t.codec_.name_, "'\n",
			     "         Adding more sophisticated logic for this track could significantly improve the recovered "
			     "file's quality!\n");
			if (!(t.codec_.name_ == "tmcd" && t.sizes_.size() <= 1)) hitEnterToContinue();
		}
	}
}
bool Mp4::findAtom(FileRead &file_read, string atom_name, Atom &atom) {
	while (atom.name_ != atom_name) {
		off_t new_pos = Atom::findNextAtomOff(file_read, &atom, true);
		if (new_pos >= file_read.length() || new_pos < 0) {
			logg(W, "start of ", atom_name, " not found\n");
			atom.start_ = -8; // no header
			file_read.seek(0);
			return false;
		}
		file_read.seek(new_pos);
		atom.parseHeader(file_read);
	}
	return true;
}
BufferedAtom *Mp4::mdatFromRange(FileRead &file_read, BufferedAtom &mdat) {
	mdat.start_ = g_options.range_start - 8;
	mdat.name_ = "mdat";

	auto len = file_read.length();
	if (g_options.range_end > len)
		mdat.file_end_ = len;
	else if (g_options.range_end < 0)
		mdat.file_end_ = len + g_options.range_end;
	else
		mdat.file_end_ = g_options.range_end;

	logg(I, "range: ", g_options.range_start, ":", g_options.range_end, " -> ", mdat.start_, ":", mdat.file_end_, " (",
	     mdat.contentSize(), ")\n");
	if (mdat.contentSize() <= 0) logg(ET, "bad range, contentSize: ", mdat.contentSize(), "\n");
	return &mdat;
}

BufferedAtom *Mp4::findMdat(FileRead &file_read) {
	ctx_.file_.mdat_ = std::make_unique<BufferedAtom>(file_read);
	auto &mdat = *ctx_.file_.mdat_;

	if (file_read.filename_ == filename_ok_) {
		Atom *p = root_atom_->atomByName("mdat", true);
		if (p) mdat.Atom::operator=(*p);
	}

	else if (g_options.range_start != kRangeUnset)
		return mdatFromRange(file_read, mdat);

	if (!isPointingAtAtom(file_read)) {
		if (isPointingAtRtmdHeader(file_read)) {
			logg(I, "found rtmd-header at start of file, probably a .RSV file. Try '-rsv-ben' too\n");
			mdat.start_ = -8;
			mdat.name_ = "mdat";
		} else {
			logg(W, "no mp4-structure found in: '", file_read.filename_, "'\n");
			auto moov = root_atom_->atomByNameSafe("moov");
			//		if (ftyp_ == "XAVC") {
			if (orig_mdat_start_ < moov->start_) {
				logg(I, "using orig_mdat_start_ (=", orig_mdat_start_, ")\n");
				mdat.start_ = orig_mdat_start_;
				mdat.name_ = "mdat";
			} else if (!g_options.search_mdat) {
				logg(I, "assuming start_offset=0. \n",
				     "      use '-sm' to search for 'mdat' atom instead (via brute-force)\n");
				mdat.start_ = -8;
				mdat.name_ = "mdat";
			}
		}
	}

	findAtom(file_read, "mdat", mdat);
	mdat.file_end_ = file_read.length();

	return &mdat;
}
