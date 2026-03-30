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
void test_hvc1();
void test_track_scanner();
void test_codec_callbacks();
void test_dyn_stats_builder();
void test_h265_nal_types();
void test_sample_stats();
void test_repair_report();
void test_analyze_report();
void test_avc_config();

int main() {
	test_bitreader();
	test_common();
	test_mutual_pattern();
	test_track_order();
	test_slice_info();
	test_mp4_scan();
	test_repairer();
	test_hvc1();
	test_track_scanner();
	test_codec_callbacks();
	test_dyn_stats_builder();
	test_h265_nal_types();
	test_sample_stats();
	test_repair_report();
	test_analyze_report();
	test_avc_config();

	int total = g_tests_passed + g_tests_failed;
	std::cout << "\n" << g_tests_passed << "/" << total << " tests passed";
	if (g_tests_failed) std::cout << "  [" << g_tests_failed << " FAILED]";
	std::cout << "\n";
	return g_tests_failed ? 1 : 0;
}
