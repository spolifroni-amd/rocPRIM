// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_DEVICE_DEVICE_REDUCE_BY_KEY_HC_HPP_
#define ROCPRIM_DEVICE_DEVICE_REDUCE_BY_KEY_HC_HPP_

#include <iterator>
#include <iostream>

#include "../config.hpp"
#include "../detail/various.hpp"
#include "../detail/match_result_type.hpp"

#include "../functional.hpp"

#include "detail/device_reduce_by_key.hpp"

BEGIN_ROCPRIM_NAMESPACE

/// \addtogroup devicemodule_hc
/// @{

namespace detail
{

#define ROCPRIM_DETAIL_HC_SYNC(name, size, start) \
    { \
        if(debug_synchronous) \
        { \
            std::cout << name << "(" << size << ")"; \
            acc_view.wait(); \
            auto end = std::chrono::high_resolution_clock::now(); \
            auto d = std::chrono::duration_cast<std::chrono::duration<double>>(end - start); \
            std::cout << " " << d.count() * 1000 << " ms" << '\n'; \
        } \
    }

template<
    class KeysInputIterator,
    class ValuesInputIterator,
    class UniqueOutputIterator,
    class AggregatesOutputIterator,
    class UniqueCountOutputIterator,
    class BinaryFunction,
    class KeyCompareFunction
>
inline
void reduce_by_key_impl(void * temporary_storage,
                        size_t& storage_size,
                        KeysInputIterator keys_input,
                        ValuesInputIterator values_input,
                        const unsigned int size,
                        UniqueOutputIterator unique_output,
                        AggregatesOutputIterator aggregates_output,
                        UniqueCountOutputIterator unique_count_output,
                        BinaryFunction reduce_op,
                        KeyCompareFunction key_compare_op,
                        hc::accelerator_view acc_view,
                        const bool debug_synchronous)
{
    using key_type = typename std::iterator_traits<KeysInputIterator>::value_type;
    using result_type = typename ::rocprim::detail::match_result_type<
        typename std::iterator_traits<ValuesInputIterator>::value_type,
        typename std::iterator_traits<AggregatesOutputIterator>::value_type,
        BinaryFunction
    >::type;
    using carry_out_type = carry_out<key_type, result_type>;

    constexpr unsigned int block_size = 256;
    constexpr unsigned int items_per_thread = 7;

    constexpr unsigned int scan_block_size = 256;
    constexpr unsigned int scan_items_per_thread = 7;

    constexpr unsigned int items_per_block = block_size * items_per_thread;
    constexpr unsigned int scan_items_per_block = scan_block_size * scan_items_per_thread;

    const unsigned int blocks = ::rocprim::detail::ceiling_div(static_cast<unsigned int>(size), items_per_block);
    const unsigned int blocks_per_full_batch = ::rocprim::detail::ceiling_div(blocks, scan_items_per_block);
    const unsigned int full_batches = blocks % scan_items_per_block != 0
        ? blocks % scan_items_per_block
        : scan_items_per_block;
    const unsigned int batches = (blocks_per_full_batch == 1 ? full_batches : scan_items_per_block);

    const size_t unique_counts_bytes = ::rocprim::detail::align_size(batches * sizeof(unsigned int));
    const size_t carry_outs_bytes = ::rocprim::detail::align_size(batches * sizeof(carry_out_type));
    const size_t leading_aggregates_bytes = ::rocprim::detail::align_size(batches * sizeof(result_type));
    if(temporary_storage == nullptr)
    {
        storage_size = unique_counts_bytes + carry_outs_bytes + leading_aggregates_bytes;
        return;
    }

    if(debug_synchronous)
    {
        std::cout << "block_size " << block_size << '\n';
        std::cout << "items_per_thread " << items_per_thread << '\n';
        std::cout << "blocks " << blocks << '\n';
        std::cout << "blocks_per_full_batch " << blocks_per_full_batch << '\n';
        std::cout << "full_batches " << full_batches << '\n';
        std::cout << "batches " << batches << '\n';
        std::cout << "storage_size " << storage_size << '\n';
        acc_view.wait();
    }

    char * ptr = reinterpret_cast<char *>(temporary_storage);
    unsigned int * unique_counts = reinterpret_cast<unsigned int *>(ptr);
    ptr += unique_counts_bytes;
    carry_out_type * carry_outs = reinterpret_cast<carry_out_type *>(ptr);
    ptr += carry_outs_bytes;
    result_type * leading_aggregates = reinterpret_cast<result_type *>(ptr);

    // Start point for time measurements
    std::chrono::high_resolution_clock::time_point start;

    if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
    hc::parallel_for_each(
        acc_view,
        hc::tiled_extent<1>(batches * block_size, block_size),
        [=](hc::tiled_index<1>) [[hc]]
        {
            fill_unique_counts<block_size, items_per_thread>(
                keys_input, size, unique_counts, key_compare_op,
                blocks_per_full_batch, full_batches, blocks
            );
        }
    );
    ROCPRIM_DETAIL_HC_SYNC("fill_unique_counts", size, start)

    if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
    hc::parallel_for_each(
        acc_view,
        hc::tiled_extent<1>(scan_block_size, scan_block_size),
        [=](hc::tiled_index<1>) [[hc]]
        {
            scan_unique_counts<scan_block_size, scan_items_per_thread>(
                unique_counts, unique_count_output,
                batches
            );
        }
    );
    ROCPRIM_DETAIL_HC_SYNC("scan_unique_counts", scan_block_size, start)

    if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
    hc::parallel_for_each(
        acc_view,
        hc::tiled_extent<1>(batches * block_size, block_size),
        [=](hc::tiled_index<1>) [[hc]]
        {
            reduce_by_key<block_size, items_per_thread>(
                keys_input, values_input, size,
                unique_counts, carry_outs, leading_aggregates,
                unique_output, aggregates_output,
                key_compare_op, reduce_op,
                blocks_per_full_batch, full_batches, blocks
            );
        }
    );
    ROCPRIM_DETAIL_HC_SYNC("reduce_by_key", size, start)

    if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
    hc::parallel_for_each(
        acc_view,
        hc::tiled_extent<1>(scan_block_size, scan_block_size),
        [=](hc::tiled_index<1>) [[hc]]
        {
            scan_and_scatter_carry_outs<scan_block_size, scan_items_per_thread>(
                carry_outs, leading_aggregates,
                aggregates_output,
                key_compare_op, reduce_op,
                batches
            );
        }
    );
    ROCPRIM_DETAIL_HC_SYNC("scan_and_scatter_carry_outs", scan_block_size, start)
}

#undef ROCPRIM_DETAIL_HC_SYNC

} // end of detail namespace

/// \brief HC parallel reduce-by-key primitive for device level.
///
/// reduce_by_key function performs a device-wide reduction operation of groups
/// of consecutive values having the same key using binary \p reduce_op operator. The first key of each group
/// is copied to \p unique_output and reduction of the group is written to \p aggregates_output.
/// The total number of group is written to \p unique_count_output.
///
/// \par Overview
/// * Supports non-commutative reduction operators. However, a reduction operator should be
/// associative. When used with non-associative functions the results may be non-deterministic
/// and/or vary in precision.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * Ranges specified by \p keys_input and \p values_input must have at least \p size elements.
/// * Range specified by \p unique_count_output must have at least 1 element.
/// * Ranges specified by \p unique_output and \p aggregates_output must have at least
/// <tt>*unique_count_output</tt> (i.e. the number of unique keys) elements.
///
/// \tparam KeysInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam ValuesInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam UniqueOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam AggregatesOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam UniqueCountOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam BinaryFunction - type of binary function used for reduction. Default type
/// is \p rocprim::plus<T>, where \p T is a \p value_type of \p ValuesInputIterator.
/// \tparam KeyCompareFunction - type of binary function used to determine keys equality. Default type
/// is \p rocprim::equal_to<T>, where \p T is a \p value_type of \p KeysInputIterator.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] keys_input - iterator to the first element in the range of keys.
/// \param [in] values_input - iterator to the first element in the range of values to reduce.
/// \param [in] size - number of element in the input range.
/// \param [out] unique_output - iterator to the first element in the output range of unique keys.
/// \param [out] aggregates_output - iterator to the first element in the output range of reductions.
/// \param [out] unique_count_output - iterator to total number of groups.
/// \param [in] reduce_op - binary operation function object that will be used for reduction.
/// The signature of the function should be equivalent to the following:
/// <tt>T f(const T &a, const T &b);</tt>. The signature does not need to have
/// <tt>const &</tt>, but function object must not modify the objects passed to it.
/// Default is BinaryFunction().
/// \param [in] key_compare_op - binary operation function object that will be used to determine keys equality.
/// The signature of the function should be equivalent to the following:
/// <tt>bool f(const T &a, const T &b);</tt>. The signature does not need to have
/// <tt>const &</tt>, but function object must not modify the objects passed to it.
/// Default is KeyCompareFunction().
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level sum operation is performed on an array of
/// integer values and integer keys.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// size_t input_size;                       // e.g., 8
/// hc::array<int> keys_input(...);          // e.g., [1, 1, 1, 2, 10, 10, 10, 88]
/// hc::array<int> values_input(...);        // e.g., [1, 2, 3, 4,  5,  6,  7,  8]
/// hc::array<int> unique_output(...);       // empty array of at least 4 elements
/// hc::array<int> aggregates_output(...);   // empty array of at least 4 elements
/// hc::array<int> unique_count_output(...); // empty array of 1 element
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::reduce_by_key(
///     nullptr, temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), values_input.accelerator_pointer(), input_size,
///     unique_output.accelerator_pointer(), aggregates_output.accelerator_pointer(),
///     unique_count_output.accelerator_pointer()
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform reduction
/// rocprim::reduce_by_key(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), values_input.accelerator_pointer(), input_size,
///     unique_output.accelerator_pointer(), aggregates_output.accelerator_pointer(),
///     unique_count_output.accelerator_pointer()
/// );
/// // unique_output:       [1, 2, 10, 88]
/// // aggregates_output:   [6, 4, 18,  8]
/// // unique_count_output: [4]
/// \endcode
/// \endparblock
template<
    class KeysInputIterator,
    class ValuesInputIterator,
    class UniqueOutputIterator,
    class AggregatesOutputIterator,
    class UniqueCountOutputIterator,
    class BinaryFunction = ::rocprim::plus<typename std::iterator_traits<ValuesInputIterator>::value_type>,
    class KeyCompareFunction = ::rocprim::equal_to<typename std::iterator_traits<KeysInputIterator>::value_type>
>
inline
void reduce_by_key(void * temporary_storage,
                   size_t& storage_size,
                   KeysInputIterator keys_input,
                   ValuesInputIterator values_input,
                   unsigned int size,
                   UniqueOutputIterator unique_output,
                   AggregatesOutputIterator aggregates_output,
                   UniqueCountOutputIterator unique_count_output,
                   BinaryFunction reduce_op = BinaryFunction(),
                   KeyCompareFunction key_compare_op = KeyCompareFunction(),
                   hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                   bool debug_synchronous = false)
{
    detail::reduce_by_key_impl(
        temporary_storage, storage_size,
        keys_input, values_input, size,
        unique_output, aggregates_output, unique_count_output,
        reduce_op, key_compare_op,
        acc_view, debug_synchronous
    );
}

/// @}
// end of group devicemodule_hc

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_DEVICE_DEVICE_REDUCE_BY_KEY_HC_HPP_
