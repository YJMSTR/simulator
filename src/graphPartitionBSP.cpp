/**
 * @file graphPartitionBSP.cpp
 * @brief BSP-based graph partitioning using hypergraph partitioner
 */

#include "common.h"
#include "hypergraph_partitioner.h"
#include "bsp_executor.h"
#include <iostream>

// Global BSP executor instance
static std::unique_ptr<BspExecutor> g_bsp_executor;
static std::unique_ptr<PartitioningPipeline> g_partitioning_pipeline;

// Override the original graphPartition function
void graph::graphPartition() {
    std::cout << "\n=== BSP-based Graph Partitioning ===\n";
    
    // Get number of threads from environment or default
    int num_threads = std::thread::hardware_concurrency();
    const char* thread_env = std::getenv("BSP_NUM_THREADS");
    if (thread_env) {
        num_threads = std::atoi(thread_env);
    }
    std::cout << "Using " << num_threads << " threads for parallel simulation\n";
    
    // Create partitioning pipeline
    g_partitioning_pipeline = std::make_unique<PartitioningPipeline>(this);
    
    // Configure with cost model if available
    std::string cost_model_file = "";
    const char* model_env = std::getenv("BSP_COST_MODEL");
    if (model_env) {
        cost_model_file = model_env;
    }
    g_partitioning_pipeline->configure(num_threads, cost_model_file);
    
    // Run partitioning pipeline
    auto partitions = g_partitioning_pipeline->run();
    
    // Print statistics
    g_partitioning_pipeline->printStatistics();
    
    // Count total registers for BSP executor
    size_t num_registers = 0;
    for (Node* node : allNodes) {
        if (node->type == NODE_REG_DST) {
            num_registers++;
        }
    }
    
    // Create BSP executor
    g_bsp_executor = std::make_unique<BspExecutor>(num_threads, num_registers);
    g_bsp_executor->initializePartitions(std::move(partitions));
    
    // Store BSP executor pointer in graph for later use
    // Note: In production, this would be properly integrated into the graph class
    
    std::cout << "BSP partitioning complete\n\n";
}

// Getter for BSP executor (to be used by cppEmitter)
BspExecutor* getBspExecutor() {
    return g_bsp_executor.get();
}

// Cleanup function
void cleanupBspResources() {
    g_bsp_executor.reset();
    g_partitioning_pipeline.reset();
}