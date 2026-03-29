#include "test_harness.h"
#include "codec/avc1/nal-slice.h"
#include "util/common.h"

// SliceInfo::isInNewFrame implements the H.264 spec rule for detecting
// the start of a new picture (7.4.1.2.4). Tests here exercise each
// discriminating field individually and the strict_nal_frame_check flag.

static SliceInfo make_slice(int frame_num, int pps_id = 0, int idr_pic_flag = 0, int field_pic_flag = 0,
                            int poc_type = 2, int poc_lsb = 0, int idr_pic_id = 0) {
	SliceInfo s;
	s.frame_num = frame_num;
	s.pps_id = pps_id;
	s.idr_pic_flag = idr_pic_flag;
	s.field_pic_flag = field_pic_flag;
	s.poc_type = poc_type;
	s.poc_lsb = poc_lsb;
	s.idr_pic_id = idr_pic_id;
	return s;
}

void test_slice_info() {
	std::cout << "test_slice_info:\n";
	const bool saved_strict = g_options.strict_nal_frame_check;

	// Slices with all fields equal are part of the same frame
	{
		auto prev = make_slice(5);
		auto cur = make_slice(5);
		CHECK(!cur.isInNewFrame(prev));
	}

	// Different frame_num always indicates a new frame
	{
		auto prev = make_slice(5);
		auto cur = make_slice(6);
		CHECK(cur.isInNewFrame(prev));
	}

	// Different pps_id with same frame_num → new frame
	{
		auto prev = make_slice(5, /*pps_id=*/0);
		auto cur = make_slice(5, /*pps_id=*/1);
		CHECK(cur.isInNewFrame(prev));
	}

	// IDR vs non-IDR slice: idr_pic_flag change triggers new frame
	{
		auto prev = make_slice(5, 0, /*idr_pic_flag=*/0);
		auto cur = make_slice(5, 0, /*idr_pic_flag=*/1);
		CHECK(cur.isInNewFrame(prev));
	}

	// Different field_pic_flag → new frame
	{
		SliceInfo prev = make_slice(5);
		SliceInfo cur = make_slice(5);
		prev.field_pic_flag = 0;
		cur.field_pic_flag = 1;
		CHECK(cur.isInNewFrame(prev));
	}

	// poc_lsb difference: detected only when strict mode is on and poc_type==0
	{
		g_options.strict_nal_frame_check = true;
		auto prev = make_slice(5, 0, 0, 0, /*poc_type=*/0, /*poc_lsb=*/8);
		auto cur = make_slice(5, 0, 0, 0, /*poc_type=*/0, /*poc_lsb=*/10);
		CHECK(cur.isInNewFrame(prev));

		g_options.strict_nal_frame_check = false;
		CHECK(!cur.isInNewFrame(prev)); // same slices, but check is now skipped
	}

	// idr_pic_id difference: triggers only in strict mode when both slices are IDR
	{
		g_options.strict_nal_frame_check = true;
		auto prev = make_slice(5, 0, /*idr_pic_flag=*/1, 0, 2, 0, /*idr_pic_id=*/1);
		auto cur = make_slice(5, 0, /*idr_pic_flag=*/1, 0, 2, 0, /*idr_pic_id=*/2);
		CHECK(cur.isInNewFrame(prev));

		g_options.strict_nal_frame_check = false;
		CHECK(!cur.isInNewFrame(prev));
	}

	// poc_lsb check does not fire when poc_type != 0 (even in strict mode)
	{
		g_options.strict_nal_frame_check = true;
		// poc_type=1: poc_lsb is irrelevant for the lsb check
		auto prev = make_slice(5, 0, 0, 0, /*poc_type=*/1, /*poc_lsb=*/0);
		auto cur = make_slice(5, 0, 0, 0, /*poc_type=*/1, /*poc_lsb=*/99);
		CHECK(!cur.isInNewFrame(prev));
	}

	g_options.strict_nal_frame_check = saved_strict;
}
