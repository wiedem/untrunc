#pragma once
#include <cstdint>

class Atom;

// reads and writes mvhd and mdhd atoms
class HasHeaderAtom {
  public:
	int timescale_;
	int64_t duration_;

	static void editHeaderAtom(Atom *header_atom, int64_t duration, bool is_tkhd = false);
	void editHeaderAtom();
	void readHeaderAtom();
	int getDurationInMs();

  protected:
	Atom *header_atom_;
};
