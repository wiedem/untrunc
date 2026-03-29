#include "test_harness.h"
#include "core/mp4_repairer.h"

// collectLikelySizes: pure static function that filters sample sizes by
// relative frequency. It is the core heuristic for AAC frame size fallback.

void test_repairer() {
	std::cout << "test_repairer:\n";

	// Empty input produces an empty result.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({});
		CHECK(result.empty());
	}

	// Single repeated value: appears 100% of the time, always included.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({256, 256, 256, 256});
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Two values at 50% each: both exceed the 1% threshold.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({256, 512, 256, 512});
		CHECK_EQ((int)result.size(), 2);
		CHECK_EQ(result[0], 256); // result is sorted ascending
		CHECK_EQ(result[1], 512);
	}

	// Rare value: appears once out of 101 entries (~0.99%) -> excluded (below 1%).
	// The dominant value appears 100/101 times and must be included.
	{
		std::vector<int> sizes(100, 256);
		sizes.push_back(999); // 1/101 ≈ 0.0099 < 0.01
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Exactly at the threshold: 1/100 = 0.01 >= 0.01 -> included.
	{
		std::vector<int> sizes(99, 256);
		sizes.push_back(512); // 1/100 = 0.01
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 2);
		CHECK_EQ(result[0], 256);
		CHECK_EQ(result[1], 512);
	}

	// Custom threshold: with min_freq=0.5, only values appearing >= 50% are kept.
	{
		// 256: 3/5=60%, 512: 1/5=20%, 768: 1/5=20%
		std::vector<int> sizes = {256, 256, 256, 512, 768};
		auto result = Mp4Repairer::collectLikelySizes(sizes, 0.5);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Result is sorted ascending regardless of insertion order.
	{
		std::vector<int> sizes = {768, 256, 512, 768, 256, 512};
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 3);
		CHECK_EQ(result[0], 256);
		CHECK_EQ(result[1], 512);
		CHECK_EQ(result[2], 768);
	}
}
