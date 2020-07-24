// MIT License
//
// Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All rights reserved.
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

#include "common_test_header.hpp"

// required rocprim headers
#include <rocprim/iterator/constant_iterator.hpp>
#include <rocprim/device/device_transform.hpp>

// required test headers
#include "test_utils_types.hpp"

// Params for tests
template<class InputType>
struct RocprimConstantIteratorParams
{
    using input_type = InputType;
};

template<class Params>
class RocprimConstantIteratorTests : public ::testing::Test
{
public:
    using input_type = typename Params::input_type;
    const bool debug_synchronous = false;
};

typedef ::testing::Types<
    RocprimConstantIteratorParams<int>,
    RocprimConstantIteratorParams<unsigned int>,
    RocprimConstantIteratorParams<unsigned long>,
    RocprimConstantIteratorParams<float>
> RocprimConstantIteratorTestsParams;

TYPED_TEST_CASE(RocprimConstantIteratorTests, RocprimConstantIteratorTestsParams);

template<class T>
struct transform
{
    __device__ __host__
    constexpr T operator()(const T& a) const
    {
        return 5 + a;
    }
};

TYPED_TEST(RocprimConstantIteratorTests, Transform)
{
    int device_id = test_common_utils::obtain_device_from_ctest();
    SCOPED_TRACE(testing::Message() << "with device_id= " << device_id);
    HIP_CHECK(hipSetDevice(device_id));
    
    using T = typename TestFixture::input_type;
    using Iterator = typename rocprim::constant_iterator<T>;
    const bool debug_synchronous = TestFixture::debug_synchronous;

    const size_t size = 1024;

    hipStream_t stream = 0; // default

    for (size_t seed_index = 0; seed_index < random_seeds_count + seed_size; seed_index++)
    {
        unsigned int seed_value = seed_index < random_seeds_count  ? rand() : seeds[seed_index - random_seeds_count];
        SCOPED_TRACE(testing::Message() << "with seed= " << seed_value);

        // Create constant_iterator<U> with random starting point
        const auto value = test_utils::get_random_value<T>(0, 200, seed_value);
        Iterator input_begin(value);

        std::vector<T> output(size);
        T * d_output;
        HIP_CHECK(hipMalloc(&d_output, output.size() * sizeof(T)));
        HIP_CHECK(hipDeviceSynchronize());

        // Calculate expected results on host
        std::vector<T> expected(size, transform<T>()(value));

        // Run
        HIP_CHECK(
            rocprim::transform(
                input_begin, d_output, size,
                transform<T>(), stream, debug_synchronous
            )
        );
        HIP_CHECK(hipPeekAtLastError());
        HIP_CHECK(hipDeviceSynchronize());

        // Copy output to host
        HIP_CHECK(
            hipMemcpy(
                output.data(), d_output,
                output.size() * sizeof(T),
                hipMemcpyDeviceToHost
            )
        );
        HIP_CHECK(hipDeviceSynchronize());

        // Validating results
        for(size_t i = 0; i < output.size(); i++)
        {
            if(std::is_integral<T>::value)
            {
                ASSERT_EQ(output[i], expected[i]) << "where index = " << i;
            }
            else if(std::is_floating_point<T>::value)
            {
                auto tolerance = std::max<T>(std::abs(0.1f * expected[i]), T(test_utils::precision_threshold<T>::percentage));
                ASSERT_NEAR(output[i], expected[i], tolerance) << "where index = " << i;
            }
        }

        hipFree(d_output);
    }

}
