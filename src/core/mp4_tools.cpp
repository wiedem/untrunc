// Standalone MP4 file manipulation utilities (no Mp4 instance state required).

#include <string>
#include <iostream>
#include <iomanip>
#include <algorithm>

#include "mp4_tools.h"
#include "mp4.h"
#include "atom/atom.h"
#include "io/file.h"

using namespace std;

namespace Mp4Tools {

void listm(const string &filename) {
	struct MoovStats {
		off_t atom_start_;
		off_t min_off_ = numeric_limits<off_t>::max(), max_off_ = -1;
		int n_tracks = 0;

		MoovStats(off_t start) : atom_start_(start) {}

		explicit operator bool() { return n_tracks > 0; }
	};

	FileRead f(filename);
	map<string, int> cnt;
	vector<std::unique_ptr<Atom>> moovs, mdats;

	auto printMoov = [](Atom &moov) {
		static int idx = 0;
		vector<string> vc;
		auto ms = MoovStats(moov.start_);

		for (Atom *trak : moov.atomsByName("trak")) {
			Track t(trak, nullptr, 0);
			vc.emplace_back(t.getCodecNameSlow());
			t.getChunkOffsets();
			ms.min_off_ = min(ms.min_off_, t.chunks_.front().off_);
			ms.max_off_ = max(ms.max_off_, t.chunks_.back().off_);
			ms.n_tracks++;
		}
		cout << ss(idx++, ": ", setw(12), ms.min_off_, " : ", setw(12), ms.max_off_, ", width=", setw(12),
		           ms.max_off_ - ms.min_off_, ", ", setw(28), left, vecToStr(vc), right, " (start=", setw(13),
		           ss(ms.atom_start_, ","), " moov_sz=", setw(12), moov.length_, ")\n");
	};

	auto printMdat = [](Atom &atom) {
		static int idx = 0;
		cout << ss(idx++, ": ", setw(85), " (start=", setw(13), ss(atom.start_, ","), " mdat_sz=", setw(12),
		           atom.length_, ")\n");
	};

	for (Atom &atom : AllAtomsIn(f)) {
		if (atom.name_ == "moov") {
			f.seek(atom.start_);
			moovs.push_back(std::make_unique<Atom>(f));
		} else if (atom.name_ == "mdat") {
			f.seek(atom.start_);
			mdats.push_back(std::make_unique<Atom>(f));
		}
	}

	cout << "moovs:\n";
	for (auto &atom : moovs)
		printMoov(*atom);
	cout << "mdats:\n";
	for (auto &atom : mdats)
		printMdat(*atom);
}

void unite(const string &mdat_fn, const string &moov_fn) {
	string output = mdat_fn + "_united.mp4";
	warnIfAlreadyExists(output);

	FileRead fmdat(mdat_fn), fmoov(moov_fn);

	BufferedAtom mdat(fmdat), moov(fmoov);
	if (g_options.range_start != kRangeUnset)
		Mp4::mdatFromRange(fmdat, mdat);
	else
		assertt(Mp4::findAtom(fmdat, "mdat", mdat));
	assertt(Mp4::findAtom(fmoov, "moov", moov));

	bool force_64 = mdat.header_length_ > 8;
	logg(V, "force_64: ", force_64, '\n');

	FileWrite fout(output);
	if (g_options.range_start != kRangeUnset) {
		Atom free;
		free.name_ = "free";
		free.start_ = -8;
		free.content_.resize(mdat.start_ - 8);
		free.updateLength();
		free.write(fout);
	} else {
		fout.copyRange(fmdat, 0, mdat.start_);
	}
	mdat.updateFileEnd(fmdat.length());
	moov.file_end_ = moov.start_ + moov.length_; // otherwise uninitialized
	mdat.write(fout, force_64);
	moov.write(fout);
}

void shorten(const string &filename, int mega_bytes, bool force) {
	int64_t n_bytes = (int64_t)mega_bytes * 1 << 20;
	string suf = force ? "_fshort-" : "_short-";
	string output = ss(filename + suf, mega_bytes, getMovExtension(filename));
	warnIfAlreadyExists(output);

	FileRead f(filename);
	BufferedAtom mdat(f), moov(f);
	if (f.length() <= n_bytes) logg(ET, "File size already is < ", pretty_bytes(n_bytes), "\n");

	bool good_structure = isPointingAtAtom(f);
	if (good_structure) assertt(Mp4::findAtom(f, "mdat", mdat));

	FileWrite fout(output);
	if (!good_structure || !Mp4::findAtom(f, "moov", moov) || moov.start_ < mdat.start_) {
		fout.copyN(f, 0, n_bytes);
	} else {
		auto mdat_end_real = mdat.start_ + mdat.length_;
		off_t n_after_mdat = f.length() - mdat_end_real;

		if (n_bytes < n_after_mdat) {
			if (!force)
				logg(ET, "moov might not be fully contained (", n_after_mdat, " < ", n_bytes, "), force with '-fsh'\n");
			fout.copyN(f, 0, n_bytes);
			return;
		}

		fout.copyRange(f, 0, mdat.start_);
		mdat.updateFileEnd(n_bytes - n_after_mdat);
		mdat.write(fout, mdat.header_length_ > 8);
		fout.copyN(f, mdat_end_real, n_after_mdat);
	}
}

} // namespace Mp4Tools
