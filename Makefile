# Top-level build interface, mirroring neovim's Makefile-wraps-CMake pattern.
# Targets here are the contract documented in CLAUDE.md.
# Constrained to GNU Make 3.81 (what Apple ships).

BUILD_DIR ?= build
CMAKE_BUILD_TYPE ?= Debug
SANITIZE ?= 0
CMAKE_EXTRA_FLAGS ?=

CMAKE ?= cmake
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/clang-tidy)
STYLUA ?= stylua
LUACHECK ?= luacheck
# Prefer the user-local tree: the README's bootstrap pairs it with LuaJIT,
# while a system-tree busted may be paired with plain Lua (no ffi).
BUSTED ?= $(shell [ -x $(HOME)/.luarocks/bin/busted ] && echo $(HOME)/.luarocks/bin/busted || command -v busted 2>/dev/null || echo busted)
# Homebrew's clang-tidy does not know Apple's default SDK; pass it explicitly.
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)

C_SOURCES = $(shell find src/mua test/functional/fixtures -name '*.c' 2>/dev/null)
C_HEADERS = $(shell find src/mua test/functional/fixtures -name '*.h' 2>/dev/null)
LUA_LINT_TARGETS = $(wildcard runtime) $(wildcard test) $(wildcard .busted)
STYLUA_TARGETS = $(wildcard runtime) $(wildcard test)

TEST_ENV = MUA_TEST_LIB=$(BUILD_DIR)/lib/libmua.dylib MUA_PRG=$(BUILD_DIR)/bin/mua \
	MUA_SSE_SERVER=$(BUILD_DIR)/bin/mua_sse_server

ifeq ($(SANITIZE),1)
SAN_FLAG = -DENABLE_SANITIZERS=ON
# The ASan runtime must be preloaded into luajit before the test dylib is
# dlopen'd. SIP strips DYLD_* across busted's /bin/sh launcher, so invoke the
# (unprotected) luajit binary directly with the local rocks tree on the path.
ASAN_DYLIB = $(shell cc -print-resource-dir)/lib/darwin/libclang_rt.asan_osx_dynamic.dylib
LUAJIT_BIN ?= $(shell command -v luajit 2>/dev/null || echo /opt/homebrew/bin/luajit)
BUSTED_INNER = $(firstword $(wildcard $(HOME)/.luarocks/lib/luarocks/rocks-5.1/busted/*/bin/busted))
LR_PATH = $(shell luarocks --local --lua-version=5.1 path --lr-path 2>/dev/null)
LR_CPATH = $(shell luarocks --local --lua-version=5.1 path --lr-cpath 2>/dev/null)
UNIT_BUSTED = DYLD_INSERT_LIBRARIES=$(ASAN_DYLIB) LUA_PATH="$(LR_PATH);;" \
	LUA_CPATH="$(LR_CPATH);;" $(LUAJIT_BIN) $(BUSTED_INNER)
else
SAN_FLAG = -DENABLE_SANITIZERS=OFF
UNIT_BUSTED = $(BUSTED)
endif

MAKEFLAGS += --no-print-directory

all: build

$(BUILD_DIR)/.ran-cmake:
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(SAN_FLAG) $(CMAKE_EXTRA_FLAGS)
	touch $@

build: $(BUILD_DIR)/.ran-cmake
	$(CMAKE) --build $(BUILD_DIR)
	@ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json

# Changing SANITIZE or CMAKE_BUILD_TYPE requires `make distclean` first.

ifneq ($(TEST_FILE),)
ifneq ($(filter test/unit/%,$(TEST_FILE)),)
unittest: build
	$(TEST_ENV) $(UNIT_BUSTED) --run=unit $(TEST_FILE)
else
unittest:
	@echo "unittest: TEST_FILE is not under test/unit -- skipping"
endif
ifneq ($(filter test/functional/%,$(TEST_FILE)),)
functionaltest: build
	$(TEST_ENV) $(BUSTED) --run=functional $(TEST_FILE)
else
functionaltest:
	@echo "functionaltest: TEST_FILE is not under test/functional -- skipping"
endif
else
unittest: build
	@if [ -n "$$(find test/unit -name '*_spec.lua' 2>/dev/null)" ]; then \
		$(TEST_ENV) $(UNIT_BUSTED) --run=unit; \
	else echo "unittest: no specs yet"; fi
functionaltest: build
	@if [ -n "$$(find test/functional -name '*_spec.lua' 2>/dev/null)" ]; then \
		$(TEST_ENV) $(BUSTED) --run=functional; \
	else echo "functionaltest: no specs yet"; fi
endif

test: unittest functionaltest

lint: lint-c lint-lua

lint-c: build
	@if [ -n "$(C_SOURCES)" ]; then \
		$(CLANG_TIDY) -p $(BUILD_DIR) --quiet \
			$(if $(SDKROOT),--extra-arg=-isysroot --extra-arg=$(SDKROOT)) $(C_SOURCES); \
	else echo "lint-c: no C sources yet"; fi

lint-lua:
	@if [ -n "$(LUA_LINT_TARGETS)" ]; then \
		$(LUACHECK) $(LUA_LINT_TARGETS); \
	else echo "lint-lua: no Lua yet"; fi

format:
	@if [ -n "$(C_SOURCES)$(C_HEADERS)" ]; then $(CLANG_FORMAT) -i $(C_SOURCES) $(C_HEADERS); fi
	@if [ -n "$(STYLUA_TARGETS)" ]; then $(STYLUA) $(STYLUA_TARGETS); fi

clean:
	@if [ -d $(BUILD_DIR) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf $(BUILD_DIR) compile_commands.json

.PHONY: all build test unittest functionaltest lint lint-c lint-lua format clean distclean
