// Standalone MP4 file manipulation utilities.
// These operate only on file I/O and atoms, with no Mp4 instance state.

#pragma once
#include <string>

namespace Mp4Tools {

void listm(const std::string &filename);
void unite(const std::string &mdat_fn, const std::string &moov_fn);
void shorten(const std::string &filename, int mega_bytes, bool force);

} // namespace Mp4Tools
