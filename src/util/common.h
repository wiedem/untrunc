#pragma once
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <algorithm>
#include <random>

class Mp4;

using uint = uint32_t;
using uchar = unsigned char;

using buffs_t = std::vector<std::vector<uchar>>;
using offs_t = std::vector<off_t>;

template <typename T> inline unsigned int to_uint(T a) {
	return static_cast<unsigned int>(a);
}
template <typename T> inline size_t to_size_t(T a) {
	return static_cast<size_t>(a);
}
template <typename T> inline int64_t to_int64(T a) {
	return static_cast<int64_t>(a);
}
template <typename T> inline uint64_t to_uint64(T a) {
	return static_cast<uint64_t>(a);
}

#include "logger.h"

const int64_t kRangeUnset = std::numeric_limits<int64_t>::min();

struct Options {
	LogMode log_mode = LogMode::I;
	uint max_partsize_default = 1u << 23; // 8MiB
	uint max_partsize = 0;                // configurable via "-mp"
	uint max_buf_sz_needed = 1u << 19;    // 512kiB
	bool interactive = true;
	bool muted = false;
	bool ignore_unknown = false;
	bool stretch_video = false;
	bool show_tracks = false;
	bool dont_write = false;
	bool use_chunk_stats = false;
	bool dont_exclude = false;
	bool dump_repaired = false;
	bool search_mdat = false;
	bool strict_nal_frame_check = true;
	bool allow_large_sample = false;
	bool ignore_forbidden_nal_bit = false;
	bool dont_omit = false;
	bool ignore_out_of_bound_chunks = false;
	bool skip_existing = false;
	bool ignore_keyframe_mismatch = false;
	bool skip_nal_filler_data = false;
	bool rsv_ben_mode = false;
	bool off_as_hex = true;
	bool fast_assert = false;
	bool no_ctts = false;
	int64_t range_start = kRangeUnset;
	int64_t range_end = kRangeUnset;
	std::string dst_path;
	void (*on_progress)(int) = nullptr;
};

extern Options g_options;
extern const bool has_sawb_bug;
extern std::string g_version_str;
extern uint g_num_w2; // hidden warnings

template <class... Args> std::string ss(Args &&...args) {
	std::stringstream stream;
	(stream << ... << std::forward<Args>(args));
	return stream.str();
}

#define logg(lvl, ...)                                                                                                 \
	do {                                                                                                               \
		if (g_options.log_mode >= lvl) {                                                                               \
			logg_impl(lvl, __VA_ARGS__);                                                                               \
		} else if (lvl == W2) {                                                                                        \
			g_num_w2++;                                                                                                \
		}                                                                                                              \
	} while (0)

template <class... Args> void logg_impl(LogMode m, Args &&...x) {
	g_logger->log(m, ss(std::forward<Args>(x)...));
}

#define loggF(lvl, ...)                                                                                                \
	do {                                                                                                               \
		if (g_options.log_mode >= lvl) {                                                                               \
			loggF_impl(lvl, __VA_ARGS__);                                                                              \
		} else if (lvl == W2) {                                                                                        \
			g_num_w2++;                                                                                                \
		}                                                                                                              \
	} while (0)

template <class... Args> void loggF_impl(Args &&...x) {
	bool was_active = g_logger->isNoiseSuppressed();
	if (was_active) g_logger->disableNoiseSuppression();
	logg_impl(std::forward<Args>(x)...);
	if (was_active) g_logger->enableNoiseSuppression();
}

bool contains(const std::initializer_list<std::string> &c, const std::string &v);

template <typename Container> bool contains(Container const &c, typename Container::const_reference v) {
	return std::find(c.begin(), c.end(), v) != c.end();
}

std::mt19937 &getRandomGenerator();

// Returns a sorted sample of up to 100 elements from 'in'.
// Strategy: pick (k-1)=9 random windows of size k=10 plus the last k=10 elements.
// Returns 'in' unchanged when its size is <= 100.
template <template <typename, typename...> class C, class T> C<T> choose100(const C<T> &in) {
	size_t n = 100, k = 10;
	if (n > in.size()) return in;

	auto gen = getRandomGenerator();
	std::uniform_int_distribution<size_t> dis(0, std::distance(in.begin(), in.end()) - 1 - k);
	C<T> out;

	for (uint i = 0; i < k - 1; i++) {
		size_t idx = dis(gen);
		for (size_t j = idx; j < idx + k; j++)
			out.push_back(in[j]);
	}

	auto it = in.end() - k;
	for (uint i = 0; i < k; i++)
		out.push_back(*it++);

	std::sort(out.begin(), out.end());
	return out;
}

template <class T> std::string vecToStr(std::vector<T> v) {
	std::stringstream ss;
	ss << "[";
	for (size_t i = 0; i < v.size(); ++i) {
		if (i != 0) ss << ", ";
		ss << v[i];
	}
	ss << "]";
	return ss.str();
}

template <class Numeric> std::string hexIf(Numeric x) {
	if (g_options.off_as_hex) return ss(std::hex, "0x", x, std::dec);
	return ss(x);
}

const std::map<std::string, std::string> g_atom_names = {{"esds", "ES Descriptor"},
                                                         {"stsd", "sample description"},
                                                         {"minf", "media information"},
                                                         {"stss", "sync samples"},
                                                         {"udta", "user data"},
                                                         {"stsz", "sample to size"},
                                                         {"ctts", "sample to composition time"},
                                                         {"stsc", "sample to chunk"},
                                                         {"stts", "sample to decode time"},
                                                         {"co64", "chunk to offset 64"},
                                                         {"stco", "chunk to offset"},
                                                         {"mvhd", "movie header"},
                                                         {"mdhd", "media header"}};

void mute();
void unmute();

uint16_t swap16(uint16_t us);
uint32_t swap32(uint32_t ui);
uint64_t swap64(uint64_t ull);

void outProgress(double now, double all, const std::string &prefix = "");

void printBuffer(const uchar *pos, int n);
std::string mkHexStr(const uchar *pos, int n, int seperate_each = 0);

void hitEnterToContinue(bool new_line = true);

std::string pretty_bytes(double bytes);
void printVersion();

void chkHiddenWarnings();

void trim_right(std::string &in);

std::string getMovExtension(const std::string &path);
std::string getOutputSuffix();

double calcEntropy(const std::vector<uchar> &in);
int64_t gcd(int64_t a, int64_t b);

void warnIfAlreadyExists(const std::string &);
bool isAllZeros(const uchar *buf, int n);

int parseByteStr(std::string &s);
void parseMaxPartsize(std::string &s);

#ifdef _WIN32
void argv_as_utf8(int argc, char *argv[]);
FILE *_my_open(const char *path, const wchar_t *mode);
#define my_open(path, mode) _my_open(path, L##mode)
#else
#define argv_as_utf8(...) ;
#define my_open fopen
#endif

void showStacktrace();
std::vector<std::string> splitAndTrim(const std::string &str);

template <typename... Args>
void printArgs(std::ostream &os, const std::string &sep, const std::string &argNames, Args... args) {
	std::vector<std::string> names = splitAndTrim(argNames);
	if (names.size()) {
		os << sep;
		int i = 0;
		bool first = true; // Flag to check if it is the first element
		(
		    [&](const auto &arg) {
			    if (!first) {
				    os << ", ";
			    }
			    first = false;
			    os << names[i++] << "=" << arg;
		    }(args),
		    ...);
	}
	os << "\n";
}

#define dbgg(msg, ...)                                                                                                 \
	if (g_options.log_mode >= V) __dbgg(msg, #__VA_ARGS__, ##__VA_ARGS__)

template <typename... Args> void __dbgg(const std::string &message, const std::string &argNames, Args... args) {
	std::cout << message;
	printArgs(std::cout, " ", argNames, args...);
}

#ifndef NDEBUG
#define assertt(Expr, ...)                                                                                             \
	if (!(Expr)) __assertt(#Expr, __PRETTY_FUNCTION__, __FILE__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#else
#define assertt(...) ;
#endif

template <typename... Args>
void __assertt(const char *expr_str, const char *fn, const char *file, int line, const std::string &argNames,
               Args... args) {
	g_logger->disableNoiseSuppression();

	std::cerr << file << ":" << line << ": " << fn << ": Assertion `" << expr_str << "' failed.";

	printArgs(std::cerr, "  // ", argNames, args...);

	if (g_options.fast_assert) exit(1);

	showStacktrace();
	abort();
}
