#ifndef FLSM_BENCHMARK_H
#define FLSM_BENCHMARK_H

#include "kvstore.h"
#include <string>

class Benchmark {
public:
    static void run_all(const std::string& type, int num_ops = 20000); 
private:
    static void run_workload(const std::string& dir, const std::string& type, bool is_warm, int num_ops);
};

#endif // FLSM_BENCHMARK_H
