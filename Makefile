.PHONY: all build test clean

all: test

build:
	@echo "Updating submodules..."
	@git submodule update --init --recursive
	@echo "Configuring and Building LevelDB via CMake..."
	@mkdir -p build
	@cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 ..
	@cmake --build build -j

test: build
	@echo "Compiling scan_test..."
	g++ test/scan_test.cpp -Iinclude -Lbuild -lleveldb -lpthread -std=c++17 -o scan_test
	@echo "Running scan_test..."
	./scan_test

clean:
	@echo "Cleaning up build artifacts..."
	rm -rf build scan_test /tmp/stress_test_db
