#!/bin/bash
# BSP Implementation Test Script

set -e

echo "BSP Implementation Test Suite"
echo "============================="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test configuration
TEST_DESIGN="${1:-ready-to-run/bin/microbench-rocket.bin}"
TEST_CYCLES=10000
THREAD_COUNTS="1 2 4 8"

# Function to run a test
run_test() {
    local test_name=$1
    local command=$2
    
    echo -ne "Running $test_name... "
    
    if eval "$command" > /dev/null 2>&1; then
        echo -e "${GREEN}PASSED${NC}"
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        return 1
    fi
}

# Function to measure performance
measure_performance() {
    local threads=$1
    local output=$(./build/gsim --bsp --threads=$threads $TEST_DESIGN 2>&1 | grep "cycles/sec" | awk '{print $NF}')
    echo $output
}

echo ""
echo "1. Checking build dependencies..."
echo "---------------------------------"

# Check for required libraries
run_test "NUMA library" "ldconfig -p | grep libnuma"
run_test "pthread support" "ldconfig -p | grep libpthread"

echo ""
echo "2. Building BSP implementation..."
echo "---------------------------------"

# Clean build
make clean > /dev/null 2>&1

# Build without BSP
run_test "Standard build" "make -j$(nproc)"

# Build with BSP
run_test "BSP build" "make clean && make BSP=1 -j$(nproc)"

# Build with profiling
run_test "BSP profiling build" "make clean && make BSP=1 BSP_PROFILING=1 -j$(nproc)"

echo ""
echo "3. Functional tests..."
echo "----------------------"

# Test basic execution
run_test "Single-threaded BSP" "./build/gsim --bsp --threads=1 $TEST_DESIGN"
run_test "Multi-threaded BSP (4 threads)" "./build/gsim --bsp --threads=4 $TEST_DESIGN"
run_test "BSP with profiling" "BSP_PROFILING=1 ./build/gsim --bsp --threads=4 --profile $TEST_DESIGN"

echo ""
echo "4. Performance scaling test..."
echo "------------------------------"

echo -e "Threads\tCycles/sec\tSpeedup"
echo "--------------------------------"

baseline=""
for threads in $THREAD_COUNTS; do
    perf=$(measure_performance $threads)
    
    if [ -z "$baseline" ]; then
        baseline=$perf
        speedup="1.00"
    else
        speedup=$(echo "scale=2; $perf / $baseline" | bc)
    fi
    
    echo -e "$threads\t$perf\t$speedup"
done

echo ""
echo "5. Stress tests..."
echo "------------------"

# Test with different partition sizes
for size in 20 35 50; do
    run_test "Partition size $size" "./build/gsim --bsp --threads=4 --supernode-max-size=$size $TEST_DESIGN"
done

# Test work stealing under imbalance
run_test "Work stealing stress" "BSP_NUM_THREADS=8 ./build/gsim --bsp $TEST_DESIGN"

echo ""
echo "6. Memory and synchronization tests..."
echo "--------------------------------------"

# Check for memory leaks (requires valgrind)
if command -v valgrind &> /dev/null; then
    run_test "Memory leak check" "valgrind --leak-check=quick --error-exitcode=1 ./build/gsim --bsp --threads=2 $TEST_DESIGN"
else
    echo -e "${YELLOW}Skipping memory leak test (valgrind not found)${NC}"
fi

# Test false sharing detection
run_test "False sharing detection" "BSP_PROFILING=1 ./build/gsim --bsp --threads=4 $TEST_DESIGN 2>&1 | grep -q 'False sharing'"

echo ""
echo "7. Integration tests..."
echo "-----------------------"

# Test with different designs if available
for design in "NutShell" "rocket" "boom"; do
    if [ -f "ready-to-run/bin/microbench-$design.bin" ]; then
        run_test "$design design" "./build/gsim --bsp --threads=4 ready-to-run/bin/microbench-$design.bin"
    fi
done

echo ""
echo "Test Summary"
echo "============"
echo -e "${GREEN}All tests completed!${NC}"
echo ""
echo "Performance Notes:"
echo "- Best thread count for this system: $(nproc)"
echo "- NUMA nodes detected: $(numactl --show | grep nodebind | wc -w)"
echo "- Cache line size: $(getconf LEVEL1_DCACHE_LINESIZE) bytes"
echo ""
echo "To run more detailed benchmarks:"
echo "  make bsp-benchmark"
echo ""
echo "To analyze a specific run:"
echo "  BSP_PROFILING=1 ./build/gsim --bsp --threads=$(nproc) $TEST_DESIGN"
echo "  # Check generated profile files in current directory"