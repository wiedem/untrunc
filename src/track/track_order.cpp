#include "track_order.h"

#include <iostream>

#include "util/common.h"

bool findOrder(std::vector<std::pair<int, int>> &data, bool ignore_first_failed) {
	int order_sz = -1;
	for (uint i = 1; i < data.size(); i++) {
		if (data[i] == data[0]) {
			order_sz = i;
			break;
		}
	}
	if (order_sz < 0) {
		data.clear();
		return false;
	}

	uint first_failed = 0;
	for (uint i = 1; i < data.size(); i++) {
		if (data[i] != data[i % order_sz]) {
			first_failed = i;
			break;
		}
	}

	if (first_failed == data.size() - order_sz && order_sz <= 4) // last values might be shorter
		first_failed = 0;

	int orig_sz = data.size();
	data.resize(order_sz);
	if (g_options.log_mode >= V) {
		std::cout << "first_failed: " << first_failed << " of " << orig_sz << '\n';
		std::cout << "order: ";
		for (auto &p : data)
			std::cout << ss("(", p.first, ", ", p.second, ") ");
		std::cout << '\n';
	}

	if (first_failed && !ignore_first_failed) data.clear();
	return !first_failed;
}

// like findOrder, but only looks at first entry
std::vector<int> findOrderSimple(const std::vector<std::pair<int, int>> &data) {
	std::vector<int> result;

	for (uint i = 0; i < data.size(); i++) {
		int val_i = data[i].first;
		if (i && val_i == data[0].first) break;
		result.emplace_back(val_i);
	}
	if (result.empty()) return result;

	uint first_failed = 0;
	for (uint i = 1; i < data.size(); i++) {
		if (data[i].first != result[i % result.size()]) {
			first_failed = i;
			break;
		}
	}

	if (g_options.log_mode >= V) {
		std::cout << "first_failed: " << first_failed << " of " << data.size() << '\n';
		std::cout << "simpleOrder: ";
		for (auto &x : result)
			std::cout << x << " ";
		std::cout << '\n';
	}

	if (first_failed) result.clear();
	return result;
}
