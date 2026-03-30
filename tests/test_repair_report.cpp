#include "test_harness.h"
#include "core/repair_report.h"

#include <iostream>
#include <string>

void test_repair_report() {
	std::cout << "test_repair_report:\n";

	// Default state is Success, exit 0
	{
		RepairReport r;
		CHECK_EQ(r.status(), RepairStatus::Success);
		CHECK_EQ(r.exitCode(), 0);
	}

	// Fatal error -> Error, exit 1
	{
		RepairReport r;
		r.onFatalError("cannot read file");
		CHECK_EQ(r.status(), RepairStatus::Error);
		CHECK_EQ(r.exitCode(), 1);
	}

	// Unknown sequences -> Partial, exit 2
	{
		RepairReport r;
		r.onUnknownSequences({{"0x100 / 0x200", 512}, {"0x300 / 0x400", 512}}, 0.5);
		CHECK_EQ(r.status(), RepairStatus::Partial);
		CHECK_EQ(r.exitCode(), 2);
		CHECK_EQ(r.unknownSeqCount(), (size_t)2);
		CHECK_EQ(r.unknownSeqBytes(), (int64_t)1024);
	}

	// Premature end -> Partial, exit 2
	{
		RepairReport r;
		r.onPrematureEnd(73.4);
		CHECK(r.isPrematureEnd());
		CHECK_EQ(r.status(), RepairStatus::Partial);
		CHECK_EQ(r.exitCode(), 2);
	}

	// Fatal overrides partial (unknown sequences also present)
	{
		RepairReport r;
		r.onUnknownSequences({{"0x0", 2048}}, 1.0);
		r.onFatalError("out of memory");
		CHECK_EQ(r.status(), RepairStatus::Error);
		CHECK_EQ(r.exitCode(), 1);
	}

	// Fatal overrides partial (premature end also present)
	{
		RepairReport r;
		r.onPrematureEnd(50.0);
		r.onFatalError("write failed");
		CHECK_EQ(r.status(), RepairStatus::Error);
		CHECK_EQ(r.exitCode(), 1);
	}

	// Premature end + unknown sequences: still Partial (not Error)
	{
		RepairReport r;
		r.onPrematureEnd(40.0);
		r.onUnknownSequences({{"0x0", 512}}, 0.1);
		CHECK_EQ(r.status(), RepairStatus::Partial);
		CHECK_EQ(r.exitCode(), 2);
	}

	// Warnings accumulate in order
	{
		RepairReport r;
		r.onWarning("first");
		r.onWarning("second");
		r.onWarning("third");
		CHECK_EQ((int)r.warnings().size(), 3);
		CHECK_EQ(r.warnings()[0].message, std::string("first"));
		CHECK_EQ(r.warnings()[1].message, std::string("second"));
		CHECK_EQ(r.warnings()[2].message, std::string("third"));
	}

	// Multiple errors accumulate
	{
		RepairReport r;
		r.onFatalError("error one");
		r.onFatalError("error two");
		CHECK_EQ((int)r.errors().size(), 2);
		CHECK_EQ(r.exitCode(), 1);
	}

	// Track stats
	{
		RepairReport r;
		r.onTrackStats({{"mp4a", 500, 0}, {"hvc1", 499, 50}});
		CHECK_EQ(r.chunksRepaired(), (int64_t)999);
		CHECK_EQ((int)r.trackStats().size(), 2);
		CHECK_EQ(r.trackStats()[0].name, std::string("mp4a"));
		CHECK_EQ(r.trackStats()[0].samples, (int64_t)500);
		CHECK_EQ(r.trackStats()[1].keyframes, (int64_t)50);
	}

	// Unknown sequence details
	{
		RepairReport r;
		r.onUnknownSequences({{"0x1000 / 0x2000", 4096}, {"0x3000 / 0x4000", 8192}}, 1.23);
		CHECK_EQ(r.unknownSeqCount(), (size_t)2);
		CHECK_EQ(r.unknownSeqBytes(), (int64_t)(4096 + 8192));
		CHECK_EQ(r.unknownSeqs()[0].offset, std::string("0x1000 / 0x2000"));
		CHECK_EQ(r.unknownSeqs()[0].length, (int64_t)4096);
		CHECK_EQ(r.unknownSeqs()[1].length, (int64_t)8192);
	}

	// Output file accessor
	{
		RepairReport r;
		r.onOutputFile("/tmp/out_fixed.mp4");
		CHECK_EQ(r.outputFile(), std::string("/tmp/out_fixed.mp4"));
	}
}
