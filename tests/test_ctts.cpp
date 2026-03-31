#include "test_harness.h"
#include "track/track.h"

#include <utility>
#include <vector>

using P = std::pair<int, int>;

static int totalSamples(const std::vector<P> &ctts) {
	int n = 0;
	for (auto &p : ctts)
		n += p.first;
	return n;
}

void test_expand_ctts() {
	std::cout << "test_expand_ctts:\n";

	// Empty pattern returns empty result
	{
		auto r = expandCtts({}, 100);
		CHECK(r.empty());
	}

	// Zero samples returns empty result
	{
		auto r = expandCtts({{1, 1024}, {1, 2560}}, 0);
		CHECK(r.empty());
	}

	// Short repeating pattern expanded to target sample count
	{
		std::vector<P> pattern = {{1, 1024}, {1, 2560}};
		auto r = expandCtts(pattern, 6);
		CHECK_EQ(totalSamples(r), 6);
		// Pattern should cycle: 1024, 2560, 1024, 2560, 1024, 2560
		CHECK_EQ(r[0], P(1, 1024));
		CHECK_EQ(r[1], P(1, 2560));
		CHECK_EQ(r[2], P(1, 1024));
		CHECK_EQ(r[3], P(1, 2560));
		CHECK_EQ(r[4], P(1, 1024));
		CHECK_EQ(r[5], P(1, 2560));
	}

	// Pattern with multi-sample entries, exact fit
	{
		std::vector<P> pattern = {{3, 512}, {2, 1024}};
		auto r = expandCtts(pattern, 10);
		CHECK_EQ(totalSamples(r), 10);
		// Two full cycles: (3,512)(2,1024)(3,512)(2,1024)
		CHECK_EQ((int)r.size(), 4);
		CHECK_EQ(r[0], P(3, 512));
		CHECK_EQ(r[1], P(2, 1024));
		CHECK_EQ(r[2], P(3, 512));
		CHECK_EQ(r[3], P(2, 1024));
	}

	// Truncation: target < pattern total, last entry trimmed
	{
		std::vector<P> pattern = {{3, 512}, {2, 1024}};
		auto r = expandCtts(pattern, 4);
		CHECK_EQ(totalSamples(r), 4);
		// First entry (3, 512) fills 3, need 1 more from (2, 1024)
		CHECK_EQ((int)r.size(), 2);
		CHECK_EQ(r[0], P(3, 512));
		CHECK_EQ(r[1], P(1, 1024));
	}

	// Truncation mid-first-entry
	{
		std::vector<P> pattern = {{10, 512}};
		auto r = expandCtts(pattern, 3);
		CHECK_EQ(totalSamples(r), 3);
		CHECK_EQ((int)r.size(), 1);
		CHECK_EQ(r[0], P(3, 512));
	}

	// Raw ctts data (one entry per sample) truncated to fewer samples
	{
		std::vector<P> raw = {{1, 100}, {1, 200}, {1, 300}, {1, 400}, {1, 500}};
		auto r = expandCtts(raw, 3);
		CHECK_EQ(totalSamples(r), 3);
		CHECK_EQ((int)r.size(), 3);
		CHECK_EQ(r[0], P(1, 100));
		CHECK_EQ(r[1], P(1, 200));
		CHECK_EQ(r[2], P(1, 300));
	}

	// Raw ctts data expanded (cycled) when target > total
	{
		std::vector<P> raw = {{1, 100}, {1, 200}};
		auto r = expandCtts(raw, 5);
		CHECK_EQ(totalSamples(r), 5);
		CHECK_EQ((int)r.size(), 5);
		CHECK_EQ(r[0], P(1, 100));
		CHECK_EQ(r[1], P(1, 200));
		CHECK_EQ(r[2], P(1, 100));
		CHECK_EQ(r[3], P(1, 200));
		CHECK_EQ(r[4], P(1, 100));
	}

	// Single sample
	{
		auto r = expandCtts({{5, 1024}}, 1);
		CHECK_EQ(totalSamples(r), 1);
		CHECK_EQ(r[0], P(1, 1024));
	}
}
