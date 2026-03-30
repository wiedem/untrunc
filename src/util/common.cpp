#include "common.h"
#include "io/file.h"

#include <iostream>
#include <iomanip> // setprecision
#include <sstream>
#include <cmath>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern "C" {
#include "libavcodec/avcodec.h"
}
#include "libavutil/ffversion.h"

#ifndef UNTR_VERSION
#define UNTR_VERSION "?"
#endif

const bool has_sawb_bug = false;

using namespace std;

Options g_options;
uint g_num_w2 = 0;

std::string g_version_str = "untrunc " UNTR_VERSION " (FFmpeg " FFMPEG_VERSION ")";

uint16_t swap16(uint16_t us) {
	return (us >> 8) | (us << 8);
}

uint32_t swap32(uint32_t ui) {
	return (ui >> 24) | ((ui << 8) & 0x00FF0000) | ((ui >> 8) & 0x0000FF00) | (ui << 24);
}

uint64_t swap64(uint64_t ull) {
	return (ull >> 56) | ((ull << 40) & 0x00FF000000000000) | ((ull << 24) & 0x0000FF0000000000) |
	       ((ull << 8) & 0x000000FF00000000) | ((ull >> 8) & 0x00000000FF000000) | ((ull >> 24) & 0x0000000000FF0000) |
	       ((ull >> 40) & 0x000000000000FF00) | (ull << 56);
}

void printBuffer(const uchar *pos, int n) {
	cout << mkHexStr(pos, n, 4) << '\n';
}

string mkHexStr(const uchar *pos, int n, int seperate_each) {
	stringstream out;
	out << hex;
	for (int i = 0; i != n; ++i) {
		if (seperate_each && i % seperate_each == 0) out << (seperate_each ? " " : "");

		int x = (int)*(pos + i);
		if (x < 16) out << '0';
		out << x;
	}
	return out.str();
}

void hitEnterToContinue(bool new_line) {
	if (g_options.interactive) {
		cout << "  [[Hit enter to continue]]" << (new_line ? "\n" : "") << flush;
		getchar();
	}
	//	else cout << '\n';
}

void outProgress(double now, double all, const string &prefix) {
	double x = round(1000 * (now / all));
	if (g_options.on_progress)
		g_options.on_progress(x / 10);
	else
		cout << prefix << x / 10 << "%  \r" << flush;
}

void mute() {
	g_options.muted = true;
	av_log_set_level(AV_LOG_QUIET);
}

void unmute() {
	g_options.muted = false;
	if (g_options.log_mode <= E)
		av_log_set_level(AV_LOG_QUIET);
	else if (g_options.log_mode < V)
		av_log_set_level(AV_LOG_WARNING);
	else if (g_options.log_mode > V)
		av_log_set_level(AV_LOG_DEBUG);
	g_logger->disableNoiseSuppression();
}

string pretty_bytes(double num) {
	uint idx = 0;
	vector<string> units = {"", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi"};
	while (idx + 1 < units.size()) {
		if (num < 1024) break;
		num /= 1024;
		idx++;
	}
	stringstream s;
	s << setprecision(3) << num << units[idx] << "B";
	return s.str();
}

void chkHiddenWarnings() {
	if (g_num_w2 && g_options.log_mode >= W) {
		cout << string(10, ' ') << '\n';
		cout << g_num_w2 << " warnings were hidden!\n";
	}
	g_num_w2 = 0;
}

void trim_right(string &in) {
	while (in.size() && (isspace(in.back()) || !in.back()))
		in.pop_back();
}

bool contains(const std::initializer_list<string> &c, const std::string &v) {
	return std::find(c.begin(), c.end(), v) != c.end();
}

string getMovExtension(const string &path) {
	auto idx = path.find_last_of(".");
	if (idx == string::npos) return ".mp4";
	auto ext = path.substr(idx);
	if (ext.find("/") != string::npos || ext.find("\\") != string::npos) return ".mp4";
	return ext;
}

// Shannon entropy
double calcEntropy(const vector<uchar> &in) {
	map<char, int> cnt;
	for (char c : in)
		cnt[c]++;

	double entropy = 0;
	for (auto p : cnt) {
		double freq = (double)p.second / in.size();
		entropy -= freq * log2(freq);
	}
	return entropy;
}

int64_t gcd(int64_t a, int64_t b) {
	return b ? gcd(b, a % b) : a;
}

mt19937 &getRandomGenerator() {
	static std::mt19937 gen = []() {
		const char *seed_env = std::getenv("UNTRUNC_SEED");
		if (seed_env) {
			auto seed = std::stoul(seed_env);
			logg(I, "using fixed random seed: ", seed, "\n");
			return std::mt19937(seed);
		}
		std::random_device rd;
		return std::mt19937(rd());
	}();
	return gen;
}

void warnIfAlreadyExists(const string &output) {
	if (FileRead::alreadyExists(output)) {
		logg(W, "destination '", output, "' already exists\n");
		hitEnterToContinue();
	}
}

bool isAllZeros(const uchar *buf, int n) {
	//	for (int i=0; i < n; i+=4) if (*(int*)(buf+i)) return false;
	for (int i = 0; i < n; i++)
		if (*(buf + i)) return false;
	return true;
}

int parseByteStr(string &s) {
	if (s.back() == 'b') s.pop_back();

	char c = s.back();
	int f = 1;
	if (isdigit(c))
		f = 1;
	else if (c == 'k')
		f = 1 << 10;
	else if (c == 'm')
		f = 1 << 20;
	else {
		logg(ET, "Error: unkown suffix: ", c, '\n');
	}

	if (f > 1) s.pop_back();
	return f * stoi(s);
}

void parseMaxPartsize(string &s) {
	g_options.max_partsize_default = 0; // disable default
	if (s == "f") return;               // just force dynamic max_partsize, no default
	g_options.max_partsize = parseByteStr(s);
}

#ifdef _WIN32
#include <windows.h>
#include <codecvt>

string to_utf8(const wchar_t *utf16) {
	wstring_convert<codecvt_utf8_utf16<wchar_t>, wchar_t> convert;
	return convert.to_bytes(utf16);
}

wstring to_utf16(const char *utf8) {
	wstring_convert<codecvt_utf8_utf16<wchar_t>> converter;
	return converter.from_bytes(utf8);
}

void argv_as_utf8(int argc, char *argv[]) {
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 0; i < argc; i++) {
		argv[i] = strdup(to_utf8(wargv[i]).c_str());
	}
	LocalFree(wargv);
}

FILE *_my_open(const char *path, const wchar_t *mode) {
	wstring pathW = to_utf16(path);
	return _wfopen(pathW.c_str(), mode);
}
#endif

void showStacktrace() {
#ifdef _WIN32
	// Do nothing on Windows
#else
	// Check if gdb is available
	if (system("which gdb > /dev/null 2>&1") == 0) {
		pid_t pid = getpid();

#ifdef __linux__
		// Allow any process to ptrace us (needed for gdb on modern Linux)
		prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

		string cmd = "gdb -batch -q -ex 'thread apply all bt' -p " + to_string(pid);
		cerr << "\n+ " << cmd << endl;
		cmd += " 2>&1 | grep -v -E \"warning: could not find '.*' file for /lib/|warning: "
		       ".*\\.\\./sysdeps/unix/sysv/linux/.*: No such file or directory\" | cat -s";
		system(cmd.c_str());
		cerr << "\n";
	} else {
		cerr << "gdb is not available on this system." << endl;
	}
#endif
}

// Function to trim leading and trailing whitespace from a string
string trim(const string &str) {
	size_t first = str.find_first_not_of(' ');
	if (first == string::npos) return "";
	size_t last = str.find_last_not_of(' ');
	return str.substr(first, last - first + 1);
}

// split the string by comma and trim whitespace from each part
vector<string> splitAndTrim(const string &str) {
	vector<string> result;
	stringstream ss(str);
	string item;

	while (getline(ss, item, ',')) {
		result.push_back(trim(item));
	}

	return result;
}
