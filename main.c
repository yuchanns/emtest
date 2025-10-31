#define SOKOL_IMPL
#define SOKOL_WGPU

#include "sokol/sokol_app.h"
#include "sokol/sokol_args.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

#include <emscripten/emscripten.h>
#include <emscripten/wasm_worker.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lualib.h"
#include <lauxlib.h>
#include <lua.h>

#define WASM_STACK_SIZE (64 * 1024)

struct thread {
  void (*func)(void *);
  void *ud;
};

static void *thread_start(struct thread *threads, int n);
static void thread_join(void *handle, int n);

struct wasm_worker_context {
  struct thread *thread;
  emscripten_semaphore_t *semaphore;
};

struct wasm_thread_group {
  emscripten_wasm_worker_t *workers;
  struct wasm_worker_context *contexts;
  emscripten_semaphore_t semaphore;
};

static inline void
thread_function(int arg) {
  struct wasm_worker_context *ctx = (struct wasm_worker_context *)(intptr_t)arg;
  emscripten_semaphore_release(ctx->semaphore, 1);
  ctx->thread->func(ctx->thread->ud);
  emscripten_semaphore_waitinf_acquire(ctx->semaphore, 1);
}

static void
wasm_thread_group_destroy(struct wasm_thread_group *group,
                                      int worker_count) {
  if (!group) {
    return;
  }
  if (group->workers) {
    for (int i = 0; i < worker_count; ++i) {
      if (group->workers[i] > 0) {
        emscripten_terminate_wasm_worker(group->workers[i]);
      }
    }
    free(group->workers);
  }
  if (group->contexts) {
    free(group->contexts);
  }
  free(group);
}

static inline void *
thread_start(struct thread *threads, int n) {
  struct wasm_thread_group *group =
      (struct wasm_thread_group *)malloc(sizeof(*group));
  if (!group) {
    return NULL;
  }
  group->workers = NULL;
  group->contexts = NULL;

  emscripten_semaphore_init(&group->semaphore, 0);

  group->workers =
      (emscripten_wasm_worker_t *)malloc(n * sizeof(emscripten_wasm_worker_t));
  group->contexts = (struct wasm_worker_context *)malloc(
      n * sizeof(struct wasm_worker_context));
  if (!group->workers || !group->contexts) {
    wasm_thread_group_destroy(group, 0);
    return NULL;
  }

  for (int i = 0; i < n; ++i) {
    group->contexts[i].thread = &threads[i];
    group->contexts[i].semaphore = &group->semaphore;
    emscripten_wasm_worker_t worker =
        emscripten_malloc_wasm_worker(WASM_STACK_SIZE);
    if (worker <= 0) {
      wasm_thread_group_destroy(group, i);
      return NULL;
    }
    group->workers[i] = worker;
    emscripten_wasm_worker_post_function_vi(worker, thread_function,
                                            (int)(intptr_t)&group->contexts[i]);
  }

  return group;
}

static inline void
thread_join(void *handle, int n) {
  struct wasm_thread_group *group = (struct wasm_thread_group *)handle;
  if (!group) {
    return;
  }

  while (emscripten_atomic_load_u32((const void *)&group->semaphore) != 0) {
    emscripten_sleep(1);
  }

  wasm_thread_group_destroy(group, n);
  fprintf(stdout, "Thread group destroyed.\n");
}

static void
run_in_worker(void *arg) {
  (void)arg;
  emscripten_outf("Worker started.\n");
  int i;
  for (i = 0;; i++) {
    double now = emscripten_performance_now();
    emscripten_outf("Worker is working... iteration %d at time %.2f ms\n", i,
                    now);
    uint64_t ns = 5 * 1000000000ULL;
    emscripten_wasm_worker_sleep(ns);
    if (i >= 5) {
      break;
    }
  }
  emscripten_outf("Worker finished.\n");
  sapp_quit();
}

struct app_context {
  lua_State *L;
};

static struct app_context *CTX = NULL;

static int
lthread_start(lua_State *L) {
  int n = (int)luaL_checkinteger(L, 1);
  if (n <= 0) {
    luaL_error(L, "Invalid number of threads");
    return 0;
  }

  struct thread *threads =
      (struct thread *)malloc((size_t)n * sizeof(struct thread));
  if (!threads) {
    luaL_error(L, "Failed to allocate threads");
    return 0;
  }

  for (int i = 0; i < n; i++) {
    threads[i].func = run_in_worker;
    threads[i].ud = NULL;
  }

  void *handle = thread_start(threads, n);

  if (!handle) {
    luaL_error(L, "Failed to start threads");
    return 0;
  }

  lua_pushlightuserdata(L, handle);
  return 1;
}

static int
lthread_join(lua_State *L) {
  void *handle = lua_touserdata(L, 1);
  int n = (int)luaL_checkinteger(L, 2);
  if (handle) {
    thread_join(handle, n);
  }
  return 0;
}

static int
start_app(lua_State *L) {
  lua_pushcfunction(L, lthread_start);
  lua_pushinteger(L, 2);

  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Error during Lua start: %s\n", err);
    lua_pop(L, 1);
    return 1;
  }

  lua_setfield(L, LUA_REGISTRYINDEX, "thread_handle");

  return 0;
}

static void
app_init() {
  lua_State *L = CTX->L;
  if (!L) {
    fprintf(stderr, "Lua state is NULL, quitting application.\n");
    sapp_quit();
  }
  if (start_app(L) != 0) {
    sapp_quit();
  }
}

static void
app_cleanup(void) {
  lua_State *L = CTX->L;
  if (L) {
    lua_pushcfunction(L, lthread_join);
    lua_getfield(L, LUA_REGISTRYINDEX, "thread_handle");
    lua_pushinteger(L, 2);
    lua_pcall(L, 2, 0, 0);
    lua_close(L);
    CTX->L = NULL;
  }
}

void
openlibs(lua_State *L) { luaL_openlibs(L); }

static int
msghandler(lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  luaL_traceback(L, L, msg, 1);
  return 1;
}

static int
pmain(lua_State *L) {
  openlibs(L);
  return 0;
}

sapp_desc
sokol_main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  static struct app_context app;
  lua_State *L = luaL_newstate();

  if (L) {
    lua_settop(L, 0);
    lua_pushcfunction(L, msghandler);
    lua_pushcfunction(L, pmain);

    if (lua_pcall(L, 0, 1, 1) != LUA_OK) {
      const char *err = lua_tostring(L, -1);
      lua_pushlightuserdata(L, (void *)err);
      lua_replace(L, 1);
    }
  }

  app.L = L;

  CTX = &app;

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
