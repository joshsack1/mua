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
BUSTED ?= $(shell command -v busted 2>/dev/null || echo $(HOME)/.luarocks/bin/busted)
# Homebrew's clang-tidy does not know Apple's default SDK; pass it explicitly.
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)

C_SOURCES = $(shell find src/mua -name '*.c' 2>/dev/null)
C_HEADERS = $(shell find src/mua -name '*.h' 2>/dev/null)
LUA_LINT_TARGETS = $(wildcard runtime) $(wildcard test) $(wildcard .busted)
STYLUA_TARGETS = $(wildcard runtime) $(wildcard test)

TEST_ENV = MUA_TEST_LIB=$(BUILD_DIR)/lib/libmua.dylib MUA_PRG=$(BUILD_DIR)/bin/mua

ifeq ($(SANITIZE),1)
SAN_FLAG = -DENABLE_SANITIZERS=ON
ASAN_DYLIB = $(shell cc -print-resource-dir)/lib/darwin/libclang_rt.asan_osx_dynamic.dylib
UNIT_ENV = DYLD_INSERT_LIBRARIES=$(ASAN_DYLIB)
else
SAN_FLAG = -DENABLE_SANITIZERS=OFF
UNIT_ENV =
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
	$(TEST_ENV) $(UNIT_ENV) $(BUSTED) --run=unit $(TEST_FILE)
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
		$(TEST_ENV) $(UNIT_ENV) $(BUSTED) --run=unit; \
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
