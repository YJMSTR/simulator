/**
 * @file cppEmitterBSP.cpp
 * @brief C++ code emitter for BSP-based parallel RTL simulation
 */

#include "common.h"
#include "bsp_executor.h"
#include <cstdio>
#include <sstream>
#include <set>

// External function to get BSP executor
extern BspExecutor* getBspExecutor();

// Generate BSP-aware simulation code
void graph::cppEmitterBSP() {
    FILE* header = genHeaderStart();
    FILE* src = genSrcStart();
    
    // Include BSP headers
    fprintf(header, "#include \"bsp_executor.h\"\n");
    fprintf(header, "#include <vector>\n");
    fprintf(header, "#include <atomic>\n");
    fprintf(header, "#include <thread>\n\n");
    
    // Generate class definition with BSP support
    fprintf(header, "class S%s {\n", name.c_str());
    fprintf(header, "private:\n");
    fprintf(header, "    BspExecutor* bsp_executor_;\n");
    fprintf(header, "    size_t num_registers_;\n");
    fprintf(header, "    int num_threads_;\n");
    fprintf(header, "\n");
    
    // Generate register declarations
    fprintf(header, "    // Global register state (cache-aligned)\n");
    fprintf(header, "    alignas(64) std::vector<uint64_t> registers_;\n");
    fprintf(header, "\n");
    
    // Generate node declarations for inputs/outputs
    fprintf(header, "    // Input/Output interface\n");
    for (Node* node : input) {
        genNodeDef(header, node);
    }
    for (Node* node : output) {
        genNodeDef(header, node);
    }
    fprintf(header, "\n");
    
    // Generate memory declarations
    if (!memory.empty()) {
        fprintf(header, "    // Memory modules\n");
        for (Node* mem : memory) {
            genNodeDef(header, mem);
        }
        fprintf(header, "\n");
    }
    
    // Generate thread-local computation functions
    fprintf(header, "    // Thread-local computation functions\n");
    fprintf(header, "    void computePartition(int partition_id, ThreadLocalStorage& tls);\n");
    fprintf(header, "\n");
    
    // Public interface
    fprintf(header, "public:\n");
    fprintf(header, "    S%s(int num_threads = 0);\n", name.c_str());
    fprintf(header, "    ~S%s();\n", name.c_str());
    fprintf(header, "    void step();\n");
    fprintf(header, "    void reset();\n");
    fprintf(header, "    uint64_t getCycles() const;\n");
    fprintf(header, "    void printStats() const;\n");
    
    // Generate getter/setter for interface signals
    for (Node* node : input) {
        genInterfaceInput(header, node);
    }
    for (Node* node : output) {
        genInterfaceOutput(header, node);
    }
    
    fprintf(header, "};\n\n");
    fprintf(header, "#endif\n");
    fclose(header);
    
    // Generate source file
    fprintf(src, "#include \"S%s.h\"\n", name.c_str());
    fprintf(src, "#include <iostream>\n");
    fprintf(src, "#include <cstring>\n\n");
    
    // Constructor
    fprintf(src, "S%s::S%s(int num_threads) {\n", name.c_str(), name.c_str());
    fprintf(src, "    num_threads_ = (num_threads > 0) ? num_threads : std::thread::hardware_concurrency();\n");
    
    // Count registers
    fprintf(src, "    num_registers_ = %zu;\n", regsrc.size());
    fprintf(src, "    registers_.resize(num_registers_);\n");
    
    // Initialize BSP executor
    fprintf(src, "    bsp_executor_ = getBspExecutor();\n");
    fprintf(src, "    if (!bsp_executor_) {\n");
    fprintf(src, "        throw std::runtime_error(\"BSP executor not initialized\");\n");
    fprintf(src, "    }\n");
    fprintf(src, "    bsp_executor_->start();\n");
    
    // Initialize registers and memories
    fprintf(src, "    reset();\n");
    fprintf(src, "}\n\n");
    
    // Destructor
    fprintf(src, "S%s::~S%s() {\n", name.c_str(), name.c_str());
    fprintf(src, "    if (bsp_executor_) {\n");
    fprintf(src, "        bsp_executor_->stop();\n");
    fprintf(src, "    }\n");
    fprintf(src, "}\n\n");
    
    // Reset function
    fprintf(src, "void S%s::reset() {\n", name.c_str());
    fprintf(src, "    std::memset(registers_.data(), 0, registers_.size() * sizeof(uint64_t));\n");
    
    // Reset memories
    for (Node* mem : memory) {
        if (mem->isArray()) {
            fprintf(src, "    std::memset(%s, 0, sizeof(%s));\n", mem->name.c_str(), mem->name.c_str());
        } else {
            fprintf(src, "    %s = 0;\n", mem->name.c_str());
        }
    }
    fprintf(src, "}\n\n");
    
    // Step function - now uses BSP executor
    fprintf(src, "void S%s::step() {\n", name.c_str());
    
    // Copy input values to registers
    fprintf(src, "    // Update input registers\n");
    int reg_idx = 0;
    for (Node* node : regsrc) {
        if (node->prev.empty()) {  // This is an input register
            for (Node* prev : node->prev) {
                if (prev->type == NODE_OTHERS && input.end() != std::find(input.begin(), input.end(), prev)) {
                    fprintf(src, "    bsp_executor_->setRegister(%d, %s);\n", reg_idx, prev->name.c_str());
                    break;
                }
            }
        }
        reg_idx++;
    }
    
    // Execute one BSP cycle
    fprintf(src, "    bsp_executor_->executeCycle();\n");
    
    // Copy register values to outputs
    fprintf(src, "    // Update output signals\n");
    fprintf(src, "    const auto& reg_state = bsp_executor_->getRegisterState();\n");
    reg_idx = 0;
    for (Node* node : regsrc) {
        for (Node* next : node->next) {
            if (next->type == NODE_OTHERS && output.end() != std::find(output.begin(), output.end(), next)) {
                fprintf(src, "    %s = reg_state[%d];\n", next->name.c_str(), reg_idx);
            }
        }
        reg_idx++;
    }
    
    fprintf(src, "}\n\n");
    
    // Generate partition computation function
    fprintf(src, "void S%s::computePartition(int partition_id, ThreadLocalStorage& tls) {\n", name.c_str());
    fprintf(src, "    // This function would contain the actual computation logic for each partition\n");
    fprintf(src, "    // Generated based on the partition assignment\n");
    fprintf(src, "    // For now, this is a placeholder\n");
    
    // The actual computation would be generated here based on partition assignment
    // This would include:
    // 1. Reading from global registers
    // 2. Computing combinational logic
    // 3. Writing to thread-local register storage
    
    fprintf(src, "}\n\n");
    
    // Get cycles function
    fprintf(src, "uint64_t S%s::getCycles() const {\n", name.c_str());
    fprintf(src, "    // Return current simulation cycle\n");
    fprintf(src, "    return 0; // Placeholder\n");
    fprintf(src, "}\n\n");
    
    // Print statistics
    fprintf(src, "void S%s::printStats() const {\n", name.c_str());
    fprintf(src, "    if (bsp_executor_) {\n");
    fprintf(src, "        bsp_executor_->printStatistics();\n");
    fprintf(src, "    }\n");
    fprintf(src, "}\n\n");
    
    // Generate interface getter/setter implementations
    for (Node* node : input) {
        fprintf(src, "void S%s::set_%s(uint64_t val) {\n", name.c_str(), node->name.c_str());
        fprintf(src, "    %s = val;\n", node->name.c_str());
        fprintf(src, "}\n\n");
    }
    
    for (Node* node : output) {
        fprintf(src, "uint64_t S%s::get_%s() const {\n", name.c_str(), node->name.c_str());
        fprintf(src, "    return %s;\n", node->name.c_str());
        fprintf(src, "}\n\n");
    }
    
    fclose(src);
    
    // Generate partition-specific computation files
    generatePartitionCode();
}

// Helper function to generate partition-specific code
void graph::generatePartitionCode() {
    auto bsp = getBspExecutor();
    if (!bsp) return;
    
    // This would generate optimized code for each partition
    // Including:
    // 1. Replicated logic
    // 2. Local computation
    // 3. Communication patterns
    
    // For now, this is a placeholder for the actual implementation
}