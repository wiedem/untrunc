#include "bitreader.h"

#include <iostream>

int readGolomb(const uchar *&buffer, int &offset) {
	// count the leading zeroes
	int count = 0;
	while ((*buffer & (0x1 << (7 - offset))) == 0) {
		count++;
		offset++;
		if (offset == 8) {
			buffer++;
			offset = 0;
		}
		if (count > 20) {
			std::cout << "Failed reading golomb: too large!\n";
			return -1;
		}
	}
	// skip the single 1 delimiter
	offset++;
	if (offset == 8) {
		buffer++;
		offset = 0;
	}
	uint32_t res = 1;
	// read count bits
	while (count-- > 0) {
		res <<= 1;
		res |= (*buffer & (0x1 << (7 - offset))) >> (7 - offset);
		offset++;
		if (offset == 8) {
			buffer++;
			offset = 0;
		}
	}
	return res - 1;
}

uint readBits(int n, const uchar *&buffer, int &offset) {
	uint res = 0;
	int d = 8 - offset;
	uint mask = ((1 << d) - 1);
	int to_rshift = d - n;
	if (to_rshift > 0) {
		res = (*buffer & mask) >> to_rshift;
		offset += n;
	} else if (to_rshift == 0) {
		res = (*buffer & mask);
		buffer++;
		offset = 0;
	} else {
		res = (*buffer & mask);
		n -= d;
		buffer++;
		offset = 0;
		while (n >= 8) {
			res <<= 8;
			res |= *buffer;
			n -= 8;
			buffer++;
		}
		if (n > 0) {
			offset = n;
			res <<= n;
			res |= *buffer >> (8 - n);
		}
	}
	return res;
}
