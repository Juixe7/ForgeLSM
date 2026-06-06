#ifndef FLSM_CLI_H
#define FLSM_CLI_H

#include "kvstore.h"
#include <string>

class CLI {
public:
    static void run(KVStore& store);
    static void parse_command(KVStore& store, const std::string& cmd_line);
};

#endif // FLSM_CLI_H
