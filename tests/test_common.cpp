#include "test_harness.h"
#include "util/common.h"

#include <cmath>

void test_common() {
	std::cout << "test_common:\n";

	// swap32: hand-written bit rotation is critical for reading all atom sizes
	// from big-endian MP4 files. A single wrong shift breaks all atom parsing.
	{
		struct {
			uint32_t in;
			uint32_t expected;
		} cases[] = {
		    {0x12345678, 0x78563412}, {0x00000001, 0x01000000}, {0xFF000000, 0x000000FF},
		    {0x00000000, 0x00000000}, {0xFFFFFFFF, 0xFFFFFFFF},
		};

		for (auto &c : cases) {
			CHECK_EQ(swap32(c.in), c.expected);
			// Roundtrip: applying twice must yield the original value.
			CHECK_EQ(swap32(swap32(c.in)), c.in);
		}
	}

	// swap64: same correctness requirement for 64-bit atom sizes (co64/extended).
	{
		struct {
			uint64_t in;
			uint64_t expected;
		} cases[] = {
		    {0x0102030405060708ULL, 0x0807060504030201ULL},
		    {0x0000000000000001ULL, 0x0100000000000000ULL},
		    {0x0000000000000000ULL, 0x0000000000000000ULL},
		};

		for (auto &c : cases) {
			CHECK_EQ(swap64(c.in), c.expected);
			CHECK_EQ(swap64(swap64(c.in)), c.in);
		}
	}

	// gcd: used for chunk-alignment (pkt_sz_gcd_) and end-offset alignment.
	// Incorrect results silently produce wrong frame boundaries.
	{
		struct {
			int64_t a, b, expected;
		} cases[] = {
		    {12, 8, 4},    {8, 12, 4}, // commutative
		    {7, 7, 7},     {0, 5, 5},  // gcd(0,n) = n
		    {5, 0, 5},                 // gcd(n,0) = n
		    {17, 13, 1},               // coprime
		    {100, 75, 25}, {1024, 256, 256},
		};

		for (auto &c : cases)
			CHECK_EQ(gcd(c.a, c.b), c.expected);
	}

	// getMovExtension: determines the output file extension.
	// Edge cases matter because the function uses find_last_of and checks
	// whether the dot found is actually in a directory component.
	{
		struct {
			const char *path;
			const char *expected;
		} cases[] = {
		    {"file.mp4", ".mp4"},      {"file.mov", ".mov"}, {"/path/to/file.mp4", ".mp4"},
		    {"file.tar.gz", ".gz"},    // last dot wins
		    {"file", ".mp4"},          // no extension: fallback
		    {"/path.to/file", ".mp4"}, // dot in directory, not filename
		};

		for (auto &c : cases)
			CHECK_EQ(getMovExtension(c.path), std::string(c.expected));
	}

	// calcEntropy: Shannon entropy with base-2 logarithm.
	// For a uniform distribution over N distinct values the result is exactly log2(N).
	// These exact values are used as a gate in doesMatchApprox (threshold 0.75 bits).
	const double eps = 1e-9;
	{
		// Single repeated byte: entropy = 0
		std::vector<uchar> all_same = {0x42, 0x42, 0x42, 0x42};
		CHECK(std::fabs(calcEntropy(all_same) - 0.0) < eps);
	}
	{
		// Two bytes, equally distributed: entropy = log2(2) = 1.0
		std::vector<uchar> two_vals = {0x00, 0xFF, 0x00, 0xFF};
		CHECK(std::fabs(calcEntropy(two_vals) - 1.0) < eps);
	}
	{
		// Four distinct bytes, each appearing once: entropy = log2(4) = 2.0
		std::vector<uchar> four_vals = {0x01, 0x02, 0x04, 0x08};
		CHECK(std::fabs(calcEntropy(four_vals) - 2.0) < eps);
	}
}
