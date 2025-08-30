/**
 * @file bsp_profiler.cpp
 * @brief Implementation of BSP profiling and debugging tools
 */

#include "bsp_profiler.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>

// Initialize static member
BspProfiler* BspProfiler::instance_ = nullptr;

// BspPhaseProfiler Implementation
void BspPhaseProfiler::printReport(std::ostream& out) const {
    out << "\n========== BSP Performance Report ==========\n";
    out << "Total simulation cycles: " << total_cycles_.load() << "\n";
    out << "Total elapsed time: " << std::fixed << std::setprecision(2) 
        << global_timer_.elapsed_ms() << " ms\n";
    out << "Average cycle time: " << std::fixed << std::setprecision(3)
        << (total_cycles_.load() > 0 ? global_timer_.elapsed_ms() / total_cycles_.load() : 0) 
        << " ms/cycle\n\n";
    
    // Per-thread statistics
    out << "Per-Thread Statistics:\n";
    out << std::setw(8) << "Thread" 
        << std::setw(15) << "Computation"
        << std::setw(15) << "Sync"
        << std::setw(15) << "Communication"
        << std::setw(15) << "Work Stealing"
        << std::setw(15) << "Tasks"
        << std::setw(15) << "Cache Hit%"
        << "\n";
    out << std::string(113, '-') << "\n";
    
    for (size_t i = 0; i < thread_profiles_.size(); ++i) {
        const auto& profile = thread_profiles_[i];
        
        auto comp_stats = profile.computation.getStats();
        auto sync_stats = profile.synchronization.getStats();
        auto comm_stats = profile.communication.getStats();
        auto steal_stats = profile.work_stealing.getStats();
        
        uint64_t total_cache_accesses = profile.cache_hits + profile.cache_misses;
        double cache_hit_rate = (total_cache_accesses > 0) ? 
            (100.0 * profile.cache_hits / total_cache_accesses) : 0.0;
        
        out << std::setw(8) << i
            << std::setw(15) << std::fixed << std::setprecision(2) << comp_stats.total_ms << " ms"
            << std::setw(15) << sync_stats.total_ms << " ms"
            << std::setw(15) << comm_stats.total_ms << " ms"
            << std::setw(15) << steal_stats.total_ms << " ms"
            << std::setw(15) << profile.tasks_executed.load()
            << std::setw(14) << std::fixed << std::setprecision(1) << cache_hit_rate << "%"
            << "\n";
    }
    
    // Phase breakdown
    out << "\nPhase Breakdown (average per cycle):\n";
    double total_comp_ms = 0, total_sync_ms = 0, total_comm_ms = 0;
    
    for (const auto& profile : thread_profiles_) {
        total_comp_ms += profile.computation.getStats().total_ms;
        total_sync_ms += profile.synchronization.getStats().total_ms;
        total_comm_ms += profile.communication.getStats().total_ms;
    }
    
    double cycles = static_cast<double>(total_cycles_.load());
    if (cycles > 0) {
        out << "  Computation: " << std::fixed << std::setprecision(3) 
            << (total_comp_ms / cycles / thread_profiles_.size()) << " ms\n";
        out << "  Synchronization: " << (total_sync_ms / cycles / thread_profiles_.size()) << " ms\n";
        out << "  Communication: " << (total_comm_ms / cycles / thread_profiles_.size()) << " ms\n";
    }
    
    // Work stealing statistics
    out << "\nWork Stealing Statistics:\n";
    uint64_t total_stolen = 0;
    uint64_t total_steal_attempts = 0;
    
    for (size_t i = 0; i < thread_profiles_.size(); ++i) {
        const auto& profile = thread_profiles_[i];
        total_stolen += profile.tasks_stolen.load();
        total_steal_attempts += profile.work_stealing.getStats().count;
    }
    
    out << "  Total tasks stolen: " << total_stolen << "\n";
    out << "  Total steal attempts: " << total_steal_attempts << "\n";
    out << "  Steal success rate: " << std::fixed << std::setprecision(1)
        << (total_steal_attempts > 0 ? 100.0 * total_stolen / total_steal_attempts : 0.0) << "%\n";
    
    out << "============================================\n\n";
}

void BspPhaseProfiler::saveReport(const std::string& filename) const {
    std::ofstream file(filename);
    if (file.is_open()) {
        printReport(file);
        file.close();
    }
}

void BspPhaseProfiler::generateCSV(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) return;
    
    // CSV header
    file << "Thread,Computation_ms,Sync_ms,Communication_ms,WorkStealing_ms,"
         << "Tasks_Executed,Tasks_Stolen,Cache_Hits,Cache_Misses,Cache_Hit_Rate\n";
    
    // Data rows
    for (size_t i = 0; i < thread_profiles_.size(); ++i) {
        const auto& profile = thread_profiles_[i];
        
        auto comp_stats = profile.computation.getStats();
        auto sync_stats = profile.synchronization.getStats();
        auto comm_stats = profile.communication.getStats();
        auto steal_stats = profile.work_stealing.getStats();
        
        uint64_t hits = profile.cache_hits.load();
        uint64_t misses = profile.cache_misses.load();
        uint64_t total = hits + misses;
        double hit_rate = (total > 0) ? (100.0 * hits / total) : 0.0;
        
        file << i << ","
             << comp_stats.total_ms << ","
             << sync_stats.total_ms << ","
             << comm_stats.total_ms << ","
             << steal_stats.total_ms << ","
             << profile.tasks_executed.load() << ","
             << profile.tasks_stolen.load() << ","
             << hits << ","
             << misses << ","
             << hit_rate << "\n";
    }
    
    file.close();
}

void BspPhaseProfiler::printRealTimeStats(int interval_ms) const {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        
        // Clear screen (works on Unix-like systems)
        std::cout << "\033[2J\033[1;1H";
        
        std::cout << "=== BSP Real-Time Monitor ===\n";
        std::cout << "Cycles: " << total_cycles_.load() 
                  << " | Elapsed: " << global_timer_.elapsed_ms() << " ms\n\n";
        
        std::cout << std::setw(6) << "Thread" 
                  << std::setw(12) << "Comp(ms)" 
                  << std::setw(12) << "Tasks"
                  << std::setw(12) << "Stolen\n";
        
        for (size_t i = 0; i < thread_profiles_.size(); ++i) {
            const auto& profile = thread_profiles_[i];
            std::cout << std::setw(6) << i
                     << std::setw(12) << profile.computation.getStats().total_ms
                     << std::setw(12) << profile.tasks_executed.load()
                     << std::setw(12) << profile.tasks_stolen.load()
                     << "\n";
        }
        
        std::cout << std::flush;
    }
}

// MemoryPatternAnalyzer Implementation
void MemoryPatternAnalyzer::recordAccess(int thread_id, void* addr, size_t size, bool is_write) {
    // Simplified implementation - in production would track actual patterns
    static thread_local void* last_addr = nullptr;
    static thread_local size_t sequential_count = 0;
    
    if (last_addr && (char*)addr == (char*)last_addr + size) {
        sequential_count++;
        if (sequential_count > 10) {
            thread_patterns_[thread_id].sequential_accesses.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        thread_patterns_[thread_id].random_accesses.fetch_add(1, std::memory_order_relaxed);
        sequential_count = 0;
    }
    
    last_addr = addr;
}

void MemoryPatternAnalyzer::detectFalseSharing(int thread_id, void* addr) {
    // Check if address is in same cache line as other threads' recent accesses
    // Simplified implementation
    thread_patterns_[thread_id].false_sharing_detected.fetch_add(1, std::memory_order_relaxed);
}

void MemoryPatternAnalyzer::printAnalysis() const {
    std::cout << "\n=== Memory Access Pattern Analysis ===\n";
    
    for (size_t i = 0; i < thread_patterns_.size(); ++i) {
        const auto& pattern = thread_patterns_[i];
        uint64_t seq = pattern.sequential_accesses.load();
        uint64_t rand = pattern.random_accesses.load();
        uint64_t total = seq + rand;
        
        std::cout << "Thread " << i << ":\n";
        if (total > 0) {
            std::cout << "  Sequential: " << std::fixed << std::setprecision(1) 
                     << (100.0 * seq / total) << "%\n";
            std::cout << "  Random: " << (100.0 * rand / total) << "%\n";
        }
        std::cout << "  False sharing detected: " << pattern.false_sharing_detected.load() 
                 << " times\n";
    }
}

// LoadImbalanceDetector Implementation
double LoadImbalanceDetector::calculateImbalance() const {
    std::vector<uint64_t> work_times;
    for (const auto& stats : thread_stats_) {
        work_times.push_back(stats.computation_time.load());
    }
    
    if (work_times.empty()) return 0.0;
    
    auto [min_it, max_it] = std::minmax_element(work_times.begin(), work_times.end());
    double avg = std::accumulate(work_times.begin(), work_times.end(), 0.0) / work_times.size();
    
    return (avg > 0) ? (*max_it - *min_it) / avg : 0.0;
}

void LoadImbalanceDetector::printImbalanceReport() const {
    std::cout << "\n=== Load Imbalance Report ===\n";
    
    std::vector<double> work_percentages;
    uint64_t total_work = 0;
    
    for (const auto& stats : thread_stats_) {
        total_work += stats.computation_time.load();
    }
    
    if (total_work == 0) {
        std::cout << "No work recorded\n";
        return;
    }
    
    std::cout << "Thread Work Distribution:\n";
    for (size_t i = 0; i < thread_stats_.size(); ++i) {
        const auto& stats = thread_stats_[i];
        uint64_t work = stats.computation_time.load();
        uint64_t idle = stats.idle_time.load();
        uint64_t sync = stats.sync_wait_time.load();
        
        double work_pct = 100.0 * work / total_work;
        work_percentages.push_back(work_pct);
        
        std::cout << "  Thread " << i << ": " 
                 << std::fixed << std::setprecision(1) << work_pct << "% work, "
                 << "idle=" << (idle / 1000) << "ms, "
                 << "sync_wait=" << (sync / 1000) << "ms\n";
    }
    
    double imbalance = calculateImbalance();
    std::cout << "\nImbalance Factor: " << std::fixed << std::setprecision(2) 
             << imbalance << "\n";
    
    if (imbalance > 0.2) {
        std::cout << "WARNING: High load imbalance detected! Consider rebalancing.\n";
    }
}

// PartitionQualityAnalyzer Implementation
void PartitionQualityAnalyzer::analyzeQuality() const {
    std::cout << "\n=== Partition Quality Analysis ===\n";
    std::cout << "Number of partitions: " << num_partitions_ << "\n";
    std::cout << "Edge cuts: " << edge_cuts_ << "\n";
    std::cout << "Replicated nodes: " << replicated_nodes_ << "\n\n";
    
    std::cout << "Partition sizes and costs:\n";
    for (int i = 0; i < num_partitions_; ++i) {
        std::cout << "  Partition " << i << ": "
                 << "size=" << partition_sizes_[i] << ", "
                 << "cost=" << std::fixed << std::setprecision(2) << partition_costs_[i] << "\n";
    }
    
    double balance = calculateLoadBalance();
    std::cout << "\nLoad balance: " << std::fixed << std::setprecision(3) << balance << "\n";
    
    if (balance > 1.1) {
        std::cout << "WARNING: Poor load balance. Consider adjusting partition algorithm.\n";
    }
    
    double comm_volume = calculateCommunicationVolume();
    std::cout << "Communication volume estimate: " << comm_volume << "\n";
}

double PartitionQualityAnalyzer::calculateLoadBalance() const {
    if (partition_costs_.empty()) return 0.0;
    
    double max_cost = *std::max_element(partition_costs_.begin(), partition_costs_.end());
    double avg_cost = std::accumulate(partition_costs_.begin(), partition_costs_.end(), 0.0) 
                      / partition_costs_.size();
    
    return (avg_cost > 0) ? max_cost / avg_cost : 0.0;
}

double PartitionQualityAnalyzer::calculateCommunicationVolume() const {
    // Simplified estimate based on edge cuts and replication
    return edge_cuts_ + 0.5 * replicated_nodes_;
}

// BspProfiler Implementation
void BspProfiler::generateReport(const std::string& prefix) {
    std::string timestamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    
    // Generate phase profiling report
    std::string phase_report = prefix + "_phase_" + timestamp + ".txt";
    phase_profiler_->saveReport(phase_report);
    
    // Generate CSV for further analysis
    std::string csv_report = prefix + "_data_" + timestamp + ".csv";
    phase_profiler_->generateCSV(csv_report);
    
    // Print summary to console
    std::cout << "\n======== BSP Profiling Summary ========\n";
    
    // Phase statistics
    phase_profiler_->printReport(std::cout);
    
    // Memory patterns
    memory_analyzer_->printAnalysis();
    
    // Load imbalance
    imbalance_detector_->printImbalanceReport();
    
    // Partition quality
    partition_analyzer_->analyzeQuality();
    
    std::cout << "\nDetailed reports saved to:\n";
    std::cout << "  " << phase_report << "\n";
    std::cout << "  " << csv_report << "\n";
    std::cout << "=======================================\n";
}