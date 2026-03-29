#include "test_harness.h"
#include "track/track_order.h"

#include <utility>
#include <vector>

// findOrder detects the repeating period in a chunk-order sequence.
// findOrderSimple does the same but only tracks track indices, not sample counts.

void test_track_order() {
	std::cout << "test_track_order:\n";
	using P = std::pair<int, int>;

	// Clean 2-entry repeating sequence
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {0, 10}, {1, 5}, {0, 10}, {1, 5}};
		bool ok = findOrder(data);
		CHECK(ok);
		CHECK_EQ((int)data.size(), 2);
		CHECK_EQ(data[0], P(0, 10));
		CHECK_EQ(data[1], P(1, 5));
	}

	// Clean 3-entry repeating sequence
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {2, 8}, {0, 10}, {1, 5}, {2, 8}, {0, 10}, {1, 5}, {2, 8}};
		bool ok = findOrder(data);
		CHECK(ok);
		CHECK_EQ((int)data.size(), 3);
		CHECK_EQ(data[0].first, 0);
		CHECK_EQ(data[1].first, 1);
		CHECK_EQ(data[2].first, 2);
	}

	// No repetition: data is cleared and false is returned
	{
		std::vector<P> data = {{0, 1}, {1, 2}, {2, 3}, {3, 4}};
		bool ok = findOrder(data);
		CHECK(!ok);
		CHECK(data.empty());
	}

	// Last-cycle tolerance: deviations that start at exactly data.size()-order_sz
	// are forgiven (models the final, possibly shorter, chunk at end of file).
	// order_sz=2, data.size()=5; tolerance fires when first_failed==3 (=5-2).
	// Here index 3 deviates from the expected pattern, triggering the tolerance.
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {0, 10}, {1, 9}, {0, 10}};
		bool ok = findOrder(data);
		CHECK(ok);
		CHECK_EQ((int)data.size(), 2);
		CHECK_EQ(data[0].first, 0);
		CHECK_EQ(data[1].first, 1);
	}

	// Deviation in the MIDDLE of the sequence: tolerance does not apply.
	// order_sz=2 (data[2] first matches data[0]), first_failed=3.
	// Tolerance requires first_failed == data.size()-order_sz = 4, so it does not fire.
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {0, 10}, {1, 99}, {0, 10}, {1, 5}};
		bool ok = findOrder(data);
		CHECK(!ok);
		CHECK(data.empty());
	}

	// findOrderSimple: recovers the cyclic track-index sequence
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {2, 8}, {0, 10}, {1, 5}, {2, 8}, {0, 10}, {1, 5}, {2, 8}};
		auto order = findOrderSimple(data);
		CHECK_EQ((int)order.size(), 3);
		CHECK_EQ(order[0], 0);
		CHECK_EQ(order[1], 1);
		CHECK_EQ(order[2], 2);
	}

	// findOrderSimple: inconsistency in the repeating track index → empty result
	// Period is {0,1}, but index 3 has track 2 instead of 1
	{
		std::vector<P> data = {{0, 10}, {1, 5}, {0, 10}, {2, 8}};
		auto order = findOrderSimple(data);
		CHECK(order.empty());
	}

	// findOrderSimple: single-track recording (only one track index in data)
	{
		std::vector<P> data = {{0, 100}, {0, 100}, {0, 100}, {0, 100}};
		auto order = findOrderSimple(data);
		CHECK_EQ((int)order.size(), 1);
		CHECK_EQ(order[0], 0);
	}

	// findOrder with ignore_first_failed=true: called by genTrackOrder when
	// it still wants the detected period even if there is a mid-sequence
	// deviation. Without the flag the same input would clear the result.
	{
		// Deviation at index 3 (mid-sequence, not in last cycle).
		// With ignore_first_failed=false this clears data (tested above).
		// With ignore_first_failed=true data keeps the 2-entry period.
		std::vector<P> data = {{0, 10}, {1, 5}, {0, 10}, {1, 99}, {0, 10}, {1, 5}};
		bool ok = findOrder(data, /*ignore_first_failed=*/true);
		CHECK(!ok);                    // deviation detected, still returns false
		CHECK_EQ((int)data.size(), 2); // but the order is preserved
		CHECK_EQ(data[0], P(0, 10));
		CHECK_EQ(data[1], P(1, 5));
	}

	// Last-cycle tolerance boundary: the tolerance fires only when
	// order_sz <= 4. With order_sz=5 a deviation in the last cycle is
	// treated as a real failure and the data is cleared.
	{
		// order_sz=5. Deviation at last position (index 9), which is NOT in the
		// last cycle window (data.size()-order_sz = 10-5 = 5, first_failed=9).
		// The tolerance condition (first_failed==data.size()-order_sz) does not
		// match, so the standard failure path clears the data.
		std::vector<P> data = {
		    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5},  // first cycle
		    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 99}, // deviation at index 9
		};
		bool ok = findOrder(data);
		CHECK(!ok);
		CHECK(data.empty());
	}
	{
		// order_sz=5. Deviation exactly at data.size()-order_sz = 5, which is
		// the position that WOULD trigger the tolerance for order_sz<=4.
		// Because order_sz=5 > 4, the tolerance does not apply and data is
		// cleared. Contrasts with the order_sz=2 last-cycle test above.
		std::vector<P> data = {
		    {0, 1},  {1, 2}, {2, 3}, {3, 4}, {4, 5}, // first cycle (defines order)
		    {0, 99}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, // deviation at index 5
		};
		bool ok = findOrder(data);
		CHECK(!ok);
		CHECK(data.empty());
	}
}
