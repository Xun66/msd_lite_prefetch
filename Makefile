BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4

.PHONY: all configure build test clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

test:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DENABLE_TESTS=ON
	cmake --build $(BUILD_DIR) --parallel $(JOBS)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	cmake --build $(BUILD_DIR) --target clean
