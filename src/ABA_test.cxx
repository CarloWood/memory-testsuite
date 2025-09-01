#include "sys.h"
#include "utils/threading/Gate.h"
#include <atomic>
#include <thread>
#include "debug.h"

// Define this to 0 to test MemoryPagePool, and to 1 to test MemoryMappedPool.
#define TEST_MAPPED 0

#if TEST_MAPPED
#include "memory/MemoryMappedPool.h"
#else
#include "memory/MemoryPagePool.h"
#endif

std::atomic<bool> first_time = true;
utils::threading::Gate next_has_been_read;
utils::threading::Gate ABA_happened;

#if TEST_MAPPED
class MappedSegregatedStorage : public memory::MappedSegregatedStorage
{
 public:
  // This is a copy of memory::MappedSegregatedStorage::allocate, except for the marked sections.
  void* allocate(void* mapped_base, size_t mapped_size, size_t block_size)
  {
    /*
     * Inserted debug code: use memory::PtrTag.
     */
    using memory::PtrTag;
    /*
     * End of inserted debug code.
     */

    // Load the current value of m_head_tag into `head_tag`.
    // Use std::memory_order_acquire to synchronize with the std::memory_order_release in deallocate,
    // so that value of `next` read below will be the value written in deallocate corresponding to
    // this head value.
    PtrTag head_tag(this->m_head_tag.load(std::memory_order_acquire));
    while (head_tag != PtrTag::end_of_list)
    {
      PtrTag new_head_tag = head_tag.next();

      /*
       * Inserted debug code: make the first thread that comes here halt until `ABA_happened`.
       */
      if (first_time)
      {
        first_time = false;
        next_has_been_read.open();
        ABA_happened.wait();
      }
      /*
       * End of inserted debug code.
       */

      // If the next pointer is NULL then this could be a block that wasn't allocated before.
      // In that case the real next block is just the next block in the file.
      if (AI_UNLIKELY(new_head_tag.ptr() == nullptr))
      {
        char* front_node = reinterpret_cast<char*>(head_tag.ptr());
        char* second_node = front_node + block_size;
        new_head_tag = PtrTag::encode(second_node, head_tag.tag() + 1);
        if (AI_UNLIKELY(second_node == static_cast<char*>(mapped_base) + mapped_size))
          new_head_tag = PtrTag::end_of_list;
      }
      // The std::memory_order_acquire is used in case of failure and required for the next
      // read of m_next at the top of the current loop (the previous line).
      if (AI_LIKELY(this->CAS_head_tag(head_tag, new_head_tag, std::memory_order_acquire)))
        // Return the old head.
        return head_tag.ptr();
      // m_head_tag was changed (the new value is now in `head_tag`). Try again with the new value.
    }
    // Reached the end of the list.
    return nullptr;
  }
};

class MemoryPool : public memory::MemoryMappedPool
{
 private:
  // Replace mss_ with our own class (defined above).
  MappedSegregatedStorage mss_;

 public:
  // And use it.
  // These are exact copies of memory::MemoryMappedPool::allocate and memory::MemoryMappedPool::deallocate.
  void* allocate() override { return mss_.allocate(mapped_base_, mapped_size_, m_block_size); }
  void deallocate(void* ptr) override { mss_.deallocate(ptr); }

  // Inherit all constructors.
  MemoryPool(std::filesystem::path const& filename, size_t block_size,
      size_t file_size = 0, Mode mode = Mode::persistent, bool zero_init = false) :
    memory::MemoryMappedPool(filename, block_size, file_size, mode, zero_init)
  {
    // Set m_head to point to the start of mapped memory.
    mss_.initialize(mapped_base_);
  }
};

constexpr size_t partition_size = 4096;         // Must be a multiple of the memory page.
#else
class SimpleSegregatedStorage : public memory::SimpleSegregatedStorage
{
 public:
  // This is a copy of memory::SimpleSegregatedStorageBase::allocate, except for the marked sections.
  void* allocate(std::function<bool()> const& add_new_block)
  {
    /*
     * Inserted debug code: use memory::PtrTag.
     */
    using memory::PtrTag;
    /*
     * End of inserted debug code.
     */

    for (;;)
    {
      // Load the current value of m_head_tag into `head_tag`.
      // Use std::memory_order_acquire to synchronize with the std::memory_order_release in deallocate,
      // so that value of `next` read below will be the value written in deallocate corresponding to
      // this head value.
      PtrTag head_tag(m_head_tag.load(std::memory_order_acquire));
      while (head_tag != PtrTag::end_of_list)
      {
        PtrTag new_head_tag = head_tag.next();

        /*
         * Inserted debug code: make the first thread that comes here halt until `ABA_happened`.
         */
        if (first_time)
        {
          first_time = false;
          next_has_been_read.open();
          ABA_happened.wait();
        }
        /*
         * End of inserted debug code.
         */

        // The std::memory_order_acquire is used in case of failure and required for the next
        // read of m_next at the top of the current loop (the previous line).
        if (AI_LIKELY(CAS_head_tag(head_tag, new_head_tag, std::memory_order_acquire)))
          // Return the old head.
          return head_tag.ptr();
        // m_head_tag was changed (the new value is now in `head_tag`). Try again with the new value.
      }
      // Reached the end of the list, try to allocate more memory.
      if (!try_allocate_more(add_new_block))
        return nullptr;
    }
  }
};

class MemoryPool : public memory::MemoryPagePool
{
 private:
  // Replace m_sss with our own class (defined above).
  SimpleSegregatedStorage m_sss;

 public:
  // And use it.
  // These are exact copies of memory::MemoryPagePool::allocate and memory::MemoryPagePool::deallocate.
  void* allocate() override
  {
    return m_sss.allocate([this](){
        // This runs in the critical area of SimpleSegregatedStorage::m_add_block_mutex.
        blocks_t extra_blocks = std::clamp(m_pool_blocks, m_minimum_chunk_size, m_maximum_chunk_size);
        size_t extra_size = extra_blocks * m_block_size;
        void* chunk = std::aligned_alloc(memory_page_size(), extra_size);
        if (AI_UNLIKELY(chunk == nullptr))
          return false;
        m_sss.add_block(chunk, extra_size, m_block_size);
        m_pool_blocks += extra_blocks;
        m_chunks.push_back(chunk);
        return true;
    });
  }

  void deallocate(void* ptr) override
  {
    m_sss.deallocate(ptr);
  }

  // Inherit all constructors.
  using memory::MemoryPagePool::MemoryPagePool;
};

constexpr size_t partition_size = 8;            // Must be greater or equal the size of a pointer.
#endif
char* A_ptr;

// block contains four nodes:
// [A][B][C][D]
// After 1) we have:
// head -> A -> B -> C -> D -> null
// Then action1 does:
// next = B
// Then action2 does:
// After 2) we have:
// head -> B -> C -> D -> null
// After 3) we have:
// head -> C -> D -> null
// After 4) we have:
// head -> A -> C -> D -> null
// Then action1 continuous and does:
// head -> B

void action1(MemoryPool& mpp)
{
  Debug(NAMESPACE_DEBUG::init_thread("action1"));

  void* A = mpp.allocate();                     // 1)
  // If this fails and A_ptr is null, then most likely the thread didn't encounter ABA_happened.wait().
  ASSERT(A == A_ptr);
  std::memset(A, 0xff, partition_size);

  Dout(dc::notice, "Leaving thread.");
}

void action2(MemoryPool& mpp)
{
  Debug(NAMESPACE_DEBUG::init_thread("action2"));

  next_has_been_read.wait();

  void* A = mpp.allocate();                     // 2)
  A_ptr = reinterpret_cast<char*>(A);
  std::memset(A, 0xff, partition_size);
  void* B = mpp.allocate();                     // 3)
  ASSERT(B == A_ptr + partition_size);
  std::memset(B, 0xff, partition_size);

  mpp.deallocate(A);                            // 4)
  ABA_happened.open();

  Dout(dc::notice, "Leaving thread.");
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());

#if TEST_MAPPED
  MemoryPool mpp("ABA_test_mmap.img", partition_size, 4 * partition_size, MemoryPool::Mode::persistent, true);
#else
  MemoryPool mpp(partition_size, 4, 4);
#endif

  std::thread thr1(action1, std::ref(mpp));
  std::thread thr2(action2, std::ref(mpp));

  thr1.join();
  thr2.join();

  void* C = mpp.allocate();
  void* D = mpp.allocate();
  ASSERT(C == A_ptr + 2 * partition_size);
  ASSERT(D == A_ptr + 3 * partition_size);
}
