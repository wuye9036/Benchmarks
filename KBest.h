#pragma once

#include <utility>
#include <cstdint>
#include <vector>
#include <functional>

struct k_best
{
public:
    k_best(int k, float epsilon, int max_test_count);
    std::pair<bool, int64_t> test(std::function<void ()> const& fn);

private:
    int                    k_;
    float                  eps_;
    int                    max_tests_;
    std::vector<int64_t>   heap_;
    int64_t                min_time_;
    int64_t                perf_freq_;
};