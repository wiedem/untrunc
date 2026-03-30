#include "test_harness.h"
#include "core/analyze_report.h"

#include <iostream>
#include <string>

void test_analyze_report() {
	std::cout << "test_analyze_report:\n";

	// Default state is Success, exit 0
	{
		AnalyzeReport r;
		CHECK_EQ(r.status(), AnalyzeStatus::Success);
		CHECK_EQ(r.exitCode(), 0);
		CHECK_EQ(r.framesAnalyzed(), (int64_t)0);
		CHECK_EQ((int)r.mismatches().size(), 0);
	}

	// Frame counter increments
	{
		AnalyzeReport r;
		r.onFrameAnalyzed();
		r.onFrameAnalyzed();
		r.onFrameAnalyzed();
		CHECK_EQ(r.framesAnalyzed(), (int64_t)3);
		CHECK_EQ(r.status(), AnalyzeStatus::Success);
		CHECK_EQ(r.exitCode(), 0);
	}

	// Mismatch -> Mismatch status, exit 1
	{
		AnalyzeReport r;
		r.onFrameAnalyzed();
		r.onMismatch(0, 0, "0x100 / 0x200", "size: expected 1024, detected 768");
		CHECK_EQ(r.status(), AnalyzeStatus::Mismatch);
		CHECK_EQ(r.exitCode(), 1);
	}

	// Multiple mismatches accumulate in order
	{
		AnalyzeReport r;
		r.onMismatch(0, 10, "0xa / 0xb", "first");
		r.onMismatch(1, 20, "0xc / 0xd", "second");
		r.onMismatch(0, 30, "0xe / 0xf", "third");
		CHECK_EQ((int)r.mismatches().size(), 3);
		CHECK_EQ(r.mismatches()[0].track_idx, 0);
		CHECK_EQ(r.mismatches()[0].sample_idx, 10);
		CHECK_EQ(r.mismatches()[0].description, std::string("first"));
		CHECK_EQ(r.mismatches()[0].offset, std::string("0xa / 0xb"));
		CHECK_EQ(r.mismatches()[1].track_idx, 1);
		CHECK_EQ(r.mismatches()[1].sample_idx, 20);
		CHECK_EQ(r.mismatches()[2].description, std::string("third"));
	}

	// Frames with no mismatches: still Success
	{
		AnalyzeReport r;
		for (int i = 0; i < 100; i++)
			r.onFrameAnalyzed();
		CHECK_EQ(r.status(), AnalyzeStatus::Success);
		CHECK_EQ(r.exitCode(), 0);
	}
}
