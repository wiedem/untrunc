// Output methods for Mp4: printing info, saving video.

#include <iostream>
#include <iomanip>
#include <fstream>

extern "C" {
#include <stdint.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "mp4.h"

using namespace std;

void Mp4::printMediaInfo() {
	if (has_moov_) {
		printTracks();
		cout << "\n\n";
		printAtoms();
		cout << "\n\n";
		g_options.use_chunk_stats = true;
		printStats();
	} else {
		printAtoms();
	}
}

void Mp4::printTracks() {
	cout << "tracks:\n";
	for (uint i = 0; i < tracks_.size(); i++) {
		auto &track = tracks_[i];
		cout << "  [" << i << "] " << track.handler_type_ << " by '" << track.handler_name_ << "' ";
		if (track.codec_.name_.size()) cout << "(" << track.codec_.name_ << ") ";
		auto codec_type = av_get_media_type_string(track.codec_.av_codec_params_->codec_type);
		auto codec_name = avcodec_get_name(track.codec_.av_codec_params_->codec_id);
		cout << ss("<", codec_type, ", ", codec_name, ">\n");
	}
}

void Mp4::printAtoms() {
	if (!root_atom_) return;
	for (auto &c : root_atom_->children_)
		c->print(0);
}

void Mp4::printTrackStats() {
	for (auto &t : tracks_) {
		t.printStats();
		if (g_options.use_chunk_stats) {
			t.printDynPatterns(
			    true,
			    [this](int a, int b) -> size_t {
				    auto it = chunk_transitions_.find({a, b});
				    return it != chunk_transitions_.end() ? it->second.size() : 0;
			    },
			    [this](uint i) -> std::string { return getCodecName(i); });
		}
	}
}

void Mp4::printStats() {
	if (g_options.use_chunk_stats && ctx_.scan_.first_off_abs_ < 0) genDynStats(true);
	cout << "\nStats:\n";
	cout << "first_off_: " << ctx_.scan_.first_off_abs_ << '\n';
	cout << "ctx_.scan_.first_off_rel_: " << ctx_.scan_.first_off_rel_ << '\n';
	cout << "ctx_.file_.max_part_size_: " << ctx_.file_.max_part_size_ << '\n';
	cout << "\n";
	printTrackStats();
}

void Mp4::makeStreamable(const string &ok, const string &output) {
	warnIfAlreadyExists(output);
	parseOk(ok);
	if (!ctx_.file_.mdat_) findMdat(*ctx_.file_.file_);

	auto moov = root_atom_->atomByName("moov");
	auto mdat = root_atom_->atomByName("mdat");
	if (moov->start_ < mdat->start_) {
		logg(I, "already streamable!\n");
		return;
	}

	for (auto &t : tracks_) {
		ctx_.scan_.pkt_idx_ += t.sizes_.size();
		for (auto &c : t.chunks_)
			c.off_ -= mdat->contentStart();
	}

	saveVideo(output);
}

void Mp4::saveVideo(const string &filename) {
	/* we save all atom except:
	  cslg: would need to be recalculated (from ctts)
	  stps: partial sync, same as sync

	  movie is made by ftyp, moov, mdat (we need to know mdat begin, for absolute offsets)
	  assumes offsets in stco are absolute and so to find the relative just subtrack mdat->start + 8
*/

	if (tracks_.back().is_dummy_) tracks_.pop_back();

	if (g_options.log_mode >= I) {
		cout << "Info: Found " << ctx_.scan_.pkt_idx_ << " packets ( ";
		for (const Track &t : tracks_) {
			cout << t.codec_.name_ << ": " << t.getNumSamples() << ' ';
			if (contains({"avc1", "hvc1", "hev1"}, t.codec_.name_) || t.keyframes_.size())
				cout << ss(t.codec_.name_, "-keyframes: ", t.keyframes_.size(), " ");
		}
		cout << ")\n";
	}

	chkStretchFactor();
	setDuration();

	for (Track &track : tracks_) {
		track.applyExcludedToOffs();
		if (track.pkt_sz_gcd_ > 1 && g_options.dont_exclude) {
			track.splitChunks();
		}
		track.writeToAtoms(ctx_.file_.broken_is_64_);

		auto &cn = track.codec_.name_;
		if (contains(ignore_duration_, cn)) continue;

		int hour, min, sec, msec;
		int bmsec = track.getDurationInMs();

		auto x = div(bmsec, 1000);
		msec = x.rem;
		sec = x.quot;
		x = div(sec, 60);
		sec = x.rem;
		min = x.quot;
		x = div(min, 60);
		min = x.rem;
		hour = x.quot;
		string s_msec = (msec ? to_string(msec) + "ms " : "");
		string s_sec = (sec ? to_string(sec) + "s " : "");
		string s_min = (min ? to_string(min) + "min " : "");
		string s_hour = (hour ? to_string(hour) + "h " : "");
		logg(I, "Duration of ", cn, ": ", s_hour, s_min, s_sec, s_msec, " (", bmsec, " ms)\n");
	}

	BufferedAtom *mdat = ctx_.file_.mdat_.release(); // transfer ownership to root_atom_ via replace()
	Atom *original_mdat = root_atom_->atomByName("mdat");
	root_atom_->replace(original_mdat, mdat); // replace() takes ownership of mdat, deletes original
	//	Atom *mdat = root_atom_->atomByName("mdat");

	if (ctx_.scan_.unknown_lengths_.size()) {
		cout << setprecision(4);
		int64_t bytes_not_matched = 0;
		for (auto n : ctx_.scan_.unknown_lengths_)
			bytes_not_matched += n;
		double percentage = (double)100 * bytes_not_matched / mdat->contentSize();
		logg(W, "Unknown sequences: ", ctx_.scan_.unknown_lengths_.size(), '\n');
		logg(W, "Bytes NOT matched: ", pretty_bytes(bytes_not_matched), " (", percentage, "%)\n");
	}

	if (ctx_.scan_.atoms_skipped_.size()) {
		uint64_t sum = 0;
		for (auto x : ctx_.scan_.atoms_skipped_)
			sum += x;

		auto lvl = sum > 1 * (1 << 20) ? W : V;
		logg(lvl, "Skipped atoms in mdat: ", ctx_.scan_.atoms_skipped_.size(), " -> ", pretty_bytes(sum), " total\n");
	}

	if (g_options.dont_write) return;

	editHeaderAtom();

	Atom *ftyp = root_atom_->atomByName("ftyp");
	Atom *moov = root_atom_->atomByName("moov");

	//	moov->prune("ctts");
	moov->prune("cslg");
	moov->prune("stps");

	root_atom_->updateLength();

	// remove empty tracks
	for (auto it = tracks_.begin(); it != tracks_.end();) {
		if (!it->getNumSamples()) {
			moov->prune(it->trak_);
			logg(I, "pruned empty '", it->codec_.name_, "' track\n");
			it = tracks_.erase(it);
		} else
			it++;
	}

	//fix offsets
	off_t offset = mdat->newHeaderSize() + moov->length_;
	if (ftyp) offset += ftyp->length_; //not all mov have a ftyp.

	for (Track &track : tracks_) {
		assertt(track.chunks_.size(), track.codec_.name_, track.getNumSamples());
		for (auto &c : track.chunks_)
			c.off_ += offset;
		track.saveChunkOffsets(); //need to save the offsets back to the atoms
	}

	if (g_options.dump_repaired) {
		auto dst = filename + ".dump";
		cout << '\n';
		cout << "n_to_dump: " << to_dump_.size() << (to_dump_.size() ? "\n" : " (dumping all)\n");
		logg(I, "dumping to: '", dst, "'\n");
		ofstream f_out(dst);
		cout.rdbuf(f_out.rdbuf());

		filename_ok_ = filename;
		mdat->start_ = offset - 8; // not mdat->headerSize()
		ctx_.file_.mdat_.reset(mdat);
		dumpSamples();
		return;
	}

	//save to file
	logg(I, "saving ", filename, '\n');
	FileWrite file(filename);

	if (ftyp) ftyp->write(file);
	moov->write(file);
	mdat->write(file);
}

string Mp4::getOutputSuffix() {
	string output_suffix;
	if (g_options.ignore_unknown) output_suffix += ss("-s", Mp4::step_);
	if (g_options.use_chunk_stats) output_suffix += "-dyn";
	if (g_options.dont_exclude) output_suffix += "-k";
	if (g_options.stretch_video) output_suffix += "-sv";
	if (g_options.rsv_ben_mode) output_suffix += "-rsvBen";
	return output_suffix;
}

string rewriteDestination(const std::string &dst) {
	if (!g_options.dst_path.size()) return dst;
	if (isdir(g_options.dst_path))
		return g_options.dst_path + "/" + myBasename(dst);
	else
		return g_options.dst_path;
}

string Mp4::getPathRepaired(const std::string &ok, const std::string &corrupt) {
	auto filename_fixed = corrupt + "_fixed" + getOutputSuffix() + getMovExtension(ok);
	return rewriteDestination(filename_fixed);
}

bool Mp4::alreadyRepaired(const std::string &ok, const std::string &corrupt) {
	if (!g_options.skip_existing) return false;
	auto dst = getPathRepaired(ok, corrupt);
	if (FileRead::alreadyExists(dst)) {
		if (g_options.log_mode > E) cout << "exists: " << dst << '\n';
		return true;
	}
	return false;
}
