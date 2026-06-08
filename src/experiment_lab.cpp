#include "experiment_lab.h"

#include "kvstore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string json_str(const std::string& key, const std::string& val, bool last = false) {
    return "  \"" + key + "\": \"" + json_escape(val) + "\"" + (last ? "\n" : ",\n");
}

std::string json_num(const std::string& key, uint64_t val, bool last = false) {
    return "  \"" + key + "\": " + std::to_string(val) + (last ? "\n" : ",\n");
}

std::string json_dbl(const std::string& key, double val, bool last = false) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << val;
    return "  \"" + key + "\": " + ss.str() + (last ? "\n" : ",\n");
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::vector<std::string> split_ws(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> parts;
    std::string part;
    while (in >> part) parts.push_back(part);
    return parts;
}

bool parse_range(const std::string& token, int& start, int& end) {
    auto pos = token.find("..");
    if (pos == std::string::npos) return false;
    try {
        start = std::stoi(token.substr(0, pos));
        end = std::stoi(token.substr(pos + 2));
    } catch (...) {
        return false;
    }
    if (start > end) std::swap(start, end);
    return start >= 0 && end >= 0 && (end - start + 1) <= 20000;
}

std::vector<int> range_ids(int start, int end, const std::string& order) {
    std::vector<int> ids;
    for (int i = start; i <= end; ++i) ids.push_back(i);
    if (order == "random") {
        std::sort(ids.begin(), ids.end(), [](int a, int b) {
            uint32_t ha = static_cast<uint32_t>(a) * 2654435761u;
            uint32_t hb = static_cast<uint32_t>(b) * 2654435761u;
            return ha < hb;
        });
    }
    return ids;
}

std::string event_key(int id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "lab:event:%08d", id);
    return std::string(buf);
}

std::string event_value(int id, int version, const std::string& op) {
    std::ostringstream value;
    value << "{\"device_id\":\"device_" << (id % 128)
          << "\",\"sensor\":\"edge_lab\","
          << "\"sequence\":" << id
          << ",\"version\":" << version
          << ",\"op\":\"" << op << "\","
          << "\"temperature_c\":" << std::fixed << std::setprecision(2)
          << (20.0 + static_cast<double>((id * 37 + version * 11) % 1800) / 100.0)
          << ",\"vibration_g\":" << std::fixed << std::setprecision(3)
          << (0.10 + static_cast<double>((id * 17 + version * 7) % 900) / 1000.0)
          << "}";
    return value.str();
}

uint64_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    return static_cast<uint64_t>(std::filesystem::file_size(path, ec));
}

struct LabRun {
    bool ok = true;
    uint64_t commands = 0;
    uint64_t generated_ops = 0;
    uint64_t puts = 0;
    uint64_t updates = 0;
    uint64_t deletes = 0;
    uint64_t gets = 0;
    uint64_t flushes = 0;
    uint64_t compactions = 0;
    uint64_t recoveries = 0;
    uint64_t verify_checks = 0;
    uint64_t mismatches = 0;
    std::vector<std::string> steps;
    std::vector<std::string> mismatch_rows;
    std::map<std::string, std::string> model;
    std::set<std::string> deleted_keys;
    std::map<int, int> versions;

    void step(const std::string& name, bool pass, const std::string& detail) {
        if (!pass) ok = false;
        std::ostringstream row;
        row << "{\"name\":\"" << json_escape(name)
            << "\",\"status\":\"" << (pass ? "pass" : "fail")
            << "\",\"detail\":\"" << json_escape(detail) << "\"}";
        steps.push_back(row.str());
    }

    void mismatch(const std::string& detail) {
        ok = false;
        mismatches++;
        if (mismatch_rows.size() < 25) mismatch_rows.push_back(detail);
    }

    void verify_key(KVStore& db, const std::string& key, const std::string& phase) {
        std::string actual;
        bool found = db.get(key, actual);
        auto it = model.find(key);
        bool expected_found = it != model.end();
        verify_checks++;
        if (!expected_found && found) {
            mismatch(phase + ": expected missing but found " + key);
        } else if (expected_found && !found) {
            mismatch(phase + ": expected present but missing " + key);
        } else if (expected_found && actual != it->second) {
            mismatch(phase + ": value mismatch for " + key);
        }
    }

    uint64_t verify_all(KVStore& db, const std::string& phase) {
        uint64_t before = mismatches;
        uint64_t checked = 0;
        for (const auto& [key, _] : model) {
            verify_key(db, key, phase);
            checked++;
        }
        for (const auto& key : deleted_keys) {
            verify_key(db, key, phase);
            checked++;
        }
        step("verify", mismatches == before,
             phase + " checked " + std::to_string(checked) + " model/deleted keys.");
        return checked;
    }

    std::string to_json(KVStore& db, const std::string& dir) {
        auto summary = db.storage_summary();
        uint64_t wal_bytes = 0;
        uint64_t sst_bytes = 0;
        uint64_t sst_files = 0;
        uint64_t vlog_bytes = file_size_or_zero(std::filesystem::path(dir) / "vlog.bin");
        std::error_code ec;
        if (std::filesystem::exists(dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                auto name = entry.path().filename().string();
                uint64_t size = std::filesystem::is_regular_file(entry.path(), ec)
                    ? static_cast<uint64_t>(std::filesystem::file_size(entry.path(), ec))
                    : 0;
                if (name.rfind("wal_", 0) == 0 && entry.path().extension() == ".log") {
                    wal_bytes += size;
                } else if (name.rfind("sst_", 0) == 0 && entry.path().extension() == ".sst") {
                    sst_bytes += size;
                    sst_files++;
                }
            }
        }

        uint64_t total_disk = wal_bytes + sst_bytes + vlog_bytes;
        double space_amp = summary.live_logical_bytes > 0
            ? static_cast<double>(total_disk) / static_cast<double>(summary.live_logical_bytes)
            : 0.0;
        const auto& m = db.metrics();
        double write_amp = m.user_bytes_written > 0
            ? static_cast<double>(m.storage_bytes_written) / static_cast<double>(m.user_bytes_written)
            : 0.0;

        std::ostringstream j;
        j << "{\n";
        j << json_str("status", ok ? "pass" : "fail");
        j << json_num("commands", commands);
        j << json_num("generated_ops", generated_ops);
        j << json_num("puts", puts);
        j << json_num("updates", updates);
        j << json_num("deletes", deletes);
        j << json_num("gets", gets);
        j << json_num("flushes", flushes);
        j << json_num("compactions", compactions);
        j << json_num("recoveries", recoveries);
        j << json_num("verify_checks", verify_checks);
        j << json_num("mismatches", mismatches);
        j << json_num("model_live_keys", static_cast<uint64_t>(model.size()));
        j << json_num("model_deleted_keys", static_cast<uint64_t>(deleted_keys.size()));
        j << json_num("memtable_entries", static_cast<uint64_t>(db.memtable_entries()));
        j << json_num("memtable_bytes", static_cast<uint64_t>(db.active_byte_size()));
        j << json_num("flush_threshold", static_cast<uint64_t>(db.flush_threshold_bytes()));
        j << json_num("l0_count", static_cast<uint64_t>(db.l0_count()));
        j << json_num("l1_count", static_cast<uint64_t>(db.l1_count()));
        j << json_num("l2_count", static_cast<uint64_t>(db.l2_count()));
        j << json_num("wal_bytes", wal_bytes);
        j << json_num("sst_files", sst_files);
        j << json_num("sst_bytes", sst_bytes);
        j << json_num("vlog_bytes", vlog_bytes);
        j << json_num("total_disk_bytes", total_disk);
        j << json_num("live_keys_estimate", summary.live_keys);
        j << json_num("tombstones_estimate", summary.tombstones);
        j << json_num("live_logical_bytes_estimate", summary.live_logical_bytes);
        j << json_dbl("space_amplification_estimate", space_amp);
        j << json_dbl("write_amplification", write_amp);
        j << "  \"steps\": [";
        for (size_t i = 0; i < steps.size(); ++i) {
            if (i) j << ",";
            j << steps[i];
        }
        j << "],\n";
        j << "  \"mismatch_details\": [";
        for (size_t i = 0; i < mismatch_rows.size(); ++i) {
            if (i) j << ",";
            j << "\"" << json_escape(mismatch_rows[i]) << "\"";
        }
        j << "]\n";
        j << "}";
        return j.str();
    }
};

void do_put(LabRun& run, KVStore& db, const std::string& key, const std::string& value, bool is_update) {
    db.put(key, value);
    run.model[key] = value;
    run.deleted_keys.erase(key);
    run.generated_ops++;
    if (is_update) run.updates++;
    else run.puts++;
}

void do_delete(LabRun& run, KVStore& db, const std::string& key) {
    db.delete_key(key);
    run.model.erase(key);
    run.deleted_keys.insert(key);
    run.generated_ops++;
    run.deletes++;
}

void do_get(LabRun& run, KVStore& db, const std::string& key, const std::string& phase) {
    run.gets++;
    run.generated_ops++;
    run.verify_key(db, key, phase);
}

bool run_bulk(LabRun& run, KVStore& db, const std::vector<std::string>& parts, const std::string& command) {
    if (parts.size() < 2) return false;
    int start = 0;
    int end = 0;
    if (!parse_range(parts[1], start, end)) return false;
    std::string order = parts.size() >= 3 ? lower(parts[2]) : "sequential";
    if (order != "random") order = "sequential";
    auto ids = range_ids(start, end, order);

    for (int id : ids) {
        std::string key = event_key(id);
        if (command == "insert") {
            int version = ++run.versions[id];
            do_put(run, db, key, event_value(id, version, "insert"), false);
        } else if (command == "update") {
            int version = ++run.versions[id];
            do_put(run, db, key, event_value(id, version, "update"), true);
        } else if (command == "delete") {
            do_delete(run, db, key);
        } else if (command == "get") {
            do_get(run, db, key, "bulk get " + parts[1]);
        }
    }

    run.step(command, true, command + " " + parts[1] + " " + order +
                            " executed " + std::to_string(ids.size()) + " operations.");
    return true;
}

bool run_manual_put(LabRun& run, KVStore& db, const std::string& line, bool is_update) {
    std::istringstream in(line);
    std::string cmd;
    std::string key;
    in >> cmd >> key;
    std::string value;
    std::getline(in, value);
    value = trim(value);
    if (key.empty() || value.empty()) return false;
    do_put(run, db, key, value, is_update);
    run.step(cmd, true, key + " written.");
    return true;
}

bool run_manual_get_delete(LabRun& run, KVStore& db, const std::vector<std::string>& parts) {
    if (parts.size() < 2) return false;
    std::string cmd = lower(parts[0]);
    std::string key = parts[1];
    if (cmd == "get") {
        do_get(run, db, key, "manual get");
        run.step("get", true, key + " compared with reference model.");
    } else {
        do_delete(run, db, key);
        run.step("delete", true, key + " tombstoned.");
    }
    return true;
}

} // namespace

std::string run_experiment_lab(const ExperimentOptions& options) {
    const std::string dir = "flsm_experiment_lab";
    LabRun run;

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    KVStoreOptions kv_options;
    kv_options.flush_threshold = 32u * 1024u;
    kv_options.l0_hard_limit = 2;
    kv_options.l1_hard_limit = 2;
    KVStore db(dir, kv_options);

    std::istringstream script(options.script);
    std::string line;
    while (std::getline(script, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        run.commands++;
        auto parts = split_ws(line);
        std::string cmd = parts.empty() ? "" : lower(parts[0]);

        bool handled = false;
        if (cmd == "insert" || cmd == "update" || cmd == "delete" || cmd == "get") {
            if (parts.size() >= 2 && parts[1].find("..") != std::string::npos) {
                handled = run_bulk(run, db, parts, cmd);
            } else if (cmd == "put" || cmd == "update") {
                handled = run_manual_put(run, db, line, cmd == "update");
            } else if (cmd == "get" || cmd == "delete") {
                handled = run_manual_get_delete(run, db, parts);
            }
        } else if (cmd == "put") {
            handled = run_manual_put(run, db, line, false);
        } else if (cmd == "flush") {
            db.force_flush();
            run.flushes++;
            run.step("flush", true, "Memtable forced to L0 SSTable if non-empty.");
            handled = true;
        } else if (cmd == "compact") {
            db.force_compaction();
            run.compactions++;
            run.step("compact", true, "L0 to L1 compaction requested.");
            handled = true;
        } else if (cmd == "compact_l1" || cmd == "compact-l1") {
            db.force_l1_compaction();
            run.compactions++;
            run.step("compact_l1", true, "L1 to L2 compaction requested.");
            handled = true;
        } else if (cmd == "recover" || cmd == "restart") {
            db = KVStore(dir, kv_options);
            run.recoveries++;
            run.step("recover", true, "Database reopened from WAL/SST/VLog files.");
            handled = true;
        } else if (cmd == "verify") {
            run.verify_all(db, "explicit verify");
            handled = true;
        } else if (cmd == "state") {
            run.step("state", true,
                     "mem=" + std::to_string(db.memtable_entries()) +
                     ", L0=" + std::to_string(db.l0_count()) +
                     ", L1=" + std::to_string(db.l1_count()) +
                     ", L2=" + std::to_string(db.l2_count()));
            handled = true;
        }

        if (!handled) {
            run.step("parse", false, "Unsupported command: " + line);
        }
    }

    if (run.commands == 0) {
        run.step("parse", false, "No commands were provided.");
    } else {
        run.verify_all(db, "final automatic verify");
    }

    return run.to_json(db, dir);
}
