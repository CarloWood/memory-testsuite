#include "sys.h"
#include "memory/MemoryPagePool.h"
#include "cwds/benchmark.h"
#include <boost/thread/thread.hpp>
#include <condition_variable>
#include "debug.h"

int const iterations = 1000000;
int const producer_thread_count = 4;
int const consumer_thread_count = 4;
int const total_threads = producer_thread_count + consumer_thread_count;
memory::MemoryPagePool mpp(0x1000, 2, 1024 * 1024);

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

void producer()
{
  Debug(NAMESPACE_DEBUG::init_thread());

  wait_for_threads(total_threads);

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = mpp.allocate();
    *static_cast<int*>(ptr) = i;
  }
}

std::atomic_bool done;

void consumer()
{
  Debug(NAMESPACE_DEBUG::init_thread());

  std::vector<void*> blocks;
  blocks.reserve(iterations);

  for (int i = 0; i != iterations; ++i)
  {
    void* ptr = mpp.allocate();
    *static_cast<int*>(ptr) = i;
    blocks.push_back(ptr);
  }

  benchmark::Stopwatch sw;
  wait_for_threads(total_threads);

  sw.start();

  for (int i = 0; i != iterations; ++i)
    mpp.deallocate(blocks[i]);

  sw.stop();

  Dout(dc::notice|flush_cf, "Leaving thread! Ran for " << (sw.diff_cycles() / 3612059050.0) << " seconds.");
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  Dout(dc::notice, "Entered main()...");

  boost::thread_group producer_threads;
  boost::thread_group consumer_threads;

  for (int i = 0; i != producer_thread_count; ++i)
    producer_threads.create_thread(producer);

  for (int i = 0; i != consumer_thread_count; ++i)
    consumer_threads.create_thread(consumer);

  producer_threads.join_all();
  done = true;
  consumer_threads.join_all();

  Dout(dc::notice, "Leaving main()...");
}
