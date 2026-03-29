#include "test_harness.h"
#include <iostream>

int g_tests_passed = 0;
int g_tests_failed = 0;

void test_bitreader();
void test_common();
void test_mutual_pattern();
void test_track_order();
void test_slice_info();
void test_mp4_scan();
void test_repairer();

int main() {
	test_bitreader();
	test_common();
	test_mutual_pattern();
	test_track_order();
	test_slice_info();
	test_mp4_scan();
	test_repairer();

	int total = g_tests_passed + g_tests_failed;
	std::cout << "\n" << g_tests_passed << "/" << total << " tests passed";
	if (g_tests_failed) std::cout << "  [" << g_tests_failed << " FAILED]";
	std::cout << "\n";
	return g_tests_failed ? 1 : 0;
}
