// MIT License
//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <algorithm>
#include <functional>
#include <iostream>
#include <type_traits>
#include <vector>
#include <utility>

// Google Test
#include <gtest/gtest.h>

// HIP API
#include <hip/hip_runtime.h>
#include <hip/hip_hcc.h>

// rocPRIM
#include <device/device_radix_sort.hpp>

#include "test_utils.hpp"

namespace rp = rocprim;

#define HIP_CHECK(condition)         \
{                                  \
  hipError_t error = condition;    \
  if(error != hipSuccess){         \
      std::cout << "HIP error: " << error << " line: " << __LINE__ << std::endl; \
      exit(error); \
  } \
}

template<
    class Key,
    class Value,
    bool Descending = false,
    unsigned int StartBit = 0,
    unsigned int EndBit = sizeof(Key) * 8
>
struct params
{
    using key_type = Key;
    using value_type = Value;
    static constexpr bool descending = Descending;
    static constexpr unsigned int start_bit = StartBit;
    static constexpr unsigned int end_bit = EndBit;
};

template<class Params>
class RocprimDeviceRadixSort : public ::testing::Test {
public:
    using params = Params;
};

typedef ::testing::Types<
    params<unsigned int, int>,
    params<int, int>,
    params<unsigned int, int>,
    params<unsigned short, char, true>,
    params<double, unsigned int>,
    params<float, int>,
    params<long long, char>,
    params<unsigned int, long long, true>,
    params<unsigned char, float>,
    params<float, char, true>,
    params<int, short>,
    params<unsigned short, char>,
    params<double, int>,
    params<char, double, true>,
    params<unsigned short, int>,
    params<short, int>,

    // start_bit and end_bit
    params<unsigned int, short, true, 0, 15>,
    params<unsigned long long, char, false, 8, 20>,
    params<unsigned short, int, true, 4, 10>,
    params<unsigned int, short, false, 3, 22>,
    params<unsigned char, int, true, 0, 7>,
    params<unsigned short, double, false, 8, 11>
> Params;

TYPED_TEST_CASE(RocprimDeviceRadixSort, Params);

template<class Key, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_comparator
{
    static_assert(std::is_unsigned<Key>::value, "Test supports start and bits only for unsigned integers");

    bool operator()(const Key& lhs, const Key& rhs)
    {
        auto mask = (1ull << (EndBit - StartBit)) - 1;
        auto l = (static_cast<unsigned long long>(lhs) >> StartBit) & mask;
        auto r = (static_cast<unsigned long long>(rhs) >> StartBit) & mask;
        return Descending ? (r < l) : (l < r);
    }
};

template<class Key, bool Descending>
struct key_comparator<Key, Descending, 0, sizeof(Key) * 8>
{
    bool operator()(const Key& lhs, const Key& rhs)
    {
        return Descending ? (rhs < lhs) : (lhs < rhs);
    }
};

template<class Key, class Value, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_value_comparator
{
    bool operator()(const std::pair<Key, Value>& lhs, const std::pair<Key, Value>& rhs)
    {
        return key_comparator<Key, Descending, StartBit, EndBit>()(lhs.first, rhs.first);
    }
};

std::vector<size_t> get_sizes()
{
    std::vector<size_t> sizes = { 1, 10, 53, 211, 1024, 2345, 4096, 34567, (1 << 16) - 1220, (1 << 24) - 76543 };
    const std::vector<size_t> random_sizes = get_random_data<size_t>(10, 1, 1000000);
    sizes.insert(sizes.end(), random_sizes.begin(), random_sizes.end());
    return sizes;
}

TYPED_TEST(RocprimDeviceRadixSort, SortKeys)
{
    using key_type = typename TestFixture::params::key_type;
    constexpr bool descending = TestFixture::params::descending;
    constexpr unsigned int start_bit = TestFixture::params::start_bit;
    constexpr unsigned int end_bit = TestFixture::params::end_bit;

    const std::vector<size_t> sizes = get_sizes();
    for(size_t size : sizes)
    {
        SCOPED_TRACE(testing::Message() << "with size = " << size);

        // Generate data
        std::vector<key_type> key_input;
        if(std::is_floating_point<key_type>::value)
        {
            key_input = get_random_data<key_type>(size, (key_type)-1000, (key_type)+1000);
        }
        else
        {
            key_input = get_random_data<key_type>(
                size,
                std::numeric_limits<key_type>::min(),
                std::numeric_limits<key_type>::max()
            );
        }

        key_type * d_key_input;
        key_type * d_key_output;
        HIP_CHECK(hipMalloc(&d_key_input, size * sizeof(key_type)));
        HIP_CHECK(hipMalloc(&d_key_output, size * sizeof(key_type)));
        HIP_CHECK(
            hipMemcpy(
                d_key_input, key_input.data(),
                size * sizeof(key_type),
                hipMemcpyHostToDevice
            )
        );

        // Calculate expected results on host
        std::vector<key_type> expected(key_input);
        std::stable_sort(expected.begin(), expected.end(), key_comparator<key_type, descending, start_bit, end_bit>());

        void * d_temporary_storage = nullptr;
        size_t temporary_storage_bytes;
        rp::device_radix_sort_keys(
            d_temporary_storage, temporary_storage_bytes,
            d_key_input, d_key_output, size,
            start_bit, end_bit
        );
        HIP_CHECK(hipMalloc(&d_temporary_storage, temporary_storage_bytes));

        if(descending)
        {
            rp::device_radix_sort_keys_desc(
                d_temporary_storage, temporary_storage_bytes,
                d_key_input, d_key_output, size,
                start_bit, end_bit
            );
        }
        else
        {
            rp::device_radix_sort_keys(
                d_temporary_storage, temporary_storage_bytes,
                d_key_input, d_key_output, size,
                start_bit, end_bit
            );
        }
        HIP_CHECK(hipDeviceSynchronize());

        HIP_CHECK(hipFree(d_temporary_storage));
        HIP_CHECK(hipFree(d_key_input));

        std::vector<key_type> key_output(size);
        HIP_CHECK(
            hipMemcpy(
                key_output.data(), d_key_output,
                size * sizeof(key_type),
                hipMemcpyDeviceToHost
            )
        );

        HIP_CHECK(hipFree(d_key_output));

        for(size_t i = 0; i < size; i++)
        {
            ASSERT_EQ(key_output[i], expected[i]);
        }
    }
}
