#ifndef FLSM_EXPERIMENT_LAB_H
#define FLSM_EXPERIMENT_LAB_H

#include <string>

struct ExperimentOptions {
    std::string script;
};

// Runs a user-authored experiment script in an isolated low-threshold store
// and compares all reads/verification points with an in-memory reference model.
std::string run_experiment_lab(const ExperimentOptions& options);

#endif // FLSM_EXPERIMENT_LAB_H
