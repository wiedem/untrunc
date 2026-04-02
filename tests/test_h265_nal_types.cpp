#include "test_harness.h"
#include "codec/hvc1/nal.h"

void test_h265_nal_types() {
	std::cout << "test_h265_nal_types:\n";

	// Trailing/sub-layer/leading slice types (0-9): isSlice=true, isKeyframe=false
	for (int t = NAL_TRAIL_N; t <= NAL_RASL_R; t++) {
		CHECK(h265IsSlice(t));
		CHECK(!h265IsKeyframe(t));
	}

	// Boundary: last trailing slice / first reserved gap
	CHECK(h265IsSlice(9));
	CHECK(!h265IsSlice(10));
	CHECK(!h265IsKeyframe(9));
	CHECK(!h265IsKeyframe(10));

	// Reserved non-IRAP gap (10-15): neither slice nor keyframe
	for (int t = 10; t <= 15; t++) {
		CHECK(!h265IsSlice(t));
		CHECK(!h265IsKeyframe(t));
	}

	// Boundary: last reserved gap / first IRAP
	CHECK(!h265IsSlice(15));
	CHECK(!h265IsKeyframe(15));
	CHECK(h265IsSlice(16));
	CHECK(h265IsKeyframe(16));

	// IRAP types (16-23): both isSlice and isKeyframe
	for (int t = NAL_BLA_W_LP; t <= NAL_RSV_IRAP_23; t++) {
		CHECK(h265IsSlice(t));
		CHECK(h265IsKeyframe(t));
	}

	// Named IRAP types
	CHECK(h265IsKeyframe(NAL_BLA_W_LP));
	CHECK(h265IsKeyframe(NAL_BLA_W_RADL));
	CHECK(h265IsKeyframe(NAL_BLA_N_LP));
	CHECK(h265IsKeyframe(NAL_IDR_W_RADL));
	CHECK(h265IsKeyframe(NAL_IDR_N_LP));
	CHECK(h265IsKeyframe(NAL_CRA_NUT));
	CHECK(h265IsKeyframe(NAL_RSV_IRAP_22));
	CHECK(h265IsKeyframe(NAL_RSV_IRAP_23));

	// Boundary: last IRAP / first non-VCL
	CHECK(h265IsSlice(23));
	CHECK(h265IsKeyframe(23));
	CHECK(!h265IsSlice(24));
	CHECK(!h265IsKeyframe(24));

	// Non-VCL gap (24-31): neither slice nor keyframe
	for (int t = 24; t <= 31; t++) {
		CHECK(!h265IsSlice(t));
		CHECK(!h265IsKeyframe(t));
	}

	// Non-VCL parameter set and control types (32+)
	for (int t : {(int)H265_NAL_VPS, (int)H265_NAL_SPS, (int)H265_NAL_PPS, (int)H265_NAL_AUD, (int)NAL_EOB_NUT,
	              (int)H265_NAL_FILLER_DATA}) {
		CHECK(!h265IsSlice(t));
		CHECK(!h265IsKeyframe(t));
	}
}
