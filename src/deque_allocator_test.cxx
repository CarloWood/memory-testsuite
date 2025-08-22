#include "sys.h"
#include "memory/NodeMemoryResource.h"
#include <deque>
#include <cstdlib>
#include <new>
#include <array>

class AIStatefulTask;

static constexpr size_t glibcxx_deque_buf_size =
#ifdef _GLIBCXX_DEQUE_BUF_SIZE
    _GLIBCXX_DEQUE_BUF_SIZE
#else
    512
#endif
  ;

template<typename T>
struct DequePoolAllocator
{
  using value_type = T;
  using map_pointer_type = T*;

  DequePoolAllocator(memory::NodeMemoryResource& nmr) : m_nmr(&nmr) { }
  DequePoolAllocator(DequePoolAllocator const&) = default;

  // This allocator also supports allocating blocks with map_pointer_type's, for which we use a normal std::allocator.
  operator std::allocator<map_pointer_type> const() const noexcept { return {}; }
  template<typename U> struct rebind { using other = std::conditional_t<std::is_same_v<U, T>, DequePoolAllocator<T>, std::allocator<U>>; };

  [[nodiscard]] T* allocate(std::size_t n)
  {
    //Dout(dc::notice, "Calling DequePoolAllocator<" << libcwd::type_info_of<T>().demangled_name() << ">::allocate(" << n << ")");
    ASSERT(n <= glibcxx_deque_buf_size / sizeof(T));
    return static_cast<T*>(m_nmr->allocate(glibcxx_deque_buf_size));
  }

  void deallocate(T* p, std::size_t UNUSED_ARG(n)) noexcept
  {
    //Dout(dc::notice, "Calling DequePoolAllocator<" << libcwd::type_info_of<T>().demangled_name() << ">::deallocate(" << p << ", " << n << ")");
    m_nmr->deallocate(p);
  }

 private:
  memory::NodeMemoryResource* m_nmr;
};

template<typename T>
bool operator==(DequePoolAllocator<T> const& alloc1, DequePoolAllocator<T> const& alloc2)
{
  return &alloc1 == &alloc2;
}

template<typename T>
bool operator!=(DequePoolAllocator<T> const& alloc1, DequePoolAllocator<T> const& alloc2)
{
  return &alloc1 != &alloc2;
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  Dout(dc::notice, "Entering main()...");

  AIStatefulTask* ptr = nullptr;

  memory::MemoryPagePool mpp(0x8000);
  memory::NodeMemoryResource nmr(mpp);
  DequePoolAllocator<AIStatefulTask*> alloc(nmr);
  {
    std::deque<AIStatefulTask*, decltype(alloc)> test_deque(alloc);

    for (int n = 0; n < 10000; ++n)
      test_deque.push_back(ptr);
    for (int j = 0; j < 10; ++j)
    {
      for (int n = 0; n < 9000; ++n)
        test_deque.pop_front();
      for (int n = 0; n < 1000; ++n)
        test_deque.push_back(ptr);
      for (int n = 0; n < 1000; ++n)
        test_deque.pop_front();
      for (int n = 0; n < 9000; ++n)
        test_deque.push_back(ptr);
    }

    AIStatefulTask* res = test_deque.front();
    test_deque.pop_front();

    Dout(dc::notice, "Popped " << res);

    Dout(dc::notice, "Destructing deque...");
  }

  Dout(dc::notice, "Leaving main()...");
}
