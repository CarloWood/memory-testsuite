#include "sys.h"
#include "debug.h"
#include "utils/NodeMemoryPool.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <random>

unsigned int seed = 4321;

struct Foo {
  char a[120];
};

void test1()
{
  DoutEntering(dc::notice, "test1()");
  utils::NodeMemoryPool pool(128);

  size_t num_mallocs = 0;
  std::vector<void*> ptrs;
  for (int i = 0; i < 1025; ++i)
  {
    void* ptr = pool.malloc<Foo>();
    ++num_mallocs;
    if (i % 2 == 0)
      pool.free(ptr);
    else
      ptrs.push_back(ptr);
  }
  Dout(dc::notice, "Number of allocated chunks now " << ptrs.size() << "; " << pool);
  ASSERT(ptrs.size() == 1025 / 2);
  for (int i = 0; i < 1023; ++i)
  {
    void* ptr = pool.malloc<Foo>();
    ++num_mallocs;
    ptrs.push_back(ptr);
  }
  Dout(dc::notice, "Number of allocated chunks now " << ptrs.size() << "; " << pool);
  ASSERT(ptrs.size() == 1025 / 2 + 1023);
  std::vector<unsigned int> index(ptrs.size());
  for (unsigned int i = 0; i < ptrs.size(); ++i)
    index[i] = i;
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(index.begin(), index.end(), g);
  for (unsigned int i = 0; i < ptrs.size(); ++i)
    pool.free(ptrs[index[i]]);

  Dout(dc::notice, "Did " << num_mallocs << " allocations and frees.");
  Dout(dc::notice, pool);
}

struct Bar {
  int n;
  char a[30 - sizeof(int)];

  Bar(int n_) : n(n_) { }
  ~Bar() { std::memset(this, 0, 30); }

  void operator delete(void* ptr) { utils::NodeMemoryPool::static_free(ptr); }
};

struct ExtendedBar : public Bar {
  unsigned short m_x;
};

void test2()
{
  DoutEntering(dc::notice, "test2()");
  utils::NodeMemoryPool pool(32, sizeof(Bar) + 8);          // Allocate chunks that are larger than the object.
  Bar* bar = new(pool) Bar(42);
  ASSERT(bar->n == 42);
  reinterpret_cast<ExtendedBar*>(bar)->m_x = 0x1234;        // Secret space after the object ;).
  delete bar;
  ASSERT(reinterpret_cast<ExtendedBar*>(bar)->m_x == 0x1234);
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  test1();
  test2();
  Dout(dc::notice, "Leaving main()...");
}
