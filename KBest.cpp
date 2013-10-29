#include "KBest.h"

#include <limits>
#include <chrono>
#include <algorithm>
#include <iostream>

#if defined(WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

using namespace std;

k_best::k_best(int k, float epsilon, int max_test_count)
{
    min_time_ = std::numeric_limits<int64_t>::max();
    if(k == 0)
    {
        k_ = 1;
    }
    else
    {
        k_ = k;
    }

    if(max_test_count < k_)
    {
        max_tests_ = k_;
    }
    else
    {
        max_tests_ = max_test_count;
    }

    if(epsilon < 0.0f)
    {
        eps_ = 0.05f;
    }
    else
    {
        eps_ = epsilon;
    }

#if defined(WIN32)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    perf_freq_ = freq.QuadPart;
#endif
}

std::pair<bool, int64_t> k_best::test(function<void ()> const& fn)
{
        for(int i = 0; i < max_tests_; ++i)
        {
#if defined(WIN32)
            LARGE_INTEGER beg_time, end_time;
            QueryPerformanceCounter(&beg_time);
            fn();
            QueryPerformanceCounter(&end_time);
            int64_t elapsedUS = static_cast<int64_t>( double(end_time.QuadPart - beg_time.QuadPart) / double(perf_freq_) * 1000000 );
#else
            auto startTime = chrono::high_resolution_clock::now();
            fn();
            auto endTime = chrono::high_resolution_clock::now();
            int64_t elapsedUS = chrono::duration_cast<chrono::microseconds>(endTime-startTime).count();
#endif
            min_time_ = std::min(min_time_, elapsedUS);
            heap_.push_back(elapsedUS);
            push_heap(heap_.begin(), heap_.end());
            
            if(heap_.size() > static_cast<size_t>(k_))
            {
                pop_heap(heap_.begin(), heap_.end());
                heap_.pop_back();
            }

            if(heap_.size() == static_cast<size_t>(k_))
            {
                if( static_cast<float>(heap_.front()) < static_cast<float>(min_time_ * (1.0f + eps_)) )
                {
                    cout << "Convergence: " << i+1 << " = " ;
                    return make_pair( true, heap_.front() );
                }
            }
        }

        cout << "Convergence: No = " ;
        return make_pair(false, heap_.front());
    }