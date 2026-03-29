#include "header_atom.h"

#include "atom.h"
#include "util/common.h"

void HasHeaderAtom::editHeaderAtom(Atom *header_atom, int64_t duration, bool is_tkhd) {
	auto &data = header_atom->content_;
	uint version = data[0]; // version=1 => 64 bit (2x date + 1x duration)

	int bonus = is_tkhd ? 4 : 0;

	if (version == 0 && duration > (1LL << 32)) {
		logg(V, "converting to 64bit version of '", header_atom->name_, "'\n");
		data[0] = 1;
		data.insert(data.begin() + 16 + bonus, 4, 0x00);
		data.insert(data.begin() + 8, 4, 0x00);
		data.insert(data.begin() + 4, 4, 0x00);
	}

	if (data[0] == 1)
		header_atom->writeInt64(duration, 24 + bonus);
	else
		header_atom->writeInt(duration, 16 + bonus);
}

void HasHeaderAtom::editHeaderAtom() {
	editHeaderAtom(header_atom_, duration_);
}

void HasHeaderAtom::readHeaderAtom() {
	auto &data = header_atom_->content_;
	uint version = data[0]; // version=1 => 64 bit (2x date + 1x duration)

	if (version == 1) {
		timescale_ = header_atom_->readInt(20);
		duration_ = header_atom_->readInt64(24);
	} else {
		timescale_ = header_atom_->readInt(12);
		duration_ = header_atom_->readInt(16);
	}
}

int HasHeaderAtom::getDurationInMs() {
	return 1000 * duration_ / timescale_;
}
