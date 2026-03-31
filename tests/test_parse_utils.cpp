#include "test_harness.h"
#include "util/common.h"

#include <string>

void test_parse_byte_str() {
	std::cout << "test_parse_byte_str:\n";

	// Plain number
	{
		std::string s = "1024";
		CHECK_EQ(parseByteStr(s), 1024);
	}

	// Kilobytes: "k" suffix
	{
		std::string s = "4k";
		CHECK_EQ(parseByteStr(s), 4 * 1024);
	}

	// Megabytes: "m" suffix
	{
		std::string s = "2m";
		CHECK_EQ(parseByteStr(s), 2 * 1024 * 1024);
	}

	// With trailing "b": "kb"
	{
		std::string s = "8kb";
		CHECK_EQ(parseByteStr(s), 8 * 1024);
	}

	// With trailing "b": "mb"
	{
		std::string s = "3mb";
		CHECK_EQ(parseByteStr(s), 3 * 1024 * 1024);
	}

	// Plain number with trailing "b" (bytes)
	{
		std::string s = "512b";
		CHECK_EQ(parseByteStr(s), 512);
	}

	// Single digit
	{
		std::string s = "1";
		CHECK_EQ(parseByteStr(s), 1);
	}

	// Larger value
	{
		std::string s = "100m";
		CHECK_EQ(parseByteStr(s), 100 * 1024 * 1024);
	}
}
