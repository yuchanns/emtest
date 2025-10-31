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

static inline void *thread_start(struct thread *threads, int n) {
  if (!threads || (n <= 0)) {
    return NULL;
  }

  if (!g_worker_semaphore_initialized) {
    emscripten_semaphore_init(&g_worker_semaphore, 0);
    g_worker_semaphore_initialized = true;
  }

  struct thread *jobs =
      (struct thread *)malloc((size_t)n * sizeof(struct thread));
  if (!jobs) {
    return NULL;
  }

  for (int i = 0; i < n; i++) {
    jobs[i] = threads[i];
    emscripten_wasm_worker_t worker =
        emscripten_malloc_wasm_worker(WASM_STACK_SIZE);
    if (worker <= 0) {
      emscripten_terminate_all_wasm_workers();
      free(jobs);
      return NULL;
    }
    emscripten_wasm_worker_post_function_vi(worker, thread_function,
                                            (int)(uintptr_t)&jobs[i]);
  }

  return (void *)jobs;
}

static inline void thread_join(void *handle, int n) {
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
  emscripten_outf("Worker started.\n");
  for (;;) {
    emscripten_outf("Worker is working...\n");
    uint64_t ns = 5 * 1000000000ULL;
    emscripten_wasm_worker_sleep(ns);
  }
}

struct app_context {
  lua_State *L;
};

static struct app_context *CTX = NULL;

static int start_app(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "MAINCHUNK");
  if (lua_islightuserdata(L, 1)) {
    fprintf(stderr, "Error during Lua initialization: %s\n",
            (const char *)lua_touserdata(L, 1));
    return 1;
  }
  lua_getfield(L, -1, "start");
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Error during Lua start: %s\n", err);
    return 1;
  }
  lua_pop(L, 1);
  return 0;
}

static void app_init() {
  lua_State *L = CTX->L;
  if (!L) {
    fprintf(stderr, "Lua state is NULL, quitting application.\n");
    sapp_quit();
  }
  if (start_app(L) != 0) {
    sapp_quit();
  }
}

static int pwait(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "MAINCHUNK");
  lua_getfield(L, -1, "wait");
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    return lua_error(L);
  }
  return 0;
}

static void app_cleanup(void) {
  lua_State *L = CTX->L;
  if (L) {
    lua_pushcfunction(L, pwait);
    lua_call(L, 0, 0);
    lua_close(L);
    CTX->L = NULL;
  }
}

static int lthread_start(lua_State *L) {
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
  free(threads);

  if (!handle) {
    luaL_error(L, "Failed to start threads");
    return 0;
  }

  lua_pushlightuserdata(L, handle);
  return 1;
}

static int lthread_join(lua_State *L) {
  void *handle = lua_touserdata(L, 1);
  if (!handle) {
    luaL_error(L, "Invalid thread handle");
    return 0;
  }
  thread_join(handle, 0);
  return 0;
}

LUAMOD_API int luaopen_emtest(lua_State *L) {
  luaL_Reg l[] = {
      {"start", lthread_start},
      {"join", lthread_join},
      {NULL, NULL},
  };
  luaL_newlib(L, l);
  return 1;
}

void openlibs(lua_State *L) {
  luaL_openlibs(L);

  luaL_requiref (L, "emtest", luaopen_emtest, 0);
}

static int msghandler(lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  luaL_traceback(L, L, msg, 1);
  return 1;
}

static const char *code =
    "local f = assert(loadfile('/data/main.lua')); return f()";

static int pmain(lua_State *L) {
  openlibs(L);

  if (luaL_loadstring(L, code) != LUA_OK) {
    return lua_error(L);
  }

  lua_call(L, 0, 1);
  lua_setfield(L, LUA_REGISTRYINDEX, "MAINCHUNK");
  return 0;
}

sapp_desc sokol_main(int argc, char *argv[]) {
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
