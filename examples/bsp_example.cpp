/**
 * @file bsp_example.cpp
 * @brief Example of using BSP-based parallel RTL simulation
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>

// Include the generated simulator header
#include "Smytop.h"  // Replace with your actual module name

int main(int argc, char** argv) {
    // Parse command line arguments
    int num_threads = std::thread::hardware_concurrency();
    int num_cycles = 1000000;
    bool enable_profiling = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            num_cycles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0) {
            enable_profiling = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                     << "Options:\n"
                     << "  --threads <n>    Number of threads (default: auto-detect)\n"
                     << "  --cycles <n>     Number of simulation cycles (default: 1000000)\n"
                     << "  --profile        Enable performance profiling\n"
                     << "  --help           Show this help message\n";
            return 0;
        }
    }
    
    std::cout << "BSP-based RTL Simulation Example\n";
    std::cout << "=================================\n";
    std::cout << "Threads: " << num_threads << "\n";
    std::cout << "Cycles: " << num_cycles << "\n";
    std::cout << "Profiling: " << (enable_profiling ? "enabled" : "disabled") << "\n\n";
    
    // Set environment variable for profiling
    if (enable_profiling) {
        setenv("BSP_PROFILING", "1", 1);
    }
    
    try {
        // Create simulator instance with BSP support
        Smytop sim(num_threads);
        
        // Reset the design
        sim.reset();
        
        // Initialize inputs
        sim.set_clock(0);
        sim.set_reset(1);
        
        // Run reset for a few cycles
        for (int i = 0; i < 10; i++) {
            sim.set_clock(0);
            sim.step();
            sim.set_clock(1);
            sim.step();
        }
        
        // Release reset
        sim.set_reset(0);
        
        // Start timing
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Main simulation loop
        for (int cycle = 0; cycle < num_cycles; cycle++) {
            // Toggle clock
            sim.set_clock(0);
            sim.step();
            sim.set_clock(1);
            sim.step();
            
            // Optionally check outputs or apply stimulus
            if (cycle % 10000 == 0) {
                std::cout << "\rProgress: " << (100.0 * cycle / num_cycles) << "%" << std::flush;
            }
        }
        
        // End timing
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::cout << "\n\nSimulation Complete!\n";
        std::cout << "Total time: " << duration.count() << " ms\n";
        std::cout << "Cycles per second: " << (1000.0 * num_cycles / duration.count()) << "\n";
        
        // Print performance statistics
        if (enable_profiling) {
            sim.printStats();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}