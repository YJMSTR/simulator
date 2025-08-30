/**
 * @file hypergraph_partitioner.cpp
 * @brief Implementation of hypergraph partitioning for BSP-based RTL simulation
 */

#include "hypergraph_partitioner.h"
#include "common.h"
#include <algorithm>
#include <numeric>
#include <fstream>
#include <iostream>
#include <queue>
#include <cmath>

// Note: In production, you would include actual KaHyPar headers
// For this implementation, we'll create a simplified interface

// CostModel Implementation
CostModel::CostModel() : bit_width_factor_(1.0), base_cost_(1.0) {
    // Initialize default costs for different node types
    op_costs_[NODE_INVALID] = 0.0;
    op_costs_[NODE_OTHERS] = 1.0;
    op_costs_[NODE_REG_DST] = 2.0;
    op_costs_[NODE_REG_UPDATE] = 2.0;
    op_costs_[NODE_UOP] = 1.0;
    op_costs_[NODE_BOP] = 1.5;
    op_costs_[NODE_TOP] = 2.0;
    op_costs_[NODE_SEL] = 2.5;
    op_costs_[NODE_READER] = 3.0;
    op_costs_[NODE_WRITER] = 3.0;
    op_costs_[NODE_READWRITER] = 4.0;
    op_costs_[NODE_MEMORY] = 5.0;
}

void CostModel::train(const std::vector<std::pair<Node*, double>>& training_data) {
    // Simplified training - in practice, would use linear regression
    if (training_data.empty()) return;
    
    // Calculate average cost per operation type
    std::unordered_map<NodeType, std::vector<double>> costs_by_type;
    
    for (const auto& [node, cost] : training_data) {
        costs_by_type[node->type].push_back(cost / (node->width + 1));
    }
    
    // Update op_costs with averages
    for (auto& [type, costs] : costs_by_type) {
        if (!costs.empty()) {
            double avg = std::accumulate(costs.begin(), costs.end(), 0.0) / costs.size();
            op_costs_[type] = avg;
        }
    }
}

double CostModel::predict(const Node* node) const {
    auto it = op_costs_.find(node->type);
    double op_cost = (it != op_costs_.end()) ? it->second : base_cost_;
    
    // Cost scales with bit width
    return op_cost * (1.0 + bit_width_factor_ * std::log2(node->width + 1));
}

void CostModel::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open cost model file: " << filename << "\n";
        return;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        int type;
        double cost;
        if (iss >> type >> cost) {
            op_costs_[static_cast<NodeType>(type)] = cost;
        }
    }
    
    if (file >> bit_width_factor_ >> base_cost_) {
        // Successfully read factors
    }
}

void CostModel::saveToFile(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file for writing: " << filename << "\n";
        return;
    }
    
    for (const auto& [type, cost] : op_costs_) {
        file << static_cast<int>(type) << " " << cost << "\n";
    }
    file << bit_width_factor_ << " " << base_cost_ << "\n";
}

// Hypergraph Implementation
void Hypergraph::buildFromGraph(graph* g, const CostModel& cost_model) {
    vertex_weights.clear();
    vertex_to_node.clear();
    node_to_vertex.clear();
    hyperedges.clear();
    hyperedge_weights.clear();
    register_split.clear();
    
    int vertex_id = 0;
    
    // Phase 1: Create vertices for all nodes
    for (Node* node : g->allNodes) {
        if (node->status != VALID_NODE) continue;
        
        // Special handling for registers - split into read and write vertices
        if (node->type == NODE_REG_DST) {
            // Create read vertex
            vertex_to_node.push_back(node);
            node_to_vertex[node] = vertex_id;
            vertex_weights.push_back(1);  // Minimal cost for reads
            int read_vertex = vertex_id++;
            
            // Create write vertex
            vertex_to_node.push_back(node);
            vertex_weights.push_back(static_cast<int>(cost_model.predict(node)));
            int write_vertex = vertex_id++;
            
            register_split[node] = {read_vertex, write_vertex};
        } else {
            vertex_to_node.push_back(node);
            node_to_vertex[node] = vertex_id;
            vertex_weights.push_back(static_cast<int>(cost_model.predict(node)));
            vertex_id++;
        }
    }
    
    // Phase 2: Create hyperedges for signal nets
    std::unordered_map<Node*, std::vector<int>> net_vertices;
    
    for (const auto& [node, vid] : node_to_vertex) {
        // For each output signal, create a hyperedge connecting source to all sinks
        if (node->type == NODE_REG_DST) {
            // Register output - use read vertex
            int read_vertex = register_split[node].first;
            net_vertices[node].push_back(read_vertex);
        } else {
            net_vertices[node].push_back(vid);
        }
        
        // Add all sink vertices
        for (Node* next : node->next) {
            if (node_to_vertex.find(next) != node_to_vertex.end()) {
                if (next->type == NODE_REG_DST) {
                    // Register input - use write vertex
                    int write_vertex = register_split[next].second;
                    net_vertices[node].push_back(write_vertex);
                } else {
                    net_vertices[node].push_back(node_to_vertex[next]);
                }
            }
        }
    }
    
    // Create hyperedges from nets with 2 or more vertices
    for (const auto& [node, vertices] : net_vertices) {
        if (vertices.size() >= 2) {
            hyperedges.push_back(vertices);
            // Weight based on signal width (communication cost)
            hyperedge_weights.push_back(node->width + 1);
        }
    }
}

bool Hypergraph::validate() const {
    // Check that all vertex indices in hyperedges are valid
    for (const auto& hedge : hyperedges) {
        for (int v : hedge) {
            if (v < 0 || v >= numVertices()) {
                return false;
            }
        }
    }
    
    // Check that vertex and hyperedge weights are positive
    for (int w : vertex_weights) {
        if (w <= 0) return false;
    }
    for (int w : hyperedge_weights) {
        if (w <= 0) return false;
    }
    
    return true;
}

// HypergraphPartitioner Implementation
HypergraphPartitioner::HypergraphPartitioner() : kahypar_context_(nullptr) {
    cost_model_ = std::make_unique<CostModel>();
    config_.num_partitions = 2;
    config_.imbalance_factor = 1.03;
    config_.seed = 42;
    config_.use_mt_kahypar = true;
    config_.preset = "quality";
}

HypergraphPartitioner::~HypergraphPartitioner() {
    // Clean up KaHyPar context if needed
}

void HypergraphPartitioner::configure(int num_partitions, double imbalance, 
                                    const std::string& preset, bool use_mt) {
    config_.num_partitions = num_partitions;
    config_.imbalance_factor = imbalance;
    config_.preset = preset;
    config_.use_mt_kahypar = use_mt;
}

void HypergraphPartitioner::setCostModel(std::unique_ptr<CostModel> model) {
    cost_model_ = std::move(model);
}

void HypergraphPartitioner::buildHypergraph(graph* g) {
    hypergraph_ = std::make_unique<Hypergraph>();
    hypergraph_->buildFromGraph(g, *cost_model_);
    
    if (!hypergraph_->validate()) {
        throw std::runtime_error("Invalid hypergraph constructed");
    }
    
    std::cout << "Built hypergraph with " << hypergraph_->numVertices() 
              << " vertices and " << hypergraph_->numHyperedges() << " hyperedges\n";
}

PartitionResult HypergraphPartitioner::partition() {
    if (!hypergraph_) {
        throw std::runtime_error("Hypergraph not built");
    }
    
    PartitionResult result;
    result.num_partitions = config_.num_partitions;
    result.vertex_partition.resize(hypergraph_->numVertices());
    
    // Simplified partitioning for demonstration
    // In production, this would call actual KaHyPar library
    
    // Calculate total weight and target weight per partition
    int total_weight = std::accumulate(hypergraph_->vertex_weights.begin(), 
                                     hypergraph_->vertex_weights.end(), 0);
    int target_weight = total_weight / config_.num_partitions;
    
    // Simple greedy partitioning
    result.partition_weights.resize(config_.num_partitions, 0);
    result.partition_sizes.resize(config_.num_partitions, 0);
    
    int current_partition = 0;
    int current_weight = 0;
    
    for (int v = 0; v < hypergraph_->numVertices(); ++v) {
        if (current_weight + hypergraph_->vertex_weights[v] > target_weight * config_.imbalance_factor 
            && current_partition < config_.num_partitions - 1) {
            current_partition++;
            current_weight = 0;
        }
        
        result.vertex_partition[v] = current_partition;
        result.partition_weights[current_partition] += hypergraph_->vertex_weights[v];
        result.partition_sizes[current_partition]++;
        current_weight += hypergraph_->vertex_weights[v];
    }
    
    // Calculate cut size and imbalance
    result.cut_hyperedges_per_partition.resize(config_.num_partitions);
    result.cut_size = 0;
    
    for (size_t he = 0; he < hypergraph_->hyperedges.size(); ++he) {
        const auto& hedge = hypergraph_->hyperedges[he];
        std::unordered_set<int> partitions;
        
        for (int v : hedge) {
            partitions.insert(result.vertex_partition[v]);
        }
        
        if (partitions.size() > 1) {
            result.cut_size++;
            for (int p : partitions) {
                result.cut_hyperedges_per_partition[p].insert(he);
            }
        }
    }
    
    // Calculate imbalance
    double max_weight = *std::max_element(result.partition_weights.begin(), 
                                        result.partition_weights.end());
    double avg_weight = static_cast<double>(total_weight) / config_.num_partitions;
    result.imbalance = max_weight / avg_weight;
    
    std::cout << "Partitioning complete: cut_size=" << result.cut_size 
              << ", imbalance=" << result.imbalance << "\n";
    
    return result;
}

// ReplicationOptimizer Implementation
ReplicationOptimizer::ReplicationOptimizer(const Hypergraph& hg, const PartitionResult& pr)
    : hypergraph_(hg), partition_result_(pr) {
    stats_ = {0, 0.0, 0, std::vector<int>(pr.num_partitions, 0)};
}

std::vector<std::vector<int>> ReplicationOptimizer::findCrossingCones() {
    std::vector<std::vector<int>> cones;
    std::vector<bool> visited(hypergraph_.numVertices(), false);
    
    // Find all vertices that have outputs crossing partition boundaries
    for (const auto& hedge : hypergraph_.hyperedges) {
        std::unordered_set<int> partitions;
        for (int v : hedge) {
            partitions.insert(partition_result_.vertex_partition[v]);
        }
        
        if (partitions.size() > 1) {
            // This hyperedge crosses partitions
            // Perform backward traversal to find the cone
            std::vector<int> cone;
            std::queue<int> q;
            
            // Start from source vertex (first in hyperedge)
            int source = hedge[0];
            if (!visited[source]) {
                q.push(source);
                visited[source] = true;
                
                while (!q.empty()) {
                    int v = q.front();
                    q.pop();
                    cone.push_back(v);
                    
                    // Add predecessors (simplified - would need proper graph traversal)
                    // In real implementation, would traverse backward through the circuit
                }
                
                if (!cone.empty()) {
                    cones.push_back(cone);
                }
            }
        }
    }
    
    return cones;
}

double ReplicationOptimizer::evaluateReplicationBenefit(const std::vector<int>& cone,
                                                       const std::unordered_set<int>& target_partitions) {
    // Calculate cost of replication
    double replication_cost = 0;
    for (int v : cone) {
        replication_cost += hypergraph_.vertex_weights[v] * (target_partitions.size() - 1);
    }
    
    // Calculate benefit (eliminated dependencies)
    double benefit = cone.size() * target_partitions.size();
    
    return benefit / (replication_cost + 1.0);  // Benefit-to-cost ratio
}

void ReplicationOptimizer::optimize(double max_overhead) {
    auto cones = findCrossingCones();
    
    std::cout << "Found " << cones.size() << " crossing cones for replication analysis\n";
    
    int total_weight = std::accumulate(hypergraph_.vertex_weights.begin(),
                                     hypergraph_.vertex_weights.end(), 0);
    double current_overhead = 0.0;
    
    // Evaluate and replicate beneficial cones
    for (const auto& cone : cones) {
        // Find which partitions need this cone
        std::unordered_set<int> target_partitions;
        for (int v : cone) {
            // Check hyperedges containing this vertex
            for (size_t he = 0; he < hypergraph_.hyperedges.size(); ++he) {
                const auto& hedge = hypergraph_.hyperedges[he];
                if (std::find(hedge.begin(), hedge.end(), v) != hedge.end()) {
                    for (int u : hedge) {
                        target_partitions.insert(partition_result_.vertex_partition[u]);
                    }
                }
            }
        }
        
        // Evaluate benefit
        double benefit_ratio = evaluateReplicationBenefit(cone, target_partitions);
        
        if (benefit_ratio > 1.0 && current_overhead < max_overhead) {
            // Beneficial to replicate
            for (int v : cone) {
                vertex_replicas_[v] = target_partitions;
                stats_.total_replicated_nodes++;
            }
            
            // Update overhead
            for (int v : cone) {
                current_overhead += hypergraph_.vertex_weights[v] * (target_partitions.size() - 1) 
                                  / static_cast<double>(total_weight);
            }
            
            stats_.dependencies_eliminated += cone.size() * (target_partitions.size() - 1);
        }
    }
    
    stats_.replication_overhead = current_overhead;
    
    // Count replicas per partition
    for (const auto& [v, partitions] : vertex_replicas_) {
        for (int p : partitions) {
            stats_.replicas_per_partition[p]++;
        }
    }
    
    std::cout << "Replication optimization complete: " << stats_.total_replicated_nodes 
              << " nodes replicated, overhead=" << (stats_.replication_overhead * 100) 
              << "%, dependencies eliminated=" << stats_.dependencies_eliminated << "\n";
}

std::vector<std::unique_ptr<Partition>> ReplicationOptimizer::createFinalPartitions() {
    std::vector<std::unique_ptr<Partition>> partitions;
    
    for (int p = 0; p < partition_result_.num_partitions; ++p) {
        auto partition = std::make_unique<Partition>(p);
        
        // Add originally assigned nodes
        for (int v = 0; v < hypergraph_.numVertices(); ++v) {
            if (partition_result_.vertex_partition[v] == p) {
                partition->node_ids.push_back(v);
                
                // Check if this is a register
                Node* node = hypergraph_.vertex_to_node[v];
                if (node->type == NODE_REG_DST) {
                    partition->register_ids.push_back(v);
                    partition->output_registers.push_back(v);
                }
            }
        }
        
        // Add replicated nodes
        for (const auto& [v, target_partitions] : vertex_replicas_) {
            if (target_partitions.count(p) > 0 && partition_result_.vertex_partition[v] != p) {
                partition->replicated_logic.push_back(v);
            }
        }
        
        // Calculate costs
        partition->computational_cost = partition_result_.partition_weights[p];
        for (int v : partition->replicated_logic) {
            partition->computational_cost += hypergraph_.vertex_weights[v];
        }
        
        partition->communication_cost = partition_result_.cut_hyperedges_per_partition[p].size();
        
        partitions.push_back(std::move(partition));
    }
    
    return partitions;
}

// PartitioningPipeline Implementation
PartitioningPipeline::PartitioningPipeline(graph* g) : circuit_graph_(g) {
    partitioner_ = std::make_unique<HypergraphPartitioner>();
}

void PartitioningPipeline::configure(int num_threads, const std::string& cost_model_file) {
    partitioner_->configure(num_threads, 1.03, "quality", true);
    
    if (!cost_model_file.empty()) {
        auto cost_model = std::make_unique<CostModel>();
        cost_model->loadFromFile(cost_model_file);
        partitioner_->setCostModel(std::move(cost_model));
    }
}

std::vector<std::unique_ptr<Partition>> PartitioningPipeline::run() {
    std::cout << "\n=== Starting Partitioning Pipeline ===\n";
    
    // Stage 1: Build hypergraph
    std::cout << "Stage 1: Building hypergraph...\n";
    partitioner_->buildHypergraph(circuit_graph_);
    
    // Stage 2: Initial partitioning
    std::cout << "Stage 2: Running hypergraph partitioning...\n";
    auto partition_result = partitioner_->partition();
    
    // Stage 3: Replication optimization
    std::cout << "Stage 3: Optimizing with replication...\n";
    replicator_ = std::make_unique<ReplicationOptimizer>(partitioner_->getHypergraph(), partition_result);
    replicator_->optimize(0.2);  // Max 20% overhead
    
    // Stage 4: Create final partitions
    std::cout << "Stage 4: Creating final partitions...\n";
    auto final_partitions = replicator_->createFinalPartitions();
    
    std::cout << "=== Partitioning Pipeline Complete ===\n\n";
    
    return final_partitions;
}

void PartitioningPipeline::printStatistics() const {
    if (!replicator_) return;
    
    const auto& stats = replicator_->getStats();
    
    std::cout << "\n=== Partitioning Statistics ===\n";
    std::cout << "Total replicated nodes: " << stats.total_replicated_nodes << "\n";
    std::cout << "Replication overhead: " << (stats.replication_overhead * 100) << "%\n";
    std::cout << "Dependencies eliminated: " << stats.dependencies_eliminated << "\n";
    std::cout << "Replicas per partition: ";
    for (size_t i = 0; i < stats.replicas_per_partition.size(); ++i) {
        std::cout << "[" << i << "]=" << stats.replicas_per_partition[i] << " ";
    }
    std::cout << "\n";
}