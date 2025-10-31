EMCC ?= emcc
EMRUN ?= emrun

OUT := emtest.js
SRC := main.c
LUA_SRC := lua/onelua.c

OBJ := $(SRC:.c=.o) $(LUA_SRC:.c=.o)
CPPFLAGS += -I. -Ilua
EMFLAGS := -s WASM_WORKERS=1 --use-port=emdawnwebgpu
LDFLAGS := -s FORCE_FILESYSTEM=1 -lidbfs.js

.PHONY: all clean

all: $(OUT)
	$(EMRUN) --no_browser --serve_root . --port 8000 $(OUT)

$(OUT): $(OBJ)
	$(EMCC) $(OBJ) -o $@ $(EMFLAGS) $(LDFLAGS)

%.o: %.c
	$(EMCC) $(CPPFLAGS) $(CFLAGS) $(EMFLAGS) -c $< -o $@

lua/onelua.o: lua/onelua.c
	$(EMCC) $(CPPFLAGS) $(CFLAGS) $(EMFLAGS) -DMAKE_LIB -c $< -o $@

clean:
	$(RM) *.js *.wasm $(OBJ)
