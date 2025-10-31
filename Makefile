EMCC ?= emcc
EMRUN ?= emrun
OUT := emtest.js
SRC := main.c

.PHONY: all clean

all: $(OUT)
	$(EMRUN) --no_browser --serve_root . --port 8000 $(OUT)

$(OUT): $(SRC)
	$(EMCC) -o $@ -s WASM_WORKERS=1 --use-port=emdawnwebgpu -I. $<

clean:
	$(RM) *.js *.wasm
