.PHONY: all release debug test clean
BUILD_DIR ?= build/release
DUCKDB ?= duckdb
PYTHON ?= python3
CMAKE ?= cmake

all: release

release:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) --parallel

debug:
	$(MAKE) release BUILD_DIR=build/debug CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug $(CMAKE_ARGS)"

test:
	$(PYTHON) test/integration.py --duckdb $(DUCKDB) --extension $(BUILD_DIR)/eviltransform.duckdb_extension

clean:
	$(CMAKE) -E rm -rf build
