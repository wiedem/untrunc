#include "test_harness.h"
#include "core/mp4_scan.h"

// Helper: build an 8-byte "atom header" (big-endian size + 4-char name).
static void makeAtomHeader(uchar *buf, uint32_t size, const char *name) {
	buf[0] = (size >> 24) & 0xff;
	buf[1] = (size >> 16) & 0xff;
	buf[2] = (size >> 8) & 0xff;
	buf[3] = (size) & 0xff;
	buf[4] = name[0];
	buf[5] = name[1];
	buf[6] = name[2];
	buf[7] = name[3];
}

void test_mp4_scan() {
	std::cout << "test_mp4_scan:\n";

	// mdatHeaderSkipSize: returns 8 for "mdat", 0 otherwise
	{
		uchar buf[8];
		makeAtomHeader(buf, 100, "mdat");
		CHECK_EQ(mdatHeaderSkipSize(buf), 8);
	}
	{
		uchar buf[8];
		makeAtomHeader(buf, 100, "moov");
		CHECK_EQ(mdatHeaderSkipSize(buf), 0);
	}
	{
		uchar buf[8];
		makeAtomHeader(buf, 100, "free");
		CHECK_EQ(mdatHeaderSkipSize(buf), 0);
	}

	// atomSkipSize: returns atom size for skippable atoms
	{
		uchar buf[8];
		makeAtomHeader(buf, 256, "free");
		CHECK_EQ(atomSkipSize(buf, 256), 256);
	}
	{
		uchar buf[8];
		makeAtomHeader(buf, 256, "moov");
		CHECK_EQ(atomSkipSize(buf, 256), 256);
	}

	// atomSkipSize: "tmcd" is never skipped (may be payload)
	{
		uchar buf[8];
		makeAtomHeader(buf, 100, "tmcd");
		CHECK_EQ(atomSkipSize(buf, 200), 0);
	}

	// atomSkipSize: atom >= 1 MiB is not skipped
	{
		uchar buf[8];
		makeAtomHeader(buf, 1u << 20, "free");
		CHECK_EQ(atomSkipSize(buf, (int64_t)(2u << 20)), 0);
	}

	// atomSkipSize: atom exceeds remaining bytes
	{
		uchar buf[8];
		makeAtomHeader(buf, 500, "free");
		CHECK_EQ(atomSkipSize(buf, 100), 0);
	}

	// atomSkipSize: invalid atom name (non-printable byte) → 0
	{
		uchar buf[8] = {0, 0, 0, 100, 0x01, 0x02, 0x03, 0x04};
		CHECK_EQ(atomSkipSize(buf, 200), 0);
	}

	// atomSkipSize: zero-length atom → 0
	{
		uchar buf[8];
		makeAtomHeader(buf, 0, "free");
		CHECK_EQ(atomSkipSize(buf, 200), 0);
	}

	// atomSkipSize: 1 MiB boundary (1<<20 = 1048576).
	// Atoms at or above this size are not skipped to avoid misidentifying
	// large codec payload as a container atom.
	{
		uchar buf[8];
		// Exactly 1 MiB: not skipped.
		makeAtomHeader(buf, 1u << 20, "free");
		CHECK_EQ(atomSkipSize(buf, (int64_t)(2u << 20)), 0);
	}
	{
		uchar buf[8];
		// One byte below 1 MiB: skipped (within the allowed range).
		uint32_t just_below = (1u << 20) - 1;
		makeAtomHeader(buf, just_below, "free");
		CHECK_EQ(atomSkipSize(buf, (int64_t)(2u << 20)), (int)just_below);
	}
}
