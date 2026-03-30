#include "test_harness.h"
#include "track/sample_stats.h"

void test_sample_stats() {
	std::cout << "test_sample_stats:\n";

	// Default-constructed: upper_bound=0 means no limit, wouldExceed always false
	{
		SampleSizeStats s;
		CHECK(!s.wouldExceed("test", 0, 1000000, false));
		CHECK(!s.wouldExceed("test", 0, 1000000, true));
	}

	// Default-constructed: lower_bound=0 means no limit, isBigEnough always false
	{
		SampleSizeStats s;
		CHECK(!s.isBigEnough(1000000, false));
		CHECK(!s.isBigEnough(1000000, true));
	}

	// onConstant: all three sub-stats set to sz
	{
		SampleSizeStats s;
		s.onConstant(256);
		CHECK_EQ(s.normal.upper_bound, 256u);
		CHECK_EQ(s.normal.lower_bound, 256u);
		CHECK_EQ(s.keyframe.upper_bound, 256u);
		CHECK_EQ(s.effective_keyframe.upper_bound, 256u);
	}

	// Exact boundary: sz does not exceed upper_bound, sz+1 does
	// exceedsAllowed uses strict greater-than: limit && sz > limit
	{
		SampleSizeStats s;
		s.onConstant(256);
		CHECK(!s.wouldExceed("test", 256, 0, false)); // 256+0=256, 256>256 is false
		CHECK(!s.wouldExceed("test", 255, 1, false)); // 255+1=256, 256>256 is false
		CHECK(s.wouldExceed("test", 256, 1, false));  // 256+1=257, 257>256 is true
	}

	// Exact boundary: isBigEnough at exactly lower_bound (uses >=)
	{
		SampleSizeStats s;
		s.onConstant(256);
		CHECK(s.isBigEnough(256, false));  // 256>=256 is true
		CHECK(!s.isBigEnough(255, false)); // 255>=256 is false
	}

	// After N varied samples + onFinished: bounds set and cover observed range
	// The statistical formulas guarantee upper_bound >= max and lower_bound <= min
	{
		SampleSizeStats s;
		for (int v : {200, 220, 240, 256, 270, 280, 300})
			s.updateStat(v, false);
		s.onFinished();
		CHECK(s.normal.upper_bound > 0u);
		CHECK(s.normal.lower_bound > 0u);
		CHECK(s.normal.upper_bound >= 300u); // must cover observed max
		CHECK(s.normal.lower_bound <= 200u); // must cover observed min
		CHECK(s.normal.upper_bound > s.normal.lower_bound);
	}

	// Keyframe effective limit is higher when keyframes are larger than normal frames
	{
		SampleSizeStats s;
		for (int v : {200, 220, 240, 256, 270, 280, 300})
			s.updateStat(v, false);
		for (int v : {1000, 1200, 1400, 1500, 1600})
			s.updateStat(v, true);
		s.onFinished();
		CHECK(s.getUpperLimit(true) > s.getUpperLimit(false));
	}
}
