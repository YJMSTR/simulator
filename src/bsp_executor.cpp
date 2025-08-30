/**
 * @file bsp_executor.cpp
 * @brief Implementation of BSP executor for parallel RTL simulation
 */

#include "bsp_executor.h"
#include "bsp_profiler.h"
#include <numa.h>
#include <sched.h>
#include <chrono>
#include <iostream>
#include <iomanip>

// Thread-local random number generator
thread_local std::mt19937 BspExecutor::rng_(std::chrono::steady_clock::now().time_since_epoch().count());

// NUMA Spin Barrier Implementation
NumaSpinBarrier::NumaSpinBarrier(int num_threads) : num_threads_(num_threads) {
    // Create hierarchical barrier nodes
    int nodes_per_socket = 4; // Assuming 4 cores per NUMA node
    int num_nodes = (num_threads + nodes_per_socket - 1) / nodes_per_socket;
    
    nodes_.reserve(num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
        nodes_.emplace_back(std::make_unique<BarrierNode>());
    }
}

void NumaSpinBarrier::wait(int thread_id) {
    // Two-level barrier: first within NUMA node, then global
    int node_id = thread_id / 4; // Assuming 4 threads per NUMA node
    int local_id = thread_id % 4;
    
    auto& node = *nodes_[node_id];
    int gen = global_generation_.load(std::memory_order_acquire);
    
    // Local barrier within NUMA node
    int count = node.count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count == 4 || (node_id == nodes_.size() - 1 && count == num_threads_ % 4)) {
        // Last thread in node, participate in global barrier
        node.count.store(0, std::memory_order_release);
        
        // Global barrier
        if (global_generation_.compare_exchange_strong(gen, gen + 1, std::memory_order_acq_rel)) {
            // Last thread globally, wake everyone
            for (auto& n : nodes_) {
                n->generation.store(gen + 1, std::memory_order_release);
            }
        } else {
            // Wait for global completion
            while (node.generation.load(std::memory_order_acquire) != gen + 1) {
                __builtin_ia32_pause(); // x86 pause instruction
            }
        }
    } else {
        // Wait for local node completion
        while (node.generation.load(std::memory_order_acquire) != gen + 1) {
            __builtin_ia32_pause();
        }
    }
}

// Thread Local Storage Implementation
ThreadLocalStorage::ThreadLocalStorage(int tid, size_t num_registers) 
    : thread_id(tid), numa_node(numa_node_of_cpu(tid)) {
    local_registers.resize(num_registers);
    work_queue = std::make_unique<WorkStealingQueue>();
}

// Work Stealing Queue Implementation
WorkStealingQueue::WorkStealingQueue() : buffer_(INITIAL_SIZE) {}

WorkStealingQueue::~WorkStealingQueue() {
    // Clean up any remaining tasks
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_relaxed);
    for (int64_t i = t; i < b; ++i) {
        delete buffer_[i & (buffer_.size() - 1)].load(std::memory_order_relaxed);
    }
}

void WorkStealingQueue::push(std::function<void()> task, int priority) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    
    if (b - t >= static_cast<int64_t>(buffer_.size() - 1)) {
        // Buffer full, need to resize (simplified version, real implementation would be more complex)
        return;
    }
    
    Task* new_task = new Task{std::move(task), priority};
    buffer_[b & (buffer_.size() - 1)].store(new_task, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
}

bool WorkStealingQueue::pop(std::function<void()>& task) {
    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top_.load(std::memory_order_relaxed);
    
    if (t <= b) {
        // Non-empty queue
        Task* t_ptr = buffer_[b & (buffer_.size() - 1)].load(std::memory_order_relaxed);
        if (t == b) {
            // Last element, need CAS
            if (!top_.compare_exchange_strong(t, t + 1, 
                                             std::memory_order_seq_cst, 
                                             std::memory_order_relaxed)) {
                bottom_.store(b + 1, std::memory_order_relaxed);
                return false;
            }
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        task = std::move(t_ptr->func);
        delete t_ptr;
        return true;
    } else {
        // Empty queue
        bottom_.store(b + 1, std::memory_order_relaxed);
        return false;
    }
}

bool WorkStealingQueue::steal(std::function<void()>& task) {
    int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = bottom_.load(std::memory_order_acquire);
    
    if (t < b) {
        Task* t_ptr = buffer_[t & (buffer_.size() - 1)].load(std::memory_order_relaxed);
        if (!top_.compare_exchange_strong(t, t + 1,
                                         std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
            return false;
        }
        task = std::move(t_ptr->func);
        delete t_ptr;
        return true;
    }
    return false;
}

bool WorkStealingQueue::empty() const {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_relaxed);
    return b <= t;
}

// BSP Executor Implementation
BspExecutor::BspExecutor(int num_threads, size_t num_registers) 
    : num_threads_(num_threads), global_registers_(num_registers) {
    
    // Initialize NUMA
    if (numa_available() == -1) {
        std::cerr << "Warning: NUMA not available, performance may be suboptimal\n";
    }
    
    // Create thread-local storage
    thread_locals_.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_locals_.emplace_back(std::make_unique<ThreadLocalStorage>(i, num_registers));
    }
    
    // Create synchronization barrier
    barrier_ = std::make_unique<NumaSpinBarrier>(num_threads);
}

BspExecutor::~BspExecutor() {
    stop();
}

void BspExecutor::initializePartitions(std::vector<std::unique_ptr<Partition>> partitions) {
    if (partitions.size() != static_cast<size_t>(num_threads_)) {
        throw std::runtime_error("Number of partitions must match number of threads");
    }
    
    for (int i = 0; i < num_threads_; ++i) {
        thread_locals_[i]->partition = std::move(partitions[i]);
    }
}

void BspExecutor::start() {
    if (running_.load()) return;
    
    running_.store(true);
    terminate_.store(false);
    
    // Start worker threads
    for (int i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(&BspExecutor::workerLoop, this, i);
        
        // Pin thread to CPU
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(workers_.back().native_handle(), sizeof(cpu_set_t), &cpuset);
    }
}

void BspExecutor::stop() {
    if (!running_.load()) return;
    
    terminate_.store(true);
    
    // Wait for all threads to finish
    for (auto& worker : workers_) {
        worker.join();
    }
    
    workers_.clear();
    running_.store(false);
}

void BspExecutor::workerLoop(int thread_id) {
    auto& tls = *thread_locals_[thread_id];
    
#ifdef BSP_PROFILING
    auto profiler = BspProfiler::getInstance();
    auto phase_profiler = profiler ? profiler->getPhaseProfiler() : nullptr;
    auto imbalance_detector = profiler ? profiler->getImbalanceDetector() : nullptr;
#endif
    
    while (!terminate_.load(std::memory_order_relaxed)) {
        Timer phase_timer;
        
        // Phase 1: Computation
#ifdef BSP_PROFILING
        BSP_PROFILE_PHASE(thread_id, Computation, {
            computationPhase(tls);
        });
        auto comp_time = phase_timer.elapsed_us();
        if (imbalance_detector) {
            imbalance_detector->recordWork(thread_id, static_cast<uint64_t>(comp_time));
        }
#else
        computationPhase(tls);
#endif
        
        // Phase 2: Synchronization
        phase_timer.reset();
#ifdef BSP_PROFILING
        BSP_PROFILE_PHASE(thread_id, Synchronization, {
            barrier_->wait(thread_id);
        });
        auto sync_time = phase_timer.elapsed_us();
        if (imbalance_detector) {
            imbalance_detector->recordSyncWait(thread_id, static_cast<uint64_t>(sync_time));
        }
#else
        barrier_->wait(thread_id);
#endif
        
        // Phase 3: Communication
        phase_timer.reset();
#ifdef BSP_PROFILING
        BSP_PROFILE_PHASE(thread_id, Communication, {
            communicationPhase(tls);
        });
#else
        communicationPhase(tls);
#endif
        
        // Update performance counters
        tls.computation_cycles += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - 
            std::chrono::high_resolution_clock::now()).count();
        
        // Synchronize before next cycle
        barrier_->wait(thread_id);
        
        // Only thread 0 increments the cycle counter
        if (thread_id == 0) {
            current_cycle_.fetch_add(1, std::memory_order_relaxed);
#ifdef BSP_PROFILING
            if (phase_profiler) {
                phase_profiler->incrementCycle();
            }
#endif
        }
    }
}

void BspExecutor::computationPhase(ThreadLocalStorage& tls) {
    if (!tls.partition) return;
    
    // Execute assigned partition's computation
    // This is where the actual RTL simulation logic would go
    // For now, it's a placeholder
    
    // Process local work queue
    std::function<void()> task;
    while (tls.work_queue->pop(task)) {
        task();
    }
    
    // Try work stealing if idle
    while (tryStealWork(tls)) {
        // Successfully stole work, continue processing
    }
}

bool BspExecutor::tryStealWork(ThreadLocalStorage& tls) {
#ifdef BSP_PROFILING
    Timer steal_timer;
#endif
    
    // Random victim selection
    std::uniform_int_distribution<int> dist(0, num_threads_ - 1);
    int victim = dist(rng_);
    
    // Don't steal from self
    if (victim == tls.thread_id) {
        victim = (victim + 1) % num_threads_;
    }
    
    tls.steal_attempts++;
    
    std::function<void()> task;
    bool success = thread_locals_[victim]->work_queue->steal(task);
    
#ifdef BSP_PROFILING
    auto profiler = BspProfiler::getInstance();
    if (profiler && profiler->getPhaseProfiler()) {
        profiler->getPhaseProfiler()->recordWorkStealing(tls.thread_id, steal_timer.elapsed_us());
        if (success) {
            profiler->getPhaseProfiler()->recordTaskStolen(tls.thread_id);
        }
    }
#endif
    
    if (success) {
        tls.steal_successes++;
        task();
        return true;
    }
    
    return false;
}

void BspExecutor::communicationPhase(ThreadLocalStorage& tls) {
    if (!tls.partition) return;
    
    // Write local register values to global state
    for (int reg_id : tls.partition->output_registers) {
        global_registers_[reg_id] = tls.local_registers[reg_id];
    }
}

void BspExecutor::executeCycle() {
    // This method would be called from the main simulation loop
    // The actual work is done by the worker threads
}

void BspExecutor::setRegister(size_t idx, uint64_t value) {
    if (idx < global_registers_.size()) {
        global_registers_[idx] = value;
    }
}

void BspExecutor::printStatistics() const {
    std::cout << "\n=== BSP Executor Statistics ===\n";
    std::cout << "Current cycle: " << current_cycle_.load() << "\n\n";
    
    uint64_t total_comp_cycles = 0;
    uint64_t total_sync_cycles = 0;
    uint64_t total_steal_attempts = 0;
    uint64_t total_steal_successes = 0;
    
    for (int i = 0; i < num_threads_; ++i) {
        const auto& tls = *thread_locals_[i];
        std::cout << "Thread " << i << " (NUMA " << tls.numa_node << "):\n";
        std::cout << "  Computation: " << tls.computation_cycles / 1e6 << " ms\n";
        std::cout << "  Sync: " << tls.sync_cycles / 1e6 << " ms\n";
        std::cout << "  Steal attempts: " << tls.steal_attempts << "\n";
        std::cout << "  Steal successes: " << tls.steal_successes << "\n";
        
        total_comp_cycles += tls.computation_cycles;
        total_sync_cycles += tls.sync_cycles;
        total_steal_attempts += tls.steal_attempts;
        total_steal_successes += tls.steal_successes;
    }
    
    std::cout << "\nTotal:\n";
    std::cout << "  Avg computation: " << (total_comp_cycles / num_threads_) / 1e6 << " ms\n";
    std::cout << "  Avg sync: " << (total_sync_cycles / num_threads_) / 1e6 << " ms\n";
    std::cout << "  Steal success rate: " << std::fixed << std::setprecision(2) 
              << (total_steal_attempts > 0 ? 100.0 * total_steal_successes / total_steal_attempts : 0.0) 
              << "%\n";
}