// SPDX-FileCopyrightText: 2026 Carlo Wood
// SPDX-License-Identifier: MIT

#include "sys.h"
#include "memory/VectorAllocator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// Record independent test failures so that one run reports every defect that can be
// exercised without invoking undefined behavior.
class TestContext
{
 public:
  // Record whether condition holds and identify a failed expectation with description.
  //
  // Returns condition so a caller can guard operations that require the expectation.
  bool expect(bool condition, std::string_view description)
  {
    if (!condition)
    {
      ++failures_;
      std::cerr << "FAIL: " << description << '\n';
    }
    return condition;
  }

  // Return the number of expectations that failed during this test run.
  int failures() const { return failures_; }

 private:
  int failures_ = 0;
};

template <std::size_t Size>
struct Object
{
  std::array<std::byte, Size> bytes;
};

struct alignas(16) AlignedObject
{
  std::array<std::byte, 16> bytes;
};

// Allocate pools for the lifetime of the process because each allocator specialization
// permanently stores the first pool address in static size-class resources.
//
// The deliberately leaked pool satisfies VectorAllocator's documented lifetime rule.
memory::MemoryPagePool& permanent_pool(std::size_t block_size)
{
  return *new memory::MemoryPagePool(block_size);
}

// Check every supported request count for a type and report public optimal capacities
// that cannot hold the requested objects. This avoids accessing private size-class
// arithmetic or corrupting memory when the allocator underestimates a request.
template <typename T>
void test_capacity_rounding(TestContext& test, std::string_view type_name)
{
  using Allocator = memory::VectorAllocator<T>;
  for (std::size_t n = 1; n <= Allocator::maximum_number_of_elements; ++n)
  {
    if (!test.expect(Allocator::optimal_capacity(n) >= n, type_name))
      break;
  }
}

// Verify zero-size handling, ordinary pooled allocations, copies, rebinding, equality,
// the direct page class, the std::allocator fallback, and use by std::vector.
void test_normal_operations(TestContext& test)
{
  using Allocator = memory::VectorAllocator<int>;
  using ReboundAllocator = std::allocator_traits<Allocator>::rebind_alloc<long>;
  static_assert(std::same_as<ReboundAllocator, memory::VectorAllocator<long>>);

  memory::MemoryPagePool& pool = permanent_pool(memory::MemoryPagePool::default_block_size);
  Allocator allocator(pool);
  Allocator copy(allocator);
  ReboundAllocator rebound(allocator);

  test.expect(allocator == copy, "copies of a VectorAllocator compare equal");
  test.expect(allocator == rebound, "a rebound allocator retains the same pool");
  test.expect(allocator.allocate(0) == nullptr, "allocate(0) returns nullptr");
  allocator.deallocate(nullptr, 0);

  long* const rebound_ptr = rebound.allocate(3);
  for (long i = 0; i != 3; ++i)
    rebound_ptr[i] = i;
  for (long i = 0; i != 3; ++i)
    test.expect(rebound_ptr[i] == i, "a rebound allocator retains written values");
  rebound.deallocate(rebound_ptr, 3);

  constexpr std::array<std::size_t, 9> pooled_counts{
      1, 2, 3, 4, 5, 64, 1024, Allocator::largest_allocation / sizeof(int),
      Allocator::largest_allocation / sizeof(int) + 1};
  for (std::size_t n : pooled_counts)
  {
    int* const ptr = allocator.allocate(n);
    test.expect(ptr != nullptr, "a supported pooled allocation succeeds");
    test.expect(reinterpret_cast<std::uintptr_t>(ptr) % alignof(int) == 0,
        "a pooled allocation is aligned for its value type");
    for (std::size_t i = 0; i < n; ++i)
      ptr[i] = static_cast<int>(i);
    for (std::size_t i = 0; i < n; ++i)
      test.expect(ptr[i] == static_cast<int>(i), "a pooled allocation retains written values");
    allocator.deallocate(ptr, n);
  }

  int* const whole_page = allocator.allocate(Allocator::maximum_number_of_elements);
  test.expect(whole_page != nullptr, "the direct-page allocation class succeeds");
  allocator.deallocate(whole_page, Allocator::maximum_number_of_elements);

  auto const blocks_before_fallback = pool.pool_blocks();
  std::size_t const fallback_count = Allocator::maximum_number_of_elements + 1;
  int* const fallback = allocator.allocate(fallback_count);
  test.expect(fallback != nullptr, "an oversized request falls back to std::allocator");
  test.expect(pool.pool_blocks() == blocks_before_fallback,
      "the std::allocator fallback does not consume a pool block");
  allocator.deallocate(fallback, fallback_count);

  std::vector<int, Allocator> values(allocator);
  for (int i = 0; i != 10000; ++i)
    values.push_back(i);
  for (int i = 0; i != 10000; ++i)
    test.expect(values[static_cast<std::size_t>(i)] == i, "std::vector retains its elements");
}

// Verify the documented split ownership when two unequal allocators of one specialization
// use different pools. Geometric classes stay attached to the first pool, while requests
// above the largest geometric class use the pool stored in the individual allocator.
void test_shared_resources_and_allocator_identity(TestContext& test)
{
  using Allocator = memory::VectorAllocator<Object<8>, 64, 8192>;
  memory::MemoryPagePool& first_pool = permanent_pool(8192);
  memory::MemoryPagePool& second_pool = permanent_pool(8192);
  Allocator first(first_pool);
  Allocator second(second_pool);

  test.expect(!(first == second), "allocators with different direct-page pools compare unequal");

  Object<8>* const small = second.allocate(1);
  test.expect(first_pool.pool_blocks() != 0,
      "shared geometric resources remain attached to the first pool");
  test.expect(second_pool.pool_blocks() == 0,
      "a geometric allocation does not use a later allocator's direct-page pool");
  second.deallocate(small, 1);

  constexpr std::size_t direct_count = Allocator::largest_allocation / sizeof(Object<8>) + 1;
  Object<8>* const direct = second.allocate(direct_count);
  test.expect(second_pool.pool_blocks() != 0,
      "a direct-page allocation uses the individual allocator's pool");
  second.deallocate(direct, direct_count);
}

// Exercise allocate_at_least at ordinary boundaries and require the returned count to
// satisfy its standard contract. Returned storage is always deallocated with that count.
void test_allocate_at_least(TestContext& test)
{
  using Allocator = memory::VectorAllocator<int>;
  memory::MemoryPagePool& pool = permanent_pool(memory::MemoryPagePool::default_block_size);
  Allocator allocator(pool);

  constexpr std::array<std::size_t, 8> request_counts{0, 1, 2, 3, 5, 1025,
      Allocator::maximum_number_of_elements, Allocator::maximum_number_of_elements + 1};
  for (std::size_t n : request_counts)
  {
    auto result = allocator.allocate_at_least(n);
    test.expect(result.count >= n, "allocate_at_least returns a count no smaller than requested");
    allocator.deallocate(result.ptr, result.count);
  }
}

// Check a valid but non-power-of-two page-pool size. The direct-page capacity must be
// bounded by the physical page, and allocate_at_least must keep supported requests on
// the configured pool rather than rounding its internal request into the fallback path.
void test_non_geometric_page_size(TestContext& test)
{
  using Allocator = memory::VectorAllocator<int, 8, 12288>;
  constexpr std::size_t request = Allocator::maximum_number_of_elements;
  test.expect(Allocator::optimal_capacity(request) == Allocator::maximum_number_of_elements,
      "optimal_capacity does not exceed the physical direct-page capacity");

  memory::MemoryPagePool& pool = permanent_pool(12288);
  Allocator allocator(pool);
  auto result = allocator.allocate_at_least(request);
  test.expect(result.count >= request,
      "allocate_at_least covers the requested count with a non-geometric page size");
  test.expect(pool.pool_blocks() != 0,
      "allocate_at_least keeps a page-sized request on its MemoryPagePool");
  allocator.deallocate(result.ptr, result.count);
}

// Allocate several over-aligned objects using compatible allocator parameters. Every
// pointer returned by an allocator must satisfy alignof(value_type), including later
// partitions in a pool block.
void test_overaligned_value_type(TestContext& test)
{
  using Allocator = memory::VectorAllocator<AlignedObject, 32, 12288, 16>;
  static_assert(Allocator::element_size == sizeof(AlignedObject));
  memory::MemoryPagePool& pool = permanent_pool(12288);
  Allocator allocator(pool);
  std::array<AlignedObject*, 4> pointers{};

  for (AlignedObject*& ptr : pointers)
  {
    ptr = allocator.allocate(1);
    test.expect(reinterpret_cast<std::uintptr_t>(ptr) % alignof(AlignedObject) == 0,
        "every allocation is aligned for an over-aligned value type");
  }
  for (AlignedObject* ptr : pointers)
    allocator.deallocate(ptr, 1);
}

// Verify that rejecting a pool with the wrong block size has no persistent side effect.
// A subsequent successful construction of the same specialization must bind its static
// resources to the valid pool rather than the pool from the failed constructor.
void test_failed_constructor_does_not_bind_pool(TestContext& test)
{
  using Allocator = memory::VectorAllocator<Object<7>, 64, 4096>;
  memory::MemoryPagePool& wrong_pool = permanent_pool(8192);
  bool threw = false;
  try
  {
    Allocator invalid(wrong_pool);
  }
  catch (std::invalid_argument const&)
  {
    threw = true;
  }
  test.expect(threw, "a mismatched MemoryPagePool block size is rejected");

  memory::MemoryPagePool& valid_pool = permanent_pool(4096);
  Allocator allocator(valid_pool);
  Object<7>* const ptr = allocator.allocate(1);
  test.expect(valid_pool.pool_blocks() != 0,
      "a failed constructor does not bind shared resources to its invalid pool");
  test.expect(wrong_pool.pool_blocks() == 0,
      "the rejected pool is never used by a later valid allocator");
  allocator.deallocate(ptr, 1);
}

} // namespace

// Run all allocator checks and return failure when any contract or safety property is
// violated. Multiple failures are reported in one invocation whenever doing so is safe.
int main()
{
  Debug(NAMESPACE_DEBUG::init());
  TestContext test;

  test_capacity_rounding<Object<1>>(test, "one-byte elements are never under-allocated");
  test_capacity_rounding<Object<3>>(test, "three-byte elements are never under-allocated");
  test_capacity_rounding<int>(test, "int elements are never under-allocated");
  test_capacity_rounding<Object<12>>(test, "twelve-byte elements are never under-allocated");
  test_normal_operations(test);
  test_shared_resources_and_allocator_identity(test);
  test_allocate_at_least(test);
  test_non_geometric_page_size(test);
  test_overaligned_value_type(test);
  test_failed_constructor_does_not_bind_pool(test);

  if (test.failures() != 0)
    std::cerr << test.failures() << " VectorAllocator test expectation(s) failed.\n";
  return test.failures() == 0 ? 0 : 1;
}
