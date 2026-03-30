#include "test_harness.h"
#include "util/bitreader.h"

void test_bitreader() {
	std::cout << "test_bitreader:\n";

	// readBits: consume exactly 8 bits from an aligned byte
	{
		uint8_t buf[] = {0xAB};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readBits(8, p, off), 0xABu);
		CHECK_EQ(off, 0);
		CHECK(p == buf + 1);
	}

	// readBits: two successive 4-bit reads from a single byte
	{
		uint8_t buf[] = {0xAB}; // 1010 1011
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readBits(4, p, off), 0xAu); // high nibble
		CHECK_EQ(off, 4);
		CHECK_EQ(readBits(4, p, off), 0xBu); // low nibble
		CHECK_EQ(off, 0);
		CHECK(p == buf + 1);
	}

	// readBits: 1 bit at a time verifies per-bit offset advancement
	{
		uint8_t buf[] = {0b10110000};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readBits(1, p, off), 1u);
		CHECK_EQ(readBits(1, p, off), 0u);
		CHECK_EQ(readBits(1, p, off), 1u);
		CHECK_EQ(readBits(1, p, off), 1u);
		CHECK_EQ(off, 4);
	}

	// readBits: crossing a byte boundary at mid-byte offset
	// 0xAB=1010 1011, 0xCD=1100 1101; reading 8 bits from offset 4:
	//   low 4 of 0xAB (1011) + high 4 of 0xCD (1100) = 0xBC
	{
		uint8_t buf[] = {0xAB, 0xCD};
		const uchar *p = buf;
		int off = 4;
		CHECK_EQ(readBits(8, p, off), 0xBCu);
		CHECK_EQ(off, 4);
		CHECK(p == buf + 1);
	}

	// readBits: 24 bits spanning 3 bytes
	{
		uint8_t buf[] = {0x01, 0x02, 0x03};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readBits(24, p, off), 0x010203u);
	}

	// readGolomb: values 0-6, all encodable within a single byte
	// Unsigned Exp-Golomb: value n has k=floor(log2(n+1)) leading zeros,
	// then a 1, then k remainder bits. The first non-zero bit of each
	// test byte is the delimiter.
	struct {
		uint8_t byte;
		int expected;
	} golomb_cases[] = {
	    {0x80, 0}, // 1...
	    {0x40, 1}, // 010...
	    {0x60, 2}, // 011...
	    {0x20, 3}, // 00100...
	    {0x28, 4}, // 00101...
	    {0x30, 5}, // 00110...
	    {0x38, 6}, // 00111...
	};

	for (auto &c : golomb_cases) {
		uint8_t buf[2] = {c.byte, 0x00};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readGolomb(p, off), c.expected);
	}

	// readGolomb: value 7 uses 7 bits (crosses into second byte when padded)
	// 7 = 0001000: 3 leading zeros, delimiter 1, remainder 000
	// In one byte: 00010000 = 0x10
	{
		uint8_t buf[] = {0x10, 0x00};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readGolomb(p, off), 7);
	}

	// readGolomb: four consecutive value-0 reads from a packed stream
	// Each 0 is encoded as a single 1 bit; 1111xxxx = 0xF0
	{
		uint8_t buf[] = {0xF0, 0x00};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readGolomb(p, off), 0);
		CHECK_EQ(readGolomb(p, off), 0);
		CHECK_EQ(readGolomb(p, off), 0);
		CHECK_EQ(readGolomb(p, off), 0);
		// consumed exactly 4 bits
		CHECK_EQ(off, 4);
		CHECK(p == buf);
	}

	// readGolomb: value 1 (010) then value 0 (1) packed in one byte
	// 010 1 xxxx = 0101 0000 = 0x50
	{
		uint8_t buf[] = {0x50, 0x00};
		const uchar *p = buf;
		int off = 0;
		CHECK_EQ(readGolomb(p, off), 1);
		CHECK_EQ(readGolomb(p, off), 0);
	}
}
