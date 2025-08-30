/**
 * @file bsp_profiler.h
 * @brief Performance profiling and debugging tools for BSP-based RTL simulation
 */

#ifndef BSP_PROFILER_H
#define BSP_PROFILER_H

#include <chrono>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <string>
#include <fstream>
#include <iomanip>
#include <thread>

/**
 * @brief High-resolution timer
 */
class Timer {
private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    
    TimePoint start_time_;
    
public:
    Timer() : start_time_(Clock::now()) {}
    
    void reset() {
        start_time_ = Clock::now();
    }
    
    double elapsed_ms() const {
        auto end_time = Clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_);
        return duration.count() / 1000.0;
    }
    
    double elapsed_us() const {
        auto end_time = Clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_);
        return static_cast<double>(duration.count());
    }
};

/**
 * @brief Thread-safe performance counter
 */
class PerfCounter {
private:
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> total_time_us_{0};
    std::atomic<uint64_t> min_time_us_{UINT64_MAX};
    std::atomic<uint64_t> max_time_us_{0};
    
public:
    void record(double time_us) {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_time_us_.fetch_add(static_cast<uint64_t>(time_us), std::memory_order_relaxed);
        
        // Update min/max (not perfectly accurate but good enough)
        uint64_t time_int = static_cast<uint64_t>(time_us);
        uint64_t current_min = min_time_us_.load(std::memory_order_relaxed);
        while (time_int < current_min && 
               !min_time_us_.compare_exchange_weak(current_min, time_int)) {}
        
        uint64_t current_max = max_time_us_.load(std::memory_order_relaxed);
        while (time_int > current_max && 
               !max_time_us_.compare_exchange_weak(current_max, time_int)) {}
    }
    
    struct Stats {
        uint64_t count;
        double avg_us;
        double min_us;
        double max_us;
        double total_ms;
    };
    
    Stats getStats() const {
        Stats stats;
        stats.count = count_.load();
        stats.total_ms = total_time_us_.load() / 1000.0;
        stats.avg_us = (stats.count > 0) ? (total_time_us_.load() / static_cast<double>(stats.count)) : 0;
        stats.min_us = (stats.count > 0) ? min_time_us_.load() : 0;
        stats.max_us = max_time_us_.load();
        return stats;
    }
};

/**
 * @brief BSP phase profiler
 */
class BspPhaseProfiler {
private:
    struct ThreadProfile {
        PerfCounter computation;
        PerfCounter synchronization;
        PerfCounter communication;
        PerfCounter work_stealing;
        
        // Cache performance
        std::atomic<uint64_t> cache_misses{0};
        std::atomic<uint64_t> cache_hits{0};
        
        // Load balancing
        std::atomic<uint64_t> tasks_executed{0};
        std::atomic<uint64_t> tasks_stolen{0};
    };
    
    std::vector<ThreadProfile> thread_profiles_;
    Timer global_timer_;
    std::atomic<uint64_t> total_cycles_{0};
    
public:
    explicit BspPhaseProfiler(int num_threads) 
        : thread_profiles_(num_threads) {}
    
    // Record phase timing
    void recordComputation(int thread_id, double time_us) {
        thread_profiles_[thread_id].computation.record(time_us);
    }
    
    void recordSynchronization(int thread_id, double time_us) {
        thread_profiles_[thread_id].synchronization.record(time_us);
    }
    
    void recordCommunication(int thread_id, double time_us) {
        thread_profiles_[thread_id].communication.record(time_us);
    }
    
    void recordWorkStealing(int thread_id, double time_us) {
        thread_profiles_[thread_id].work_stealing.record(time_us);
    }
    
    // Record events
    void recordCacheMiss(int thread_id) {
        thread_profiles_[thread_id].cache_misses.fetch_add(1, std::memory_order_relaxed);
    }
    
    void recordCacheHit(int thread_id) {
        thread_profiles_[thread_id].cache_hits.fetch_add(1, std::memory_order_relaxed);
    }
    
    void recordTaskExecuted(int thread_id) {
        thread_profiles_[thread_id].tasks_executed.fetch_add(1, std::memory_order_relaxed);
    }
    
    void recordTaskStolen(int thread_id) {
        thread_profiles_[thread_id].tasks_stolen.fetch_add(1, std::memory_order_relaxed);
    }
    
    void incrementCycle() {
        total_cycles_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Generate report
    void printReport(std::ostream& out = std::cout) const;
    void saveReport(const std::string& filename) const;
    void generateCSV(const std::string& filename) const;
    
    // Real-time monitoring
    void printRealTimeStats(int interval_ms = 1000) const;
};

/**
 * @brief Memory access pattern analyzer
 */
class MemoryPatternAnalyzer {
private:
    struct AccessPattern {
        std::atomic<uint64_t> sequential_accesses{0};
        std::atomic<uint64_t> random_accesses{0};
        std::atomic<uint64_t> false_sharing_detected{0};
        std::atomic<uint64_t> cache_line_transfers{0};
    };
    
    std::vector<AccessPattern> thread_patterns_;
    
public:
    explicit MemoryPatternAnalyzer(int num_threads) 
        : thread_patterns_(num_threads) {}
    
    void recordAccess(int thread_id, void* addr, size_t size, bool is_write);
    void detectFalseSharing(int thread_id, void* addr);
    void printAnalysis() const;
};

/**
 * @brief Load imbalance detector
 */
class LoadImbalanceDetector {
private:
    struct WorkloadStats {
        std::atomic<uint64_t> computation_time{0};
        std::atomic<uint64_t> idle_time{0};
        std::atomic<uint64_t> sync_wait_time{0};
    };
    
    std::vector<WorkloadStats> thread_stats_;
    
public:
    explicit LoadImbalanceDetector(int num_threads) 
        : thread_stats_(num_threads) {}
    
    void recordWork(int thread_id, uint64_t time_us) {
        thread_stats_[thread_id].computation_time.fetch_add(time_us, std::memory_order_relaxed);
    }
    
    void recordIdle(int thread_id, uint64_t time_us) {
        thread_stats_[thread_id].idle_time.fetch_add(time_us, std::memory_order_relaxed);
    }
    
    void recordSyncWait(int thread_id, uint64_t time_us) {
        thread_stats_[thread_id].sync_wait_time.fetch_add(time_us, std::memory_order_relaxed);
    }
    
    double calculateImbalance() const;
    void printImbalanceReport() const;
};

/**
 * @brief Partition quality analyzer
 */
class PartitionQualityAnalyzer {
private:
    int num_partitions_;
    std::vector<int> partition_sizes_;
    std::vector<double> partition_costs_;
    int edge_cuts_;
    int replicated_nodes_;
    
public:
    PartitionQualityAnalyzer(int num_partitions) 
        : num_partitions_(num_partitions), 
          partition_sizes_(num_partitions),
          partition_costs_(num_partitions),
          edge_cuts_(0),
          replicated_nodes_(0) {}
    
    void setPartitionInfo(int partition_id, int size, double cost) {
        partition_sizes_[partition_id] = size;
        partition_costs_[partition_id] = cost;
    }
    
    void setEdgeCuts(int cuts) { edge_cuts_ = cuts; }
    void setReplicatedNodes(int nodes) { replicated_nodes_ = nodes; }
    
    void analyzeQuality() const;
    double calculateLoadBalance() const;
    double calculateCommunicationVolume() const;
};

/**
 * @brief Integrated BSP profiler
 */
class BspProfiler {
private:
    std::unique_ptr<BspPhaseProfiler> phase_profiler_;
    std::unique_ptr<MemoryPatternAnalyzer> memory_analyzer_;
    std::unique_ptr<LoadImbalanceDetector> imbalance_detector_;
    std::unique_ptr<PartitionQualityAnalyzer> partition_analyzer_;
    
    int num_threads_;
    bool profiling_enabled_;
    
    // Singleton instance
    static BspProfiler* instance_;
    
    BspProfiler(int num_threads) 
        : num_threads_(num_threads), profiling_enabled_(true) {
        phase_profiler_ = std::make_unique<BspPhaseProfiler>(num_threads);
        memory_analyzer_ = std::make_unique<MemoryPatternAnalyzer>(num_threads);
        imbalance_detector_ = std::make_unique<LoadImbalanceDetector>(num_threads);
        partition_analyzer_ = std::make_unique<PartitionQualityAnalyzer>(num_threads);
    }
    
public:
    static void initialize(int num_threads) {
        if (!instance_) {
            instance_ = new BspProfiler(num_threads);
        }
    }
    
    static BspProfiler* getInstance() {
        return instance_;
    }
    
    static void cleanup() {
        delete instance_;
        instance_ = nullptr;
    }
    
    // Enable/disable profiling
    void enableProfiling(bool enable) { profiling_enabled_ = enable; }
    bool isProfilingEnabled() const { return profiling_enabled_; }
    
    // Access sub-profilers
    BspPhaseProfiler* getPhaseProfiler() { return phase_profiler_.get(); }
    MemoryPatternAnalyzer* getMemoryAnalyzer() { return memory_analyzer_.get(); }
    LoadImbalanceDetector* getImbalanceDetector() { return imbalance_detector_.get(); }
    PartitionQualityAnalyzer* getPartitionAnalyzer() { return partition_analyzer_.get(); }
    
    // Generate comprehensive report
    void generateReport(const std::string& prefix = "bsp_profile");
};

// Profiling macros
#ifdef BSP_PROFILING
    #define BSP_PROFILE_INIT(num_threads) BspProfiler::initialize(num_threads)
    #define BSP_PROFILE_CLEANUP() BspProfiler::cleanup()
    #define BSP_PROFILE_PHASE(thread_id, phase, code) \
        do { \
            Timer _timer; \
            code; \
            if (BspProfiler::getInstance() && BspProfiler::getInstance()->isProfilingEnabled()) { \
                BspProfiler::getInstance()->getPhaseProfiler()->record##phase(thread_id, _timer.elapsed_us()); \
            } \
        } while(0)
#else
    #define BSP_PROFILE_INIT(num_threads)
    #define BSP_PROFILE_CLEANUP()
    #define BSP_PROFILE_PHASE(thread_id, phase, code) code
#endif

#endif // BSP_PROFILER_H