#include "FnMark.h"

#include <boost/function.hpp>
#include <iostream>
#include "KBest.h"

using namespace std;

#if !defined(_DEBUG)
static const int ARRAY_SIZE = 2048 * 2048 * 4;
#else
static const int ARRAY_SIZE = 512 * 512 * 4;
#endif

struct calc
{
    calc()
    {
        base = new float[ARRAY_SIZE];
    }

    virtual ~calc()
    {
        delete [] base;
    }

    void init()
    {
        for(int i = 0; i < ARRAY_SIZE; ++i)
        {
            base[i] = static_cast<float>(i);
        }
    }

    inline void do_with_fnptr(int i)
    {
        fn(base, i);
    }

    inline void do_with_mfnptr(int i)
    {
        (this->*mfn)(i);
    }

    inline void do_with_fnobj(int i)
    {
        fnobj(i);
    }

    inline void do_with_boost_fnobj(int i)
    {
        boost_fnobj(i);
    }

    virtual void do_with_vfn(int i) = 0;

public:
    float* base;
    void (*fn)(float* base, int i);
    void (calc::*mfn)(int i);
    std::function<void (int)> fnobj;
    boost::function<void (int)> boost_fnobj;

    static void add2(float* base, int i)
    {
        base[i] += 2;
    }

    static void mul2(float* base, int i)
    {
        base[i] *= 7.16f;
    }

    static void do_nothing(float* /*base*/, int /*i*/)
    {
    }

    void madd2(int i)
    {
        *(base+i) += 2;
    }

    void mmul2(int i)
    {
        *(base+i) *= 7.16f;
    }
};

struct calc_add2: public calc
{
    calc_add2()
    {
        fn = &calc::add2;
        mfn = &calc::madd2;
        fnobj = [this](int i) { this->base[i] += 2; };
        boost_fnobj = [this](int i) { this->base[i] += 2; };
    }

    void do_with_vfn(int i)
    {
        base[i] += 2;
    }
};

struct calc_mul2: public calc
{
    calc_mul2()
    {
        fn = &calc::mul2;
        mfn = &calc::mmul2;
        fnobj = [this](int i) { this->base[i] *= 7.16f; };
        boost_fnobj = [this](int i) { this->base[i] *= 7.16f; };
    }

    void do_with_vfn(int i)
    {
        base[i] *= 7.16f;
    }
};

void FnMarkMain(int argc)
{
    calc* obj = nullptr;
    if(argc == 1)
    {
        obj = new calc_add2();
    }
    else
    {
        obj = new calc_mul2();
    }

    {
        k_best measure(16, 0.03f, 500);
        obj->init();
        auto test_result = measure.test( [=]()
        {
            if(argc == 1)
            {
                for(int i = 0; i < ARRAY_SIZE; ++i)
                {
                    obj->base[i] += 2;
                }
            }
            else
            {
                for(int i = 0; i < ARRAY_SIZE; ++i)
                {
                    obj->base[i] *= 7.16f;
                }
            }
        });

        cout << "Branch per batch Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                if(argc == 1)
                {
                    obj->base[i] += 2;
                }
                else
                {
                    obj->base[i] *= 7.16f;
                }
            }
        });

        cout << "Branch per scalar Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                obj->do_with_fnptr(i);
            }
        });

        cout << "Fn Ptr Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                obj->do_with_vfn(i);
            }
        });

        cout << "Virtual Func Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                obj->do_with_mfnptr(i);
            }
        });

        cout << "Member Func Ptr Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                obj->do_with_fnobj(i);
            }
        });

        cout << "Function Object with Lambda Elapsed: " << test_result.second << "us" << endl;
    }

    {
        obj->init();
        k_best measure(16, 0.03f, 500);
        auto test_result = measure.test( [=]()
        {
            for(int i = 0; i < ARRAY_SIZE; ++i)
            {
                obj->do_with_boost_fnobj(i);
            }
        });

        cout << "Boost function with lambda Elapsed: " << test_result.second << "us" << endl;
    }

    delete obj;
}
