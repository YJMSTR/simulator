/**
 * @file hypergraph_partitioner.h
 * @brief Hypergraph partitioning for BSP-based RTL simulation
 */

#ifndef HYPERGRAPH_PARTITIONER_H
#define HYPERGRAPH_PARTITIONER_H

#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "graph.h"
#include "Node.h"
#include "bsp_executor.h"

// Forward declaration
struct HypergraphData;

/**
 * @brief Cost model for accurate load balancing
 */
class CostModel {
private:
    // Linear regression coefficients for different operation types
    std::unordered_map<NodeType, double> op_costs_;
    double bit_width_factor_;
    double base_cost_;
    
public:
    CostModel();
    
    // Train the model using profiling data
    void train(const std::vector<std::pair<Node*, double>>& training_data);
    
    // Predict computational cost for a node
    double predict(const Node* node) const;
    
    // Load/save model parameters
    void loadFromFile(const std::string& filename);
    void saveToFile(const std::string& filename) const;
};

/**
 * @brief Hypergraph representation of RTL design
 */
class Hypergraph {
public:
    // Vertices (logic gates/nodes)
    std::vector<int> vertex_weights;  // Computational cost of each vertex
    std::vector<Node*> vertex_to_node;  // Mapping from vertex ID to Node
    std::unordered_map<Node*, int> node_to_vertex;  // Reverse mapping
    
    // Hyperedges (signal nets)
    std::vector<std::vector<int>> hyperedges;  // Each hyperedge is a list of vertices
    std::vector<int> hyperedge_weights;  // Communication cost of each hyperedge
    
    // Register splitting for timing
    std::unordered_map<Node*, std::pair<int, int>> register_split;  // reg -> (read_vertex, write_vertex)
    
    // Build hypergraph from circuit graph
    void buildFromGraph(graph* g, const CostModel& cost_model);
    
    // Get number of vertices and hyperedges
    int numVertices() const { return vertex_weights.size(); }
    int numHyperedges() const { return hyperedges.size(); }
    
    // Validate hypergraph construction
    bool validate() const;
};

/**
 * @brief Result of hypergraph partitioning
 */
struct PartitionResult {
    std::vector<int> vertex_partition;  // Partition ID for each vertex
    int num_partitions;
    double imbalance;  // Load imbalance factor
    int cut_size;  // Number of cut hyperedges
    
    // Metrics for each partition
    std::vector<double> partition_weights;
    std::vector<int> partition_sizes;
    std::vector<std::unordered_set<int>> cut_hyperedges_per_partition;
};

/**
 * @brief Hypergraph partitioner using KaHyPar
 */
class HypergraphPartitioner {
private:
    std::unique_ptr<CostModel> cost_model_;
    std::unique_ptr<Hypergraph> hypergraph_;
    
    // KaHyPar context (opaque pointer to avoid exposing KaHyPar headers)
    void* kahypar_context_;
    
    // Configuration
    struct Config {
        int num_partitions;
        double imbalance_factor;  // e.g., 1.03 for 3% imbalance
        int seed;
        bool use_mt_kahypar;  // Use multi-threaded version
        std::string preset;  // "quality", "balanced", "speed"
    } config_;
    
public:
    HypergraphPartitioner();
    ~HypergraphPartitioner();
    
    // Configure partitioner
    void configure(int num_partitions, double imbalance = 1.03, 
                  const std::string& preset = "quality", bool use_mt = true);
    
    // Set cost model
    void setCostModel(std::unique_ptr<CostModel> model);
    
    // Build hypergraph from circuit
    void buildHypergraph(graph* g);
    
    // Perform partitioning
    PartitionResult partition();
    
    // Get the hypergraph (for replication analysis)
    const Hypergraph& getHypergraph() const { return *hypergraph_; }
};

/**
 * @brief Replication-aided partitioning (RepCut)
 */
class ReplicationOptimizer {
private:
    const Hypergraph& hypergraph_;
    const PartitionResult& partition_result_;
    
    // Replication decisions
    std::unordered_map<int, std::unordered_set<int>> vertex_replicas_;  // vertex -> set of partitions
    
    // Analysis results
    struct ReplicationStats {
        int total_replicated_nodes;
        double replication_overhead;  // Percentage of additional computation
        int dependencies_eliminated;
        std::vector<int> replicas_per_partition;
    } stats_;
    
    // Find combinational logic cones that cross partition boundaries
    std::vector<std::vector<int>> findCrossingCones();
    
    // Evaluate replication cost vs benefit
    double evaluateReplicationBenefit(const std::vector<int>& cone, 
                                     const std::unordered_set<int>& target_partitions);
    
public:
    ReplicationOptimizer(const Hypergraph& hg, const PartitionResult& pr);
    
    // Analyze and perform replication
    void optimize(double max_overhead = 0.2);  // Max 20% computation overhead
    
    // Get replication decisions
    const auto& getReplicationMap() const { return vertex_replicas_; }
    
    // Get statistics
    const ReplicationStats& getStats() const { return stats_; }
    
    // Apply replication to create final partitions
    std::vector<std::unique_ptr<Partition>> createFinalPartitions();
};

/**
 * @brief Complete partitioning pipeline
 */
class PartitioningPipeline {
private:
    std::unique_ptr<HypergraphPartitioner> partitioner_;
    std::unique_ptr<ReplicationOptimizer> replicator_;
    graph* circuit_graph_;
    
public:
    PartitioningPipeline(graph* g);
    
    // Configure pipeline
    void configure(int num_threads, const std::string& cost_model_file = "");
    
    // Run complete partitioning pipeline
    std::vector<std::unique_ptr<Partition>> run();
    
    // Get detailed statistics
    void printStatistics() const;
};

#endif // HYPERGRAPH_PARTITIONER_H