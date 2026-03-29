#pragma once
#include <iostream>

extern int g_tests_passed;
extern int g_tests_failed;

#define CHECK(expr)                                                                                                    \
	do {                                                                                                               \
		if (expr) {                                                                                                    \
			++g_tests_passed;                                                                                          \
		} else {                                                                                                       \
			++g_tests_failed;                                                                                          \
			std::cerr << "  FAIL: " #expr " [" << __FILE__ << ":" << __LINE__ << "]\n";                                \
		}                                                                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                                                                 \
	do {                                                                                                               \
		if ((a) == (b)) {                                                                                              \
			++g_tests_passed;                                                                                          \
		} else {                                                                                                       \
			++g_tests_failed;                                                                                          \
			std::cerr << "  FAIL: " #a " == " #b " [" << __FILE__ << ":" << __LINE__ << "]\n";                         \
		}                                                                                                              \
	} while (0)
