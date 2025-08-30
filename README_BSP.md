# BSP-based Parallel RTL Simulation

This project implements a high-performance parallel RTL simulator based on the Bulk-Synchronous Parallel (BSP) model. The BSP implementation provides significant speedup for large-scale RTL designs through intelligent partitioning, replication optimization, and efficient synchronization.

## Features

### 1. BSP Model Implementation
- **Three-phase execution**: Computation → Synchronization → Communication
- **Thread-local storage** with cache-line alignment to eliminate false sharing
- **NUMA-aware spin barriers** for efficient synchronization
- **Lock-free work stealing** for dynamic load balancing

### 2. Advanced Partitioning
- **Hypergraph partitioning** using KaHyPar for optimal load balance
- **Replication-aided partitioning (RepCut)** to eliminate intra-cycle dependencies
- **Cost model-based load prediction** for accurate balancing
- **Automatic detection and replication of crossing logic cones**

### 3. Performance Optimizations
- **Cache-aligned data structures** to prevent false sharing
- **Chase-Lev work-stealing deques** for efficient task distribution
- **NUMA-aware thread pinning** and memory allocation
- **Minimal synchronization overhead** with custom spin barriers

### 4. Profiling and Debugging
- **Comprehensive performance profiling** with per-thread statistics
- **Load imbalance detection** and analysis
- **Memory access pattern analysis**
- **Real-time performance monitoring**

## Building

### Prerequisites
- C++17 compatible compiler (Clang++ recommended)
- NUMA library (`libnuma-dev` on Ubuntu/Debian)
- pthread support
- (Optional) KaHyPar library for hypergraph partitioning

### Compilation

1. **Standard build** (single-threaded mode):
```bash
make
```

2. **BSP-enabled build** (multi-threaded mode):
```bash
make BSP=1
```

3. **BSP build with profiling**:
```bash
make BSP=1 BSP_PROFILING=1
```

4. **Specify thread count**:
```bash
make BSP=1 BSP_THREADS=8
```

## Usage

### Command Line Options

```bash
# Run with BSP mode
./build/gsim --bsp --threads=8 design.fir

# Enable performance profiling
./build/gsim --bsp --threads=8 --profile design.fir

# Set cost model file
BSP_COST_MODEL=cost_model.txt ./build/gsim --bsp design.fir

# Run benchmark across different thread counts
make bsp-benchmark
```

### Environment Variables

- `BSP_NUM_THREADS`: Number of threads (overrides --threads)
- `BSP_COST_MODEL`: Path to cost model file for load balancing
- `BSP_PROFILING`: Enable profiling (1 to enable)

### Example Code

```cpp
// Create BSP-enabled simulator
Smytop sim(8);  // 8 threads

// Reset and initialize
sim.reset();
sim.set_clock(0);
sim.set_reset(1);

// Run simulation
for (int i = 0; i < 1000000; i++) {
    sim.set_clock(0);
    sim.step();
    sim.set_clock(1);
    sim.step();
}

// Print performance statistics
sim.printStats();
```

## Performance Analysis

### Profiling Output

The profiler generates detailed reports including:

1. **Per-thread statistics**:
   - Computation time
   - Synchronization overhead
   - Communication time
   - Work stealing efficiency

2. **Load balance analysis**:
   - Work distribution across threads
   - Idle time analysis
   - Synchronization wait times

3. **Memory access patterns**:
   - Sequential vs. random access ratios
   - False sharing detection
   - Cache performance metrics

### Example Output

```
========== BSP Performance Report ==========
Total simulation cycles: 1000000
Total elapsed time: 1234.56 ms
Average cycle time: 1.235 ms/cycle

Per-Thread Statistics:
  Thread    Computation         Sync    Communication   Work Stealing         Tasks    Cache Hit%
--------------------------------------------------------------------------------------------------------
       0        245.67 ms     12.34 ms        8.91 ms        0.23 ms         12543        95.2%
       1        243.89 ms     13.21 ms        9.02 ms        0.45 ms         12387        94.8%
       2        246.12 ms     12.87 ms        8.76 ms        0.31 ms         12601        95.5%
       3        242.34 ms     13.45 ms        9.13 ms        0.52 ms         12298        94.3%

Work Stealing Statistics:
  Total tasks stolen: 487
  Total steal attempts: 523
  Steal success rate: 93.1%

Load Imbalance Factor: 1.015
```

## Optimization Guidelines

### 1. Partitioning Tuning
- Adjust `--supernode-max-size` for different granularities
- Use cost model training for accurate load prediction
- Monitor replication overhead (should be < 20%)

### 2. Thread Configuration
- Use power-of-2 thread counts for best performance
- Match thread count to physical cores (not hyperthreads)
- Consider NUMA topology when selecting thread count

### 3. Memory Layout
- Ensure sufficient memory for replicated logic
- Monitor false sharing detection in profiler
- Use huge pages for large designs

### 4. Debugging Performance Issues
- Check load imbalance factor (should be < 1.1)
- Monitor synchronization overhead (should be < 5%)
- Analyze work stealing efficiency (> 90% is good)

## Troubleshooting

### High Load Imbalance
- Reduce partition size with smaller supernode max size
- Enable cost model training
- Check for pathological circuit patterns

### Excessive Synchronization Overhead
- Ensure CPU frequency scaling is disabled
- Check for NUMA misconfigurations
- Verify thread pinning is working correctly

### Poor Scaling
- Profile memory access patterns
- Check for false sharing
- Verify replication overhead is reasonable

## Future Improvements

1. **Adaptive partitioning** based on runtime feedback
2. **GPU acceleration** for suitable circuit components
3. **Distributed simulation** across multiple machines
4. **Incremental compilation** for faster turnaround

## Contributing

Please see the main project README for contribution guidelines. BSP-specific contributions should focus on:
- Improving partitioning algorithms
- Optimizing synchronization primitives
- Enhancing profiling capabilities
- Adding new load balancing strategies