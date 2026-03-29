#pragma once
#include <utility>
#include <vector>

bool findOrder(std::vector<std::pair<int, int>> &data, bool ignore_first_failed = false);
std::vector<int> findOrderSimple(const std::vector<std::pair<int, int>> &data);
