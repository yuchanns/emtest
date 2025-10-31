#define SOKOL_IMPL
#define SOKOL_WGPU

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_args.h"

#include <emscripten/wasm_worker.h>
#include <emscripten/emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define WASM_STACK_SIZE (64 * 1024)

struct thread {
	void (*func)(void *);
	void *ud;
};

static void* thread_start(struct thread * threads, int n);
static void thread_join(void *handle, int n);

static emscripten_semaphore_t g_worker_semaphore;
static bool g_worker_semaphore_initialized = false;
static void *g_worker_handle = NULL;
static int g_worker_count = 0;

static inline void thread_function(int slot_ptr) {
  struct thread *job = (struct thread *)(uintptr_t)slot_ptr;
  if (!job) {
    return;
  }

  emscripten_semaphore_release(&g_worker_semaphore, 1);
  job->func(job->ud);
  emscripten_semaphore_waitinf_acquire(&g_worker_semaphore, 1);
}

static inline void *thread_start(struct thread * threads, int n) {
  if (!threads || (n <= 0)) {
    return NULL;
  }

  if (!g_worker_semaphore_initialized) {
    emscripten_semaphore_init(&g_worker_semaphore, 0);
    g_worker_semaphore_initialized = true;
  }

  struct thread *jobs = (struct thread *)malloc((size_t)n * sizeof(struct thread));
  if (!jobs) {
    return NULL;
  }

  for (int i = 0; i < n; i++) {
    jobs[i] = threads[i];
    emscripten_wasm_worker_t worker = emscripten_malloc_wasm_worker(WASM_STACK_SIZE);
    if (worker <= 0) {
      emscripten_terminate_all_wasm_workers();
      free(jobs);
      return NULL;
    }
    emscripten_wasm_worker_post_function_vi(worker, thread_function, (int)(uintptr_t)&jobs[i]);
  }

  return (void *)jobs;
}

static inline void
thread_join(void *handle, int n) {
  (void)n;

  if (!handle) {
    emscripten_terminate_all_wasm_workers();
    return;
  }

  if (g_worker_semaphore_initialized) {
    while (emscripten_atomic_load_u32((const void *)&g_worker_semaphore) != 0) {
      emscripten_sleep(1);
    }
  }

  emscripten_terminate_all_wasm_workers();
  free(handle);
}

static void run_in_worker(void *arg) {
  (void)arg;
  fprintf(stdout, "Worker is starting a task...\n");
  for (;;) {
    fprintf(stdout, "Worker is working...\n");
    uint64_t ns = 5 * 1000000000ULL;
    emscripten_wasm_worker_sleep(ns);
  }
}

static void
app_init() {
    fprintf(stdout, "Starting workers from sokol_app init...\n");

    g_worker_count = 2;
    struct thread *threads =
      (struct thread *)malloc((size_t)g_worker_count * sizeof(struct thread));
    if (!threads) {
      fprintf(stderr, "Failed to allocate threads\n");
      g_worker_count = 0;
      return;
    }

    for (int i = 0; i < g_worker_count; i++) {
      threads[i].func = run_in_worker;
      threads[i].ud = NULL;
    }

    g_worker_handle = thread_start(threads, g_worker_count);
    free(threads);

    if (!g_worker_handle) {
      fprintf(stderr, "Failed to start threads\n");
      g_worker_count = 0;
      return;
    }

    fprintf(stdout, "Workers are running.\n");
}

static void
app_cleanup(void) {
    fprintf(stdout, "Cleaning up workers...\n");
    thread_join(g_worker_handle, g_worker_count);
    g_worker_handle = NULL;
    g_worker_count = 0;
    fprintf(stdout, "Cleanup complete.\n");
}

sapp_desc
sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    return (sapp_desc){
        .init_cb = app_init,
        .frame_cb = NULL,
        .cleanup_cb = app_cleanup,
        .event_cb = NULL,
        .width = 800,
        .height = 600,
        .window_title = "Sokol Wasm Worker Example",
    };
}
