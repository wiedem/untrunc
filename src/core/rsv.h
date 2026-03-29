#pragma once

#include "util/common.h"
#include "io/file.h"

bool isPointingAtRtmdHeader(FileRead &file);
bool isRtmdHeader(const uchar *buff);