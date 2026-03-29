#pragma once
#include "common.h"

int readGolomb(const uchar *&buffer, int &offset);
uint readBits(int n, const uchar *&buffer, int &offset);
