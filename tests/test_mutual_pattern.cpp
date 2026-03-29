#include "test_harness.h"
#include "util/mutual_pattern.h"

// MutualPattern(a, b) tracks which byte positions are identical between
// the two buffers. Only those "mutual" positions are checked by doesMatch.

void test_mutual_pattern() {
	std::cout << "test_mutual_pattern:\n";
	using ByteArr = std::vector<uchar>;

	// Identical buffers: every position is mutual
	{
		ByteArr a = {0x11, 0x22, 0x33, 0x44};
		ByteArr b = a;
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_, 4u);
		CHECK(mp.doesMatch(a.data()));
	}

	// Completely disjoint buffers: no mutual positions.
	// doesMatch is vacuously true for any input because size_mutual_==0
	// and intersectLen returns 0 == 0.
	{
		ByteArr a = {0x01, 0x02, 0x03, 0x04};
		ByteArr b = {0xFF, 0xFE, 0xFD, 0xFC};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_, 0u);
		uint8_t any[] = {0xAA, 0xBB, 0xCC, 0xDD};
		CHECK(mp.doesMatch(any));
	}

	// Partial overlap: positions 1 and 3 differ, positions 0, 2, 4 are shared.
	// doesMatch ignores non-mutual positions; it checks only the 3 shared bytes.
	{
		ByteArr a = {0x01, 0x02, 0x03, 0x04, 0x05};
		ByteArr b = {0x01, 0xFF, 0x03, 0xFF, 0x05};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_, 3u);

		// Buffer with the same values at mutual positions 0, 2, 4
		uint8_t match[] = {0x01, 0xAA, 0x03, 0xBB, 0x05};
		CHECK(mp.doesMatch(match));

		// Buffer with a wrong value at mutual position 0
		uint8_t no_match[] = {0xFF, 0x02, 0x03, 0x04, 0x05};
		CHECK(!mp.doesMatch(no_match));
	}

	// intersectBufIf: narrows the pattern when the new buffer is consistent
	// with some but not all mutual positions (0 < intersect < size_mutual_).
	{
		ByteArr a = {0x01, 0x02, 0x03, 0x04};
		ByteArr b = {0x01, 0xFF, 0x03, 0xFF};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_, 2u); // positions 0 and 2

		// c disagrees at position 0 (mutual) but agrees at position 2
		ByteArr c = {0xFF, 0x02, 0x03, 0x04};
		bool narrowed = mp.intersectBufIf(c);
		CHECK(narrowed);
		CHECK_EQ(mp.size_mutual_, 1u); // only position 2 remains

		// Now only position 2 (value 0x03) is the gatekeeper
		uint8_t with_03[] = {0xAA, 0xBB, 0x03, 0xDD};
		CHECK(mp.doesMatch(with_03));
		uint8_t wrong_at_2[] = {0x01, 0x02, 0xFF, 0x04};
		CHECK(!mp.doesMatch(wrong_at_2));
	}

	// intersectBufIf: does NOT narrow when the buffer matches all mutual positions.
	// It just increments the counter, signalling "this buffer is already consistent".
	{
		ByteArr a = {0x01, 0x02, 0x03};
		ByteArr b = {0x01, 0xFF, 0x03};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_, 2u);
		CHECK_EQ(mp.cnt_, 0);

		ByteArr full_match = {0x01, 0x42, 0x03};
		bool narrowed = mp.intersectBufIf(full_match, true);
		CHECK(!narrowed);
		CHECK_EQ(mp.size_mutual_, 2u); // unchanged
		CHECK_EQ(mp.cnt_, 1);          // was counted
	}

	// getDistinct: returns only the byte values at mutual positions, in order
	{
		ByteArr a = {0x11, 0x22, 0x33};
		ByteArr b = {0x11, 0xFF, 0x33};
		MutualPattern mp(a, b);
		auto distinct = mp.getDistinct();
		CHECK_EQ(distinct.size(), 2u);
		CHECK_EQ(distinct[0], (uchar)0x11);
		CHECK_EQ(distinct[1], (uchar)0x33);
	}

	// doesMatchHalf: checks whether a short buffer matches the second half of
	// the pattern at the mutual positions. Used by anyPatternMatchesHalf to
	// probe context bytes just before a candidate offset.
	{
		// All positions mutual: second half = {0x33, 0x44}.
		ByteArr a = {0x11, 0x22, 0x33, 0x44};
		ByteArr b = {0x11, 0x22, 0x33, 0x44};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_half_, 2u);

		uint8_t match[] = {0x33, 0x44};
		CHECK(mp.doesMatchHalf(match));

		uint8_t no_match[] = {0x11, 0x22};
		CHECK(!mp.doesMatchHalf(no_match));
	}
	{
		// Partially mutual: only position 2 is mutual in the second half.
		// doesMatchHalf must honour the is_mutual_ mask and ignore position 3.
		ByteArr a = {0x11, 0x22, 0x33, 0x44};
		ByteArr b = {0xFF, 0xFF, 0x33, 0xFF};
		MutualPattern mp(a, b);
		CHECK_EQ(mp.size_mutual_half_, 1u);

		uint8_t match_pos2[] = {0x33, 0xAA}; // position 3 is not mutual, any value passes
		CHECK(mp.doesMatchHalf(match_pos2));

		uint8_t wrong_pos2[] = {0xFF, 0x44};
		CHECK(!mp.doesMatchHalf(wrong_pos2));
	}

	// doesMatchApprox: returns true when >= 79% of mutual positions match AND
	// the matching bytes have Shannon entropy > 0.75 bits (or 100% match).
	// Used for permissive pattern matching during unknown-sequence recovery.
	{
		// 100% of mutual positions match: takes the early-return path.
		ByteArr a = {0x01, 0x02, 0x03, 0x04, 0x05};
		MutualPattern mp_full(a, a);
		uint8_t all_match[] = {0x01, 0x02, 0x03, 0x04, 0x05};
		CHECK(mp_full.doesMatchApprox(all_match));

		// 0% match: ratio 0/5 < 0.79 -> false.
		uint8_t none_match[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		CHECK(!mp_full.doesMatchApprox(none_match));
	}
	{
		// 4 of 5 mutual positions match (80% > 0.79).
		// The 4 matching bytes are all identical (0x00): entropy = 0 < 0.75 -> false.
		ByteArr a_low = {0x00, 0x00, 0x00, 0x00, 0x55};
		MutualPattern mp_low(a_low, a_low);
		uint8_t buf_low_ent[] = {0x00, 0x00, 0x00, 0x00, 0xFF}; // 4/5 match, entropy = 0
		CHECK(!mp_low.doesMatchApprox(buf_low_ent));
	}
	{
		// 4 of 5 mutual positions match (80% > 0.79).
		// The 4 matching bytes are all distinct: entropy = log2(4) = 2.0 > 0.75 -> true.
		ByteArr a_high = {0x01, 0x02, 0x04, 0x08, 0x55};
		MutualPattern mp_high(a_high, a_high);
		uint8_t buf_high_ent[] = {0x01, 0x02, 0x04, 0x08, 0xFF}; // 4/5 match
		CHECK(mp_high.doesMatchApprox(buf_high_ent));
	}
}
