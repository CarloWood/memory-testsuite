#include "sys.h"
#include "memory/MemoryPagePool.h"
#include "memory/MemoryMappedPool.h"
#include "utils/AIAlert.h"
#include "utils/debug_ostream_operators.h"
#include "utils/threading/Gate.h"
#if defined(__OPTIMIZE__)
#include "cwds/benchmark.h"
#endif
#include <condition_variable>
#include <vector>
#include <algorithm>
#include <random>
#include "debug.h"

// Define this to 0 to test MemoryPagePool, and to 1 to test MemoryMappedPool.
#define TEST_MAPPED 0
#define HUGE 0

#if HUGE
size_t const mmap_file_size = 0x1000UL * 16 * 1024 * 1024;
int const iterations = 500000;
int const producer_thread_count = 16;
int const consumer_thread_count = 16;
#else
size_t const mmap_file_size = 0x1000UL * 4 * 1024 * 1024;
int const iterations = 250000;
int const producer_thread_count = 8;
int const consumer_thread_count = 8;
#endif

int const total_threads = producer_thread_count + consumer_thread_count;
#if TEST_MAPPED
using MemoryPool = memory::MemoryMappedPool;
#else
using MemoryPool = memory::MemoryPagePool;
#endif

std::mutex cvm;
std::condition_variable cv;
int thread_count;

void wait_for_threads(int n)
{
  std::unique_lock<std::mutex> lk(cvm);
  if (++thread_count == n)
  {
    Dout(dc::notice|flush_cf, "Releasing all threads!");
    cv.notify_all();
  }
  else
    cv.wait(lk, [n](){ return thread_count == n; });
}

void producer(int thread, utils::threading::Gate& consumers_joined, MemoryPool& mpp)
{
  Debug(NAMESPACE_DEBUG::init_thread(std::string("Producer ") + std::to_string(thread)));

  std::vector<void*> blocks(iterations);

  Dout(dc::notice, "producer: Entering wait_for_threads(" << total_threads << ")");
  wait_for_threads(total_threads);

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = mpp.allocate();
    if (ptr == nullptr)
    {
      DoutFatal(dc::core, "Out Of Memory. Is the mmap file large enough?");
    }
    *static_cast<int*>(ptr) = i;
    blocks[i] = ptr;
  }

  consumers_joined.wait();
  Dout(dc::notice|flush_cf, "Consumers all joined: deallocating my blocks...");

  std::vector<int> indexes(iterations);
  std::iota(indexes.begin(), indexes.end(), 0);
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(indexes.begin(), indexes.end(), g);

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = blocks[indexes[i]];
    ASSERT(*static_cast<int*>(ptr) == indexes[i]);
    mpp.deallocate(ptr);
  }

  Dout(dc::notice|flush_cf, "Leaving thread!");
}

void consumer(int thread, MemoryPool& mpp)
{
  Debug(NAMESPACE_DEBUG::init_thread(std::string("Consumer ") + std::to_string(thread)));

  std::vector<void*> blocks(iterations);

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = mpp.allocate();
    if (ptr == nullptr)
    {
      DoutFatal(dc::core, "Out Of Memory. Is the mmap file large enough?");
    }
    *static_cast<int*>(ptr) = i;
    blocks[i] = ptr;
  }

  std::vector<int> indexes(iterations);
  std::iota(indexes.begin(), indexes.end(), 0);
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(indexes.begin(), indexes.end(), g);

#if defined(__OPTIMIZE__)
  benchmark::Stopwatch sw;
#endif
  Dout(dc::notice, "consumer: Entering wait_for_threads(" << total_threads << ")");
  wait_for_threads(total_threads);

#if defined(__OPTIMIZE__)
  sw.start();
#endif

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = blocks[indexes[i]];
    ASSERT(*static_cast<int*>(ptr) == indexes[i]);
    mpp.deallocate(ptr);
  }

#if defined(__OPTIMIZE__)
  sw.stop();
  Dout(dc::notice|flush_cf, "Leaving thread! Ran for " << (sw.diff_cycles() / 3612059050.0) << " seconds.");
#else
  Dout(dc::notice|flush_cf, "Leaving thread!");
#endif

}

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  Dout(dc::notice, "Entered main()...");

  MemoryPool* mpp_ptr;
  try
  {
#if TEST_MAPPED
    mpp_ptr = new memory::MemoryMappedPool("/opt/cache/secondlife/mmap.img", 0x1000UL,
        mmap_file_size, memory::MemoryMappedPool::Mode::persistent, true);
#else
    mpp_ptr = new memory::MemoryPagePool(0x1000UL, 2, 1024 * 1024);
#endif
  }
  catch (AIAlert::Error const& error)
  {
    DoutFatal(dc::core, error);
  }

  std::vector<std::thread> producer_threads;
  std::vector<std::thread> consumer_threads;

  utils::threading::Gate consumers_joined;

  for (int i = 0; i != producer_thread_count; ++i)
    producer_threads.emplace_back([i, &consumers_joined, mpp_ptr](){ producer(i, consumers_joined, *mpp_ptr); });

  for (int i = 0; i != consumer_thread_count; ++i)
    consumer_threads.emplace_back([i, mpp_ptr](){ consumer(i, *mpp_ptr); });

  for (auto& t : consumer_threads)
    t.join();
  consumers_joined.open();
  for (auto& t : producer_threads)
    t.join();

  delete mpp_ptr;

  Dout(dc::notice, "Leaving main()...");
}
