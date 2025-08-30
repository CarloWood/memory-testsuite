#include "sys.h"
#include "memory/SimpleSegregatedStorage.h"
#include "utils/threading/Gate.h"
#include <thread>
#include "debug.h"

using memory::FreeNode;

utils::threading::Gate next_has_been_read;
utils::threading::Gate ABA_happened;
std::atomic<bool> first_time = true;

struct PtrTag
{
  std::uintptr_t encoded_;

  static constexpr std::uintptr_t tag_mask = 0x3;
  static constexpr std::uintptr_t ptr_mask = ~tag_mask;
  static constexpr std::uintptr_t end_of_list = tag_mask;

  static constexpr std::uintptr_t encode(void* ptr, uint32_t tag)
  {
    return reinterpret_cast<std::uintptr_t>(ptr) | (tag & tag_mask);
  }

  PtrTag(std::uintptr_t encoded) : encoded_(encoded) { }
  PtrTag(FreeNode* node, std::uintptr_t tag) : encoded_(node ? PtrTag::encode(node, tag) : end_of_list) { }

  FreeNode* ptr() const { return reinterpret_cast<FreeNode*>(encoded_ & ptr_mask); }
  std::uintptr_t tag() const { return encoded_ & tag_mask; }

  PtrTag next() const
  {
    FreeNode* front_node = ptr();
    FreeNode* second_node = front_node->m_next;
    if (first_time)
    {
      first_time = false;
      next_has_been_read.open();
      ABA_happened.wait();
    }
    return {second_node, tag() + 1};
  }

  bool operator!=(std::uintptr_t encoded) const { return encoded_ != encoded; }
};

class SimpleSegregatedStorage : public memory::SimpleSegregatedStorageBase<PtrTag>
{
 public:                                // To be used with std::scoped_lock<std::mutex> from calling classes.
  std::mutex m_add_block_mutex;         // Protect against calling add_block concurrently.

 public:
  using memory::SimpleSegregatedStorageBase<PtrTag>::SimpleSegregatedStorageBase;

  bool try_allocate_more(std::function<bool()> const& add_new_block) override;
  void add_block(void* block, size_t block_size, size_t partition_size);
};

// Duplication of memory::SimpleSegregatedStorage::try_allocate_more.
bool SimpleSegregatedStorage::try_allocate_more(std::function<bool()> const& add_new_block)
{
  std::scoped_lock<std::mutex> lk(m_add_block_mutex);
  return m_head_tag.load(std::memory_order_relaxed) != PtrTag::end_of_list || add_new_block();
}

// Duplication of memory::SimpleSegregatedStorage::add_block.
void SimpleSegregatedStorage::add_block(void* block, size_t block_size, size_t partition_size)
{
  unsigned int const number_of_partitions = block_size / partition_size;

  // block_size must be a multiple of partition_size (at least 2 times).
  ASSERT(number_of_partitions > 1);

  char* const first_ptr = static_cast<char*>(block);
  char* const last_ptr = first_ptr + (number_of_partitions - 1) * partition_size;     // > first_ptr, see ASSERT.
  char* node = last_ptr;
  do
  {
    char* next_node = node;
    node = next_node - partition_size;
    reinterpret_cast<FreeNode*>(node)->m_next = reinterpret_cast<FreeNode*>(next_node);
  }
  while (node != first_ptr);

  FreeNode* const first_node = reinterpret_cast<FreeNode*>(first_ptr);
  FreeNode* const last_node = reinterpret_cast<FreeNode*>(last_ptr);
  PtrTag const new_head_tag(first_node, 0);
  PtrTag head_tag(m_head_tag.load(std::memory_order_relaxed));
  do
  {
    last_node->m_next = head_tag.ptr();
  }
  while (!CAS_head_tag(head_tag, new_head_tag, std::memory_order_release));
}

constexpr size_t partition_size = 16;
std::array<char, partition_size * 4> block;
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

void action1(SimpleSegregatedStorage& sss)
{
  Debug(NAMESPACE_DEBUG::init_thread("action1"));

  void* A = sss.allocate([&sss](){
    sss.add_block(block.data(), block.size(), partition_size);  // 1).
    return true;
  });
  ASSERT(A == block.data());
  std::memset(A, 0xff, partition_size);

  Dout(dc::notice, "Leaving thread.");
}

void action2(SimpleSegregatedStorage& sss)
{
  Debug(NAMESPACE_DEBUG::init_thread("action2"));

  next_has_been_read.wait();

  void* A = sss.allocate([&sss](){ ASSERT(false); return false; });     // 2)
  ASSERT(A == block.data());
  std::memset(A, 0xff, partition_size);
  void* B = sss.allocate([&sss](){ ASSERT(false); return false; });     // 3)
  ASSERT(B == block.data() + partition_size);
  std::memset(B, 0xff, partition_size);

  sss.deallocate(A);                                                    // 4)
  ABA_happened.open();

  Dout(dc::notice, "Leaving thread.");
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());

  SimpleSegregatedStorage sss;

  std::thread thr1(action1, std::ref(sss));
  std::thread thr2(action2, std::ref(sss));

  thr1.join();
  thr2.join();

  void* C = sss.allocate([&sss](){ ASSERT(false); return false; });
  void* D = sss.allocate([&sss](){ ASSERT(false); return false; });
  ASSERT(C == block.data() + 2 * partition_size);
  ASSERT(D == block.data() + 3 * partition_size);
}
