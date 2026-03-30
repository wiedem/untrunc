#include "test_harness.h"
#include <cstring>

#include "codec/hvc1/hvc1.h"
#include "track/sample_stats.h"

// Each synthetic NAL unit: 4-byte big-endian length prefix + 2-byte NAL header
// + kDataBytes bytes of zeroed payload.
static constexpr int kDataBytes = 8;               // >= 8 satisfies the slice minimum-size check
static constexpr int kNalPayload = 2 + kDataBytes; // NAL header + data
static constexpr int kNalSize = 4 + kNalPayload;   // length prefix + payload

// Write one AVCC-format H265 NAL unit into buf.
// nal_type: H265 NAL unit type (e.g. 1=TRAIL_R, 32=VPS, 33=SPS, 34=PPS, 19=IDR_W_RADL)
// first_slice_flag: sets first_slice_segment_in_pic_flag (bit 7 of first data byte)
static void write_nal(uchar *buf, int nal_type, int first_slice_flag = 0) {
	// 4-byte big-endian length (number of bytes after the length field)
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = (uchar)kNalPayload;
	// NAL header byte 0: nal_unit_type occupies bits 6:1
	buf[4] = (uchar)((nal_type << 1) & 0xfe);
	// NAL header byte 1: nuh_temporal_id_plus1 must be non-zero for non-EOB NALs
	buf[5] = 0x01;
	// First data byte encodes first_slice_segment_in_pic_flag in bit 7
	buf[6] = first_slice_flag ? 0x80 : 0x00;
	memset(buf + 7, 0, kDataBytes - 1);
}

// Write one AVCC-format H265 NAL unit with a variable-size length prefix (lsz = 1, 2, or 4).
static int nal_size_lsz(int lsz) {
	return lsz + kNalPayload;
}

static void write_nal_lsz(uchar *buf, int lsz, int nal_type, int first_slice_flag = 0) {
	switch (lsz) {
	case 1:
		buf[0] = (uchar)kNalPayload;
		break;
	case 2:
		buf[0] = 0;
		buf[1] = (uchar)kNalPayload;
		break;
	default: // 4
		buf[0] = 0; buf[1] = 0; buf[2] = 0;
		buf[3] = (uchar)kNalPayload;
		break;
	}
	buf[lsz]     = (uchar)((nal_type << 1) & 0xfe);
	buf[lsz + 1] = 0x01; // nuh_temporal_id_plus1 = 1
	buf[lsz + 2] = first_slice_flag ? 0x80 : 0x00;
	memset(buf + lsz + 3, 0, kDataBytes - 1);
}

// BUG-001: getLengths() in hvc1.cpp overshoots into the next keyframe's inline
// VPS/SPS/PPS NAL units when no AUD precedes the keyframe.
//
// This test builds a buffer with two logical access units:
//   Sample N:   [TRAIL_R slice]
//   Sample N+1: [VPS][SPS][PPS][IDR_W_RADL]  (no AUD before the keyframe)
//
// hvc1GetLengths() is called with the full buffer (maxlength covers both
// samples). The correct result is length == kNalSize (one TRAIL_R NAL).
// Without the fix it returns length == 4 * kNalSize (TRAIL_R + VPS + SPS + PPS).
void test_hvc1() {
	std::cout << "test_hvc1:\n";

	// Build the buffer: TRAIL_R | VPS | SPS | PPS | IDR_W_RADL
	static constexpr int kNumNals = 5;
	uchar buf[kNumNals * kNalSize];

	write_nal(buf + 0 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
	write_nal(buf + 1 * kNalSize, /*VPS*/ 32);
	write_nal(buf + 2 * kNalSize, /*SPS*/ 33);
	write_nal(buf + 3 * kNalSize, /*PPS*/ 34);
	write_nal(buf + 4 * kNalSize, /*IDR*/ 19, /*first_slice_flag=*/1);

	// Default SampleSizeStats: upper_bound=0 and lower_bound=0, so
	// wouldExceed() and isBigEnough() always return false. The loop therefore
	// only stops at an explicit return inside getLengths.
	SampleSizeStats stats;

	bool was_keyframe = false;
	// loadAfter callback: the buffer is contiguous, so just return buf+offset.
	auto load_after = [&](off_t offset) -> const uchar * { return buf + offset; };

	auto r = hvc1GetLengths(buf, sizeof(buf), &stats, was_keyframe, load_after);

	// Without the fix: r.length == 4 * kNalSize (TRAIL_R + VPS + SPS + PPS).
	// With the fix:    r.length == kNalSize      (TRAIL_R only).
	CHECK_EQ(r.length, kNalSize);

	// AUD as access-unit separator: [TRAIL_R][AUD][IDR] -> stops after TRAIL_R.
	// When AUD arrives and a previous slice was already seen (previous_nal.is_ok=true),
	// it signals the end of the current access unit.
	{
		uchar buf2[3 * kNalSize];
		write_nal(buf2 + 0 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
		write_nal(buf2 + 1 * kNalSize, /*AUD*/ 35);
		write_nal(buf2 + 2 * kNalSize, /*IDR*/ 19, /*first_slice_flag=*/1);

		SampleSizeStats stats2;
		bool was_kf2 = false;
		auto la2 = [&](off_t o) -> const uchar * { return buf2 + o; };
		auto r2 = hvc1GetLengths(buf2, sizeof(buf2), &stats2, was_kf2, la2);
		CHECK_EQ(r2.length, kNalSize); // stops after TRAIL_R
	}

	// AUD as first NAL of access unit: [AUD][IDR] -> both included in the same chunk.
	// AUD is skipped without stopping when no prior slice has been seen.
	{
		uchar buf3[2 * kNalSize];
		write_nal(buf3 + 0 * kNalSize, /*AUD*/ 35);
		write_nal(buf3 + 1 * kNalSize, /*IDR*/ 19, /*first_slice_flag=*/1);

		SampleSizeStats stats3;
		bool was_kf3 = false;
		auto la3 = [&](off_t o) -> const uchar * { return buf3 + o; };
		auto r3 = hvc1GetLengths(buf3, sizeof(buf3), &stats3, was_kf3, la3);
		CHECK_EQ(r3.length, 2 * kNalSize); // AUD + IDR both in same chunk
		CHECK(was_kf3);
	}

	// Forbidden bit on first NAL: parseNal returns is_ok=false -> length=0.
	{
		uchar buf4[kNalSize];
		write_nal(buf4, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
		buf4[4] |= 0x80; // set forbidden_zero_bit in NAL header byte 0

		SampleSizeStats stats4;
		bool was_kf4 = false;
		auto la4 = [&](off_t o) -> const uchar * { return buf4 + o; };
		auto r4 = hvc1GetLengths(buf4, sizeof(buf4), &stats4, was_kf4, la4);
		CHECK_EQ(r4.length, 0);
	}

	// Forbidden bit on second NAL: stops after the first valid NAL.
	{
		uchar buf5[2 * kNalSize];
		write_nal(buf5 + 0 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
		write_nal(buf5 + 1 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/0);
		buf5[kNalSize + 4] |= 0x80; // set forbidden bit on second NAL

		SampleSizeStats stats5;
		bool was_kf5 = false;
		auto la5 = [&](off_t o) -> const uchar * { return buf5 + o; };
		auto r5 = hvc1GetLengths(buf5, sizeof(buf5), &stats5, was_kf5, la5);
		CHECK_EQ(r5.length, kNalSize); // stops after first valid NAL
	}

	// NAL length size 1 and 2: single TRAIL_R parsed correctly.
	{
		for (int lsz : {1, 2}) {
			uchar buf6[kNalSize]; // generously sized
			write_nal_lsz(buf6, lsz, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);

			SampleSizeStats stats6;
			bool was_kf6 = false;
			auto la6 = [&](off_t o) -> const uchar * { return buf6 + o; };
			auto r6 = hvc1GetLengths(buf6, nal_size_lsz(lsz), &stats6, was_kf6, la6, lsz);
			CHECK_EQ(r6.length, nal_size_lsz(lsz));
		}
	}

	// wouldExceed: stats with upper_bound just above one NAL stops before the second.
	// exceedsAllowed uses strict >: length+additional > upper_bound.
	{
		uchar buf7[2 * kNalSize];
		write_nal(buf7 + 0 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
		write_nal(buf7 + 1 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/0);

		SampleSizeStats stats7;
		stats7.normal.upper_bound = kNalSize + 1; // 2*kNalSize > kNalSize+1 triggers stop

		bool was_kf7 = false;
		auto la7 = [&](off_t o) -> const uchar * { return buf7 + o; };
		auto r7 = hvc1GetLengths(buf7, sizeof(buf7), &stats7, was_kf7, la7);
		CHECK_EQ(r7.length, kNalSize); // stops before second NAL
	}

	// isBigEnough: records alternative lengths when lower_bound is reached.
	// With lower_bound=kNalSize and upper_bound=3*kNalSize, lengths kNalSize and
	// 2*kNalSize are recorded as alternatives before the final length 3*kNalSize.
	{
		uchar buf8[3 * kNalSize];
		for (int i = 0; i < 3; i++)
			write_nal(buf8 + i * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/0);

		SampleSizeStats stats8;
		stats8.normal.lower_bound = kNalSize;
		stats8.normal.upper_bound = 3 * kNalSize;

		bool was_kf8 = false;
		auto la8 = [&](off_t o) -> const uchar * { return buf8 + o; };
		auto r8 = hvc1GetLengths(buf8, sizeof(buf8), &stats8, was_kf8, la8);
		CHECK_EQ(r8.length, 3 * kNalSize);
		CHECK_EQ((int)r8.alternative_lengths.size(), 2);
		CHECK_EQ(r8.alternative_lengths[0], kNalSize);
		CHECK_EQ(r8.alternative_lengths[1], 2 * kNalSize);
	}
}
