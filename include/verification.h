#ifndef FLSM_VERIFICATION_H
#define FLSM_VERIFICATION_H

#include <string>

struct VerificationOptions {
    std::string test = "full";
    int ops = 5000;
};

// Runs an isolated verification workload in flsm_verify_lab and returns
// evidence JSON. The production database directory is not touched.
std::string run_verification(const VerificationOptions& options);

#endif // FLSM_VERIFICATION_H
