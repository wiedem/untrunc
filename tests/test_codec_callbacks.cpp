#include "test_harness.h"
#include "codec/codec.h"
#include "track/track.h"

// Tests for Codec typed-dependency callbacks introduced in the Mp4 back-pointer
// removal refactoring. No FFmpeg codec parsing, no file I/O, no Mp4 dependency.

void test_codec_callbacks() {
	std::cout << "test_codec_callbacks:\n";

	// loadAfter(n) invokes load_after_fn_ with cur_off_ + n.
	// Verifies the offset arithmetic that replaced mp4_->loadFragment().
	{
		Codec c;
		c.cur_off_ = 100;
		off_t received = -1;
		static uchar dummy_byte = 0;
		c.load_after_fn_ = [&received](off_t off) -> const uchar * {
			received = off;
			return &dummy_byte;
		};
		c.loadAfter(50);
		CHECK_EQ(received, (off_t)150);
	}

	// getTrack() returns the Track* stored in track_, replacing the former mp4_
	// back-pointer for codec-to-track access.
	{
		Track t("dummy");
		Codec c;
		c.track_ = &t;
		CHECK(c.getTrack() == &t);
	}
}
