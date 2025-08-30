/**
 * @file bsp_executor.h
 * @brief BSP (Bulk-Synchronous Parallel) execution framework for RTL simulation
 */

#ifndef BSP_EXECUTOR_H
#define BSP_EXECUTOR_H

#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <random>
#include <cstring>

// Cache line size for x86 architecture
constexpr size_t CACHE_LINE_SIZE = 64;

// Macro for cache line alignment
#define CACHE_ALIGNED alignas(CACHE_LINE_SIZE)

// Forward declarations
class ThreadLocalStorage;
class WorkStealingQueue;
class Partition;

/**
 * @brief NUMA-aware spin barrier for high-performance synchronization
 */
class NumaSpinBarrier {
private:
    struct CACHE_ALIGNED BarrierNode {
        std::atomic<int> count{0};
        std::atomic<int> generation{0};
        char padding[CACHE_LINE_SIZE - 2 * sizeof(std::atomic<int>)];
    };

    const int num_threads_;
    std::vector<std::unique_ptr<BarrierNode>> nodes_;
    CACHE_ALIGNED std::atomic<int> global_generation_{0};
    
public:
    explicit NumaSpinBarrier(int num_threads);
    void wait(int thread_id);
};

/**
 * @brief Thread-local storage for BSP computation
 */
class CACHE_ALIGNED ThreadLocalStorage {
public:
    // Local copy of register states computed in this superstep
    std::vector<uint64_t> local_registers;
    
    // Partition assigned to this thread
    std::unique_ptr<Partition> partition;
    
    // Work-stealing queue for dynamic load balancing
    std::unique_ptr<WorkStealingQueue> work_queue;
    
    // Thread ID and NUMA node
    int thread_id;
    int numa_node;
    
    // Performance counters
    uint64_t computation_cycles{0};
    uint64_t sync_cycles{0};
    uint64_t steal_attempts{0};
    uint64_t steal_successes{0};
    
    // Padding to avoid false sharing
    char padding[CACHE_LINE_SIZE];
    
    ThreadLocalStorage(int tid, size_t num_registers);
};

/**
 * @brief Chase-Lev work-stealing deque implementation
 */
class WorkStealingQueue {
private:
    struct Task {
        std::function<void()> func;
        int priority;
    };
    
    std::atomic<int64_t> top_{0};
    std::atomic<int64_t> bottom_{0};
    std::vector<std::atomic<Task*>> buffer_;
    static constexpr size_t INITIAL_SIZE = 256;
    
public:
    WorkStealingQueue();
    ~WorkStealingQueue();
    
    // Push task (owner thread only)
    void push(std::function<void()> task, int priority = 0);
    
    // Pop task (owner thread only)
    bool pop(std::function<void()>& task);
    
    // Steal task (thief threads)
    bool steal(std::function<void()>& task);
    
    // Check if empty
    bool empty() const;
};

/**
 * @brief Partition represents a subset of the circuit assigned to a thread
 */
class Partition {
public:
    int id;
    std::vector<int> node_ids;  // IDs of nodes in this partition
    std::vector<int> register_ids;  // Register IDs this partition updates
    std::vector<int> replicated_logic;  // IDs of replicated combinational logic
    
    // Communication info
    std::vector<int> input_registers;  // Registers read from other partitions
    std::vector<int> output_registers; // Registers written by this partition
    
    // Cost model for load balancing
    double computational_cost;
    double communication_cost;
    
    Partition(int partition_id) : id(partition_id), computational_cost(0), communication_cost(0) {}
};

/**
 * @brief BSP executor for parallel RTL simulation
 */
class BspExecutor {
private:
    // Number of worker threads
    const int num_threads_;
    
    // Worker threads
    std::vector<std::thread> workers_;
    
    // Thread-local storage for each worker
    std::vector<std::unique_ptr<ThreadLocalStorage>> thread_locals_;
    
    // Global register state (aligned to cache line)
    CACHE_ALIGNED std::vector<uint64_t> global_registers_;
    
    // Synchronization barrier
    std::unique_ptr<NumaSpinBarrier> barrier_;
    
    // Control flags
    std::atomic<bool> running_{false};
    std::atomic<bool> terminate_{false};
    
    // Current simulation cycle
    std::atomic<uint64_t> current_cycle_{0};
    
    // Random number generators for work stealing
    thread_local static std::mt19937 rng_;
    
    // Worker thread main loop
    void workerLoop(int thread_id);
    
    // BSP superstep phases
    void computationPhase(ThreadLocalStorage& tls);
    void communicationPhase(ThreadLocalStorage& tls);
    
    // Work stealing
    bool tryStealWork(ThreadLocalStorage& tls);
    
public:
    explicit BspExecutor(int num_threads, size_t num_registers);
    ~BspExecutor();
    
    // Initialize partitions (called after graph partitioning)
    void initializePartitions(std::vector<std::unique_ptr<Partition>> partitions);
    
    // Start worker threads
    void start();
    
    // Stop worker threads
    void stop();
    
    // Execute one simulation cycle
    void executeCycle();
    
    // Get global register state
    const std::vector<uint64_t>& getRegisterState() const { return global_registers_; }
    
    // Set register value
    void setRegister(size_t idx, uint64_t value);
    
    // Get performance statistics
    void printStatistics() const;
};

#endif // BSP_EXECUTOR_H