# Build & Test Instructions

---

## Steps

Open a terminal in the project root directory and run:

```bash
# Step 1: Clean any previous build artefacts
make clean

# Step 2: Build LevelDB and compile + run the test suite
make
```

`make` will automatically:
1. Initialise git submodules (`third_party/googletest`)
2. Configure and build LevelDB via CMake (Release mode, C++17)
3. Compile `test/scan_test.cpp` against the built library
4. Execute the test binary (`./scan_test`)

---

## Expected Output

A successful run prints results for all five test classes followed by:

```
[==========] All tests passed.
```

The full test suite executes **2934 assertions** across:
- Scan edge cases
- DeleteRange edge cases
- ForceFullCompaction edge cases
- Interleaved operations
- 300-round randomised stress test (seed = 42)

ForceFullCompaction calls also print per-call compaction statistics to stdout, for example:

```
=== Manual Full Compaction Statistics ===
Compactions executed: 1
Number of input files: 2
Number of output files: 1
Total bytes read: 1606
Total bytes written: 1061
=========================================
```

---
