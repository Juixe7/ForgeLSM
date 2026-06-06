#include "cli.h"
#include "benchmark.h"
#include <iostream>
#include <sstream>

void CLI::run(KVStore& store) {
    std::string line;
    std::cout << "WiscKey Engine CLI\n";
    std::cout << "Commands: put <k> <v>, get <k>, delete <k>, load <n>, bench <type>\n";
    std::cout << "> ";

    while (std::getline(std::cin, line)) {
        if (line == "exit" || line == "quit") break;
        if (!line.empty()) {
            parse_command(store, line);
        }
        std::cout << "> ";
    }
}

void CLI::parse_command(KVStore& store, const std::string& cmd_line) {
    std::istringstream iss(cmd_line);
    std::string cmd;
    iss >> cmd;

    try {
        if (cmd == "put") {
            std::string key, value;
            iss >> key;
            std::getline(iss >> std::ws, value);
            if (key.empty() || value.empty()) {
                std::cerr << "Error: put requires key and value.\n";
                return;
            }
            store.put(key, value);
            std::cout << "OK\n";
        }
        else if (cmd == "get") {
            std::string key, val;
            iss >> key;
            if (key.empty()) {
                std::cerr << "Error: get requires key.\n";
                return;
            }
            if (store.get(key, val)) {
                std::cout << val << "\n";
            } else {
                std::cout << "(not found)\n";
            }
        }
        else if (cmd == "delete") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cerr << "Error: delete requires key.\n";
                return;
            }
            store.delete_key(key);
            std::cout << "OK\n";
        }
        else if (cmd == "load") {
            int n;
            iss >> n;
            if (n <= 0) {
                std::cerr << "Error: load requires positive integer.\n";
                return;
            }
            for (int i = 0; i < n; ++i) {
                store.put("load_" + std::to_string(i), "val_" + std::to_string(i));
            }
            std::cout << "Loaded " << n << " keys.\n";
        }
        else if (cmd == "bench") {
            std::string type;
            iss >> type;
            if (type != "random_write" && type != "sequential_write" && 
                type != "random_read" && type != "mixed") {
                std::cerr << "Error: valid types are random_write, sequential_write, random_read, mixed.\n";
                return;
            }
            std::cout << "Running benchmark " << type << " (20,000 ops)... " << std::flush;
            Benchmark::run_all(type);
            std::cout << "Done.\n";
        }
        else {
            std::cerr << "Error: unknown command.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "System Error during command execution: " << e.what() << "\n";
    }
}
