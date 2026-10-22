.DEFAULT_GOAL := all
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXTENSION_NAME := eviltransform
USE_UNSTABLE_C_API := 0
TARGET_DUCKDB_VERSION := v1.2.0
EXTENSION_VERSION := 0.1

BUILD_DIR ?= build/release
DUCKDB ?= duckdb
PYTHON ?= python3
PYTHON_BIN ?= $(PYTHON)

# Use the platform selected by extension-ci-tools for both metadata writers.
CMAKE_EXTRA_BUILD_FLAGS += -DDUCKDB_PLATFORM=$(strip $(shell $(PYTHON_BIN) -c "from pathlib import Path;print(Path('configure/platform.txt').read_text().strip())"))

include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

.PHONY: all configure release debug test test_release test_debug test_integration clean

all: release

configure: venv platform extension_version

platform configure/extension_version.txt: venv

release: build_extension_with_metadata_release

debug: build_extension_with_metadata_debug

test: test_release

test_release: test_extension_release

test_debug: test_extension_debug

test_integration:
	$(PYTHON) test/integration.py --duckdb $(DUCKDB) --extension $(BUILD_DIR)/eviltransform.duckdb_extension

clean: clean_build clean_cmake clean_configure
