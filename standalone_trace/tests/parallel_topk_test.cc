// ParallelTopK: worker-thread startup failure must not abort (std::terminate from a joinable
// std::thread) and must not change the result; exceptions thrown by `scan` still propagate.
//
// pthread_create is interposed in this executable (std::thread calls it through the PLT), so
// individual worker spawns can be made to fail with EAGAIN.
#include "db/ParallelTopK.h"

#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <functional>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 0: never fail; N > 0: fail the N-th spawn attempt; -1: fail every spawn attempt.
int g_fail_spawn = 0;
int g_spawn_attempts = 0;
int g_spawned = 0;

} // namespace

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *),
                              void *arg) {
  using Fn = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
  static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pthread_create"));
  ++g_spawn_attempts;
  if (g_fail_spawn == -1 || g_fail_spawn == g_spawn_attempts) return EAGAIN;
  const int rc = real(thread, attr, start, arg);
  if (rc == 0) ++g_spawned;
  return rc;
}

namespace {

constexpr size_t kN = 200000; // well above kParallelScanMinItems: several workers
constexpr size_t kLimit = 7;

size_t ValueAt(size_t i) { return (i * 7919u + 13u) % kN; }

std::vector<size_t> Expected() {
  std::vector<size_t> all(kN);
  for (size_t i = 0; i < kN; ++i) all[i] = ValueAt(i);
  std::sort(all.begin(), all.end());
  all.resize(kLimit);
  return all;
}

std::vector<size_t> Run(size_t throw_at = kN) {
  auto scan = [throw_at](size_t begin, size_t end, auto &out) {
    for (size_t i = begin; i < end; ++i) {
      if (i == throw_at) throw std::runtime_error("scan failure at " + std::to_string(i));
      out.Push(ValueAt(i));
    }
  };
  return rtl_trace::ParallelTopK<size_t>(kN, kLimit, std::less<size_t>{}, scan);
}

bool Check(const char *label, int fail_spawn, int min_spawned, int max_spawned) {
  g_fail_spawn = fail_spawn;
  g_spawn_attempts = 0;
  g_spawned = 0;
  const std::vector<size_t> got = Run();
  const int spawned = g_spawned;
  g_fail_spawn = 0;
  if (got != Expected()) {
    std::cerr << label << ": result differs from the serial answer\n";
    return false;
  }
  if (spawned < min_spawned || spawned > max_spawned) {
    std::cerr << label << ": spawned " << spawned << " workers, want [" << min_spawned << ", "
              << max_spawned << "]\n";
    return false;
  }
  std::cout << label << ": ok (attempts " << g_spawn_attempts << ", spawned " << spawned << ")\n";
  return true;
}

bool CheckScanExceptionPropagates(const char *label, int fail_spawn) {
  g_fail_spawn = fail_spawn;
  g_spawn_attempts = 0;
  bool caught = false;
  try {
    (void)Run(kN / 2);
  } catch (const std::runtime_error &) {
    caught = true;
  }
  g_fail_spawn = 0;
  if (!caught) {
    std::cerr << label << ": scan exception was not propagated\n";
    return false;
  }
  std::cout << label << ": ok\n";
  return true;
}

} // namespace

int main() {
  const int workers = static_cast<int>(
      std::min({static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency())),
                rtl_trace::kParallelScanMaxThreads,
                (kN + rtl_trace::kParallelScanChunk - 1) / rtl_trace::kParallelScanChunk})) - 1;
  if (workers < 2) {
    // Needs at least two worker spawns for the partial-failure cases to mean anything.
    std::cout << "parallel_topk_test: only " << workers << " worker thread(s) available; partial "
              << "spawn-failure cases degenerate, running them anyway\n";
  }
  bool ok = true;
  ok &= Check("no spawn failure", 0, workers, workers);
  ok &= Check("first spawn fails", 1, 0, 0);
  ok &= Check("second spawn fails", 2, workers >= 1 ? 1 : 0, workers >= 1 ? 1 : 0);
  ok &= Check("every spawn fails", -1, 0, 0);
  ok &= CheckScanExceptionPropagates("scan exception, all workers", 0);
  ok &= CheckScanExceptionPropagates("scan exception, second spawn fails", 2);
  ok &= CheckScanExceptionPropagates("scan exception, every spawn fails", -1);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
