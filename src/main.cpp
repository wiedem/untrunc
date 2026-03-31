/*
	Untrunc - main.cpp

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

#include <iostream>
#include <string>

#include "libavutil/ffversion.h"
#include "cxxopts.hpp"

#include "core/mp4.h"
#include "core/mp4_tools.h"
#include "core/repair_report.h"
#include "atom/atom.h"
#include "util/common.h"

using namespace std;

void parseRange(const string &s) {
	auto pos = s.find(":");
	if (pos == string::npos) logg(ET, "use python slice notation\n");
	auto s1 = s.substr(0, pos);
	auto s2 = s.substr(pos + 1);
	g_options.range_start = s1.size() ? stoll(s1) : 0;
	g_options.range_end = s2.size() ? stoll(s2) : numeric_limits<int64_t>::max();
}

int main(int argc, char *argv[]) {
	argv_as_utf8(argc, argv);

	cxxopts::Options options("untrunc", "Restore a truncated mp4/mov file");

	// clang-format off
	options.add_options("General")
		("V,version", "Print version")
		("n,no-interactive", "No interactive prompts")
		("h,help", "Show help")
	;

	options.add_options("Repair")
		("s,step", "Step through unknown sequences")
		("step-size", "Step size (used with --step)", cxxopts::value<int>())
		("stretch-video", "Stretch video to match audio duration (beta)")
		("rsv-ben", "RSV file recovery (Sony recording-in-progress files)")
		("dry-run", "Don't write _fixed.mp4")
		("dump-repaired", "Dump repaired tracks")
		("k,keep-unknown", "Keep unknown sequences")
		("search-mdat", "Search mdat even if no mp4 structure found")
		("ignore-oob-chunks", "Don't check if chunks are inside mdat")
		("dynamic-stats", "Use dynamic stats")
		("range", "Raw data range (A:B)", cxxopts::value<string>())
		("o,output", "Set destination dir or file", cxxopts::value<string>())
		("skip-existing", "Skip if output already exists")
		("no-ctts", "Don't restore composition time offsets")
		("no-edts", "Don't restore edit lists")
		("max-part", "Max part size in bytes", cxxopts::value<string>())
	;

	options.add_options("Analyze")
		("a,analyze", "Test codec detection accuracy (developer tool)")
		("i,info", "Show info (tracks, atoms, stats)", cxxopts::value<string>()->implicit_value(""), "TYPE")
		("d,dump", "Dump samples")
		("f,find-atoms", "Find all atoms and check their lengths")
		("list-mdat", "Find all mdat/moov atoms")
		("m,match", "Analyze file offset", cxxopts::value<int64_t>())
	;

	options.add_options("Tools")
		("make-streamable", "Move moov atom to front")
		("shorten", "Shorten file to N megabytes", cxxopts::value<int>()->implicit_value("500"))
		("force-shorten", "Force shorten even if moov might be lost")
		("u,unite", "Unite mdat and moov fragments")
	;

	options.add_options("Logging")
		("q,quiet", "Errors only")
		("w,warnings", "Show hidden warnings")
		("v,verbose", "Verbose (repeat for more: -v -v)")
		("show-all-warnings", "Don't omit potential noise")
		("decimal-offsets", "Show offsets as decimal")
		("exit-on-error", "Exit immediately on first error")
	;

	options.add_options("Positional")
		("ok-file", "", cxxopts::value<string>())
		("corrupt-file", "", cxxopts::value<string>()->default_value(""))
	;
	// clang-format on

	options.parse_positional({"ok-file", "corrupt-file"});
	options.positional_help("<ok.mp4> [corrupt.mp4]");

	cxxopts::ParseResult result;
	try {
		result = options.parse(argc, argv);
	} catch (const cxxopts::exceptions::exception &e) {
		cerr << "Error: " << e.what() << "\n";
		return 1;
	}

	if (result.count("help")) {
		cerr << options.help() << "\n";
		return 0;
	}

	if (result.count("version")) {
		cout << g_version_str << '\n';
		return 0;
	}

	// Logging
	if (result.count("quiet"))
		g_options.log_mode = LogMode::E;
	else if (result.count("warnings"))
		g_options.log_mode = LogMode::W2;
	else if (result.count("verbose")) {
		auto v = result.count("verbose");
		g_options.log_mode = v >= 2 ? LogMode::VV : LogMode::V;
	}

	// Repair options
	if (result.count("step")) g_options.ignore_unknown = true;
	if (result.count("keep-unknown")) g_options.dont_exclude = true;
	if (result.count("stretch-video")) g_options.stretch_video = true;
	if (result.count("rsv-ben")) g_options.rsv_ben_mode = true;
	if (result.count("dry-run")) g_options.dont_write = true;
	if (result.count("dump-repaired")) g_options.dump_repaired = true;
	if (result.count("search-mdat")) g_options.search_mdat = true;
	if (result.count("ignore-oob-chunks")) g_options.ignore_out_of_bound_chunks = true;
	if (result.count("dynamic-stats")) g_options.use_chunk_stats = true;
	if (result.count("skip-existing")) g_options.skip_existing = true;
	if (result.count("no-ctts")) g_options.no_ctts = true;
	if (result.count("no-edts")) g_options.no_edts = true;
	if (result.count("no-interactive")) g_options.interactive = false;

	if (result.count("output")) g_options.dst_path = result["output"].as<string>();
	if (result.count("range")) parseRange(result["range"].as<string>());
	if (result.count("max-part")) parseMaxPartsize(result["max-part"].as<string>());

	// Logging detail options
	if (result.count("show-all-warnings")) g_options.dont_omit = true;
	if (result.count("decimal-offsets")) g_options.off_as_hex = false;
	if (result.count("exit-on-error")) g_options.fast_assert = true;

	// Positional args
	if (!result.count("ok-file")) {
		cerr << options.help() << "\n";
		return 1;
	}

	string ok = result["ok-file"].as<string>();
	string corrupt = result["corrupt-file"].as<string>();

	// Mode flags
	bool find_atoms_mode = result.count("find-atoms") > 0;
	bool unite = result.count("unite") > 0;
	bool shorten = result.count("shorten") > 0;
	bool force_shorten = result.count("force-shorten") > 0;
	bool listm = result.count("list-mdat") > 0;
	bool make_streamable = result.count("make-streamable") > 0;
	bool analyze = result.count("analyze") > 0;
	bool dump_samples = result.count("dump") > 0;
	bool analyze_offset = result.count("match") > 0;

	// --info parsing
	bool show_info = false, show_tracks = false, show_atoms = false, show_stats = false;
	if (result.count("info")) {
		auto val = result["info"].as<string>();
		if (val == "tracks")
			show_tracks = true;
		else if (val == "atoms")
			show_atoms = true;
		else if (val == "stats")
			show_stats = true;
		else if (val.empty())
			show_info = true;
		else {
			cerr << "Error: unknown --info type '" << val << "' (use: tracks, atoms, stats)\n";
			return 1;
		}
	}
	g_options.show_tracks = show_tracks || show_info;

	// These modes produce data on stdout, suppress non-error logging
	if (dump_samples || analyze_offset) g_options.log_mode = LogMode::E;

	// Validation
	int step_size = result.count("step-size") ? result["step-size"].as<int>() : -1;
	if (!g_options.ignore_unknown && step_size > 0) logg(ET, "setting --step-size without using '--step'\n");

	if (g_options.rsv_ben_mode) {
		if (g_options.ignore_unknown) logg(ET, "'--rsv-ben' is not compatible with '--step'\n");
		if (g_options.dont_exclude) logg(ET, "'--rsv-ben' is not compatible with '--keep-unknown'\n");
		if (g_options.use_chunk_stats) logg(ET, "'--rsv-ben' is not compatible with '--dynamic-stats'\n");
	}

	bool skip_info = find_atoms_mode;
	if (!skip_info) {
		logg(V, g_version_str, '\n');
	}

	auto chkC = [&]() {
		if (corrupt.empty()) logg(ET, "no second file specified\n");
	};

	try {
		if (find_atoms_mode) {
			Atom::findAtomNames(ok);
			return 0;
		}
		if (unite) {
			chkC();
			Mp4Tools::unite(ok, corrupt);
			return 0;
		}
		if (shorten) {
			Mp4Tools::shorten(ok, result["shorten"].as<int>(), force_shorten);
			return 0;
		}
		if (listm) {
			Mp4Tools::listm(ok);
			return 0;
		}

		Mp4 mp4;
		if (step_size > 0) {
			logg(I, "using step_size=", step_size, "\n");
			Mp4::step_ = step_size;
		}

		auto ext = getMovExtension(ok);
		if (make_streamable) {
			mp4.makeStreamable(ok, ok + "_streamable" + ext);
			return 0;
		}
		if (mp4.alreadyRepaired(ok, corrupt)) return 0;

		logg(V, "reading ", ok, '\n');
		mp4.parseOk(ok, (show_atoms || show_info));

		if (show_tracks)
			mp4.printTracks();
		else if (show_atoms)
			mp4.printAtoms();
		else if (show_stats) {
			g_options.use_chunk_stats = true;
			mp4.printStats();
		} else if (show_info)
			mp4.printMediaInfo();
		else if (dump_samples)
			mp4.dumpSamples();
		else if (analyze) {
			AnalyzeReport report;
			mp4.analyze(false, &report);
			report.finish();
			chkHiddenWarnings();
			return report.exitCode();
		} else if (analyze_offset)
			mp4.analyzeOffset(corrupt.empty() ? ok : corrupt, static_cast<off_t>(result["match"].as<int64_t>()));
		else if (!corrupt.empty()) {
			RepairReport report;
			mp4.repair(corrupt, report);
			report.finish();
			chkHiddenWarnings();
			return report.exitCode();
		}
	} catch (const std::exception &e) {
		cerr << e.what() << '\n';
		return 1;
	}

	chkHiddenWarnings();
	return 0;
}
