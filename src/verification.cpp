#include "verification.h"

#include "kvstore.h"
#include "vlog_gc.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
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

std::string event_key(int i) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "event:machine_07:vibration:%08d", i);
    return std::string(buf);
}

std::string event_value(int i, int version) {
    std::ostringstream v;
    v << "{\"device_id\":\"machine_07\","
      << "\"sensor\":\"vibration\","
      << "\"sequence\":" << i << ","
      << "\"version\":" << version << ","
      << "\"value\":" << std::fixed << std::setprecision(3)
      << (0.500 + (i % 100) / 100.0) << ","
      << "\"unit\":\"g\"}";
    return v.str();
}

uint64_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    return static_cast<uint64_t>(std::filesystem::file_size(path, ec));
}

struct VerificationRun {
    std::string test;
    int ops = 0;
    bool ok = true;
    std::vector<std::string> steps;
    std::vector<std::string> mismatches;
    std::map<std::string, std::string> model;

    uint64_t requested_events = 0;
    uint64_t model_live_keys = 0;
    uint64_t initial_checked = 0;
    uint64_t post_mutation_checked = 0;
    uint64_t deleted_checked = 0;
    uint64_t deleted_found = 0;
    uint64_t post_flush_checked = 0;
    uint64_t post_compaction_checked = 0;
    uint64_t post_recovery_checked = 0;
    uint64_t post_gc_checked = 0;
    uint64_t l0_before_flush = 0;
    uint64_t l0_after_flush = 0;
    uint64_t l0_before_compaction = 0;
    uint64_t l0_after_compaction = 0;
    uint64_t l1_before_compaction = 0;
    uint64_t l1_after_compaction = 0;
    uint64_t missing_queries = 0;
    uint64_t bloom_skips = 0;
    uint64_t sst_searches = 0;
    uint64_t vlog_before_gc = 0;
    uint64_t vlog_after_gc = 0;

    void add_step(const std::string& name, bool pass, const std::string& detail) {
        if (!pass) ok = false;
        std::ostringstream s;
        s << "{\"name\":\"" << json_escape(name)
          << "\",\"status\":\"" << (pass ? "pass" : "fail")
          << "\",\"detail\":\"" << json_escape(detail) << "\"}";
        steps.push_back(s.str());
    }

    void mismatch(const std::string& msg) {
        ok = false;
        if (mismatches.size() < 10) mismatches.push_back(msg);
    }

    uint64_t verify_model(KVStore& db, const std::string& phase) {
        uint64_t checked = 0;
        for (const auto& [key, expected] : model) {
            std::string actual;
            if (!db.get(key, actual)) {
                mismatch(phase + ": missing " + key);
            } else if (actual != expected) {
                mismatch(phase + ": value mismatch for " + key);
            }
            checked++;
        }
        return checked;
    }

    std::string to_json() {
        model_live_keys = static_cast<uint64_t>(model.size());
        std::ostringstream j;
        j << "{\n";
        j << json_str("name", "Edge/IoT event-store " + test + " verification");
        j << json_str("test", test);
        j << json_str("status", ok ? "pass" : "fail");
        j << json_num("requested_events", requested_events);
        j << json_num("model_live_keys", model_live_keys);
        j << json_num("initial_checked", initial_checked);
        j << json_num("post_mutation_checked", post_mutation_checked);
        j << json_num("deleted_checked", deleted_checked);
        j << json_num("deleted_found", deleted_found);
        j << json_num("post_flush_checked", post_flush_checked);
        j << json_num("post_compaction_checked", post_compaction_checked);
        j << json_num("post_recovery_checked", post_recovery_checked);
        j << json_num("post_gc_checked", post_gc_checked);
        j << json_num("l0_before_flush", l0_before_flush);
        j << json_num("l0_after_flush", l0_after_flush);
        j << json_num("l0_before_compaction", l0_before_compaction);
        j << json_num("l0_after_compaction", l0_after_compaction);
        j << json_num("l1_before_compaction", l1_before_compaction);
        j << json_num("l1_after_compaction", l1_after_compaction);
        j << json_num("missing_queries", missing_queries);
        j << json_num("bloom_skips", bloom_skips);
        j << json_num("sst_searches", sst_searches);
        j << json_num("vlog_before_gc", vlog_before_gc);
        j << json_num("vlog_after_gc", vlog_after_gc);
        j << "  \"steps\": [";
        for (size_t i = 0; i < steps.size(); ++i) {
            if (i) j << ",";
            j << steps[i];
        }
        j << "],\n";
        j << "  \"mismatches\": [";
        for (size_t i = 0; i < mismatches.size(); ++i) {
            if (i) j << ",";
            j << "\"" << json_escape(mismatches[i]) << "\"";
        }
        j << "]\n";
        j << "}";
        return j.str();
    }
};

bool wants(const std::string& test, const std::string& phase) {
    if (test == "full") return phase != "gc";
    return test == phase;
}

void insert_events(VerificationRun& run, KVStore& db, int count) {
    for (int i = 0; i < count; ++i) {
        std::string key = event_key(i);
        std::string value = event_value(i, 1);
        db.put(key, value);
        run.model[key] = value;
    }
    run.requested_events = static_cast<uint64_t>(count);
    run.initial_checked = run.verify_model(db, "initial write/read");
    run.add_step("Basic put/get", run.mismatches.empty(),
                 std::to_string(count) + " deterministic IoT events written and read through the engine.");
}

void overwrite_events(VerificationRun& run, KVStore& db, int count) {
    const int overwrite_count = std::max(1, count / 10);
    for (int i = 0; i < overwrite_count; ++i) {
        std::string key = event_key(i);
        std::string value = event_value(i, 2);
        db.put(key, value);
        run.model[key] = value;
    }
    run.post_mutation_checked = run.verify_model(db, "overwrite newest-wins");
    run.add_step("Overwrite newest-wins", run.mismatches.empty(),
                 std::to_string(overwrite_count) + " events overwritten; model comparison still matches.");
}

void delete_events(VerificationRun& run, KVStore& db, int count) {
    const int delete_count = std::max(1, count / 20);
    for (int i = 0; i < delete_count; ++i) {
        int idx = count - 1 - i;
        db.delete_key(event_key(idx));
    }
    for (int i = 0; i < delete_count; ++i) {
        int idx = count - 1 - i;
        std::string value;
        if (db.get(event_key(idx), value)) run.deleted_found++;
        run.model.erase(event_key(idx));
        run.deleted_checked++;
    }
    run.add_step("Delete tombstones", run.deleted_found == 0,
                 std::to_string(run.deleted_checked) + " deleted events checked; " +
                 std::to_string(run.deleted_found) + " incorrectly found.");
}

void flush_events(VerificationRun& run, KVStore& db) {
    run.l0_before_flush = static_cast<uint64_t>(db.l0_count());
    db.force_flush();
    run.l0_after_flush = static_cast<uint64_t>(db.l0_count());
    run.post_flush_checked = run.verify_model(db, "after forced flush");
    run.add_step("Memtable flush to SSTable",
                 run.l0_after_flush > run.l0_before_flush && run.mismatches.empty(),
                 "L0 count changed from " + std::to_string(run.l0_before_flush) +
                 " to " + std::to_string(run.l0_after_flush) + "; all live keys still match.");
}

void compaction_events(VerificationRun& run, KVStore& db, int base_count) {
    for (int batch = 0; batch < 4; ++batch) {
        for (int j = 0; j < 250; ++j) {
            int seq = base_count + batch * 250 + j;
            std::string key = event_key(seq);
            std::string value = event_value(seq, 1);
            db.put(key, value);
            run.model[key] = value;
        }
        db.force_flush();
    }

    run.l0_before_compaction = static_cast<uint64_t>(db.l0_count());
    run.l1_before_compaction = static_cast<uint64_t>(db.l1_count());
    db.force_compaction();
    run.l0_after_compaction = static_cast<uint64_t>(db.l0_count());
    run.l1_after_compaction = static_cast<uint64_t>(db.l1_count());
    run.post_compaction_checked = run.verify_model(db, "after compaction");
    run.add_step("Compaction preserves keys",
                 run.l0_after_compaction < run.l0_before_compaction && run.mismatches.empty(),
                 "L0 changed " + std::to_string(run.l0_before_compaction) + " -> " +
                 std::to_string(run.l0_after_compaction) + ", L1 changed " +
                 std::to_string(run.l1_before_compaction) + " -> " +
                 std::to_string(run.l1_after_compaction) + ".");
}

void bloom_events(VerificationRun& run, KVStore& db) {
    run.missing_queries = 500;
    db.metrics().reset();
    const uint64_t model_size = std::max<uint64_t>(1, static_cast<uint64_t>(run.model.size()));
    for (uint64_t i = 0; i < run.missing_queries; ++i) {
        std::string ignored;
        db.get(event_key(static_cast<int>(i % model_size)) + ":missing", ignored);
    }
    run.bloom_skips = db.metrics().bloom_skips;
    run.sst_searches = db.metrics().sst_searches;
    run.add_step("Bloom missing-key skip path", run.bloom_skips > 0,
                 std::to_string(run.missing_queries) + " missing-key lookups produced " +
                 std::to_string(run.bloom_skips) + " Bloom skips and " +
                 std::to_string(run.sst_searches) + " SST searches.");
}

void gc_events(VerificationRun& run, KVStore& db, const std::string& dir, int count) {
    const int overwrite_count = std::max(1, count / 2);
    for (int i = 0; i < overwrite_count; ++i) {
        std::string key = event_key(i);
        std::string value = event_value(i, 3);
        db.put(key, value);
        run.model[key] = value;
    }
    run.vlog_before_gc = file_size_or_zero(dir + "/vlog.bin");
    run_vlog_gc(&db);
    run.vlog_after_gc = file_size_or_zero(dir + "/vlog.bin");
    run.post_gc_checked = run.verify_model(db, "after VLog GC");
    run.add_step("VLog GC preserves live values",
                 run.vlog_after_gc < run.vlog_before_gc && run.mismatches.empty(),
                 "VLog changed from " + std::to_string(run.vlog_before_gc) +
                 " to " + std::to_string(run.vlog_after_gc) +
                 " bytes; crash safety remains labeled partial.");
}

} // namespace

std::string run_verification(const VerificationOptions& options) {
    VerificationRun run;
    run.test = options.test.empty() ? "full" : options.test;
    run.ops = std::clamp(options.ops, 100, 50000);

    const std::string verify_dir = "flsm_verify_lab";

    try {
        std::error_code ec;
        std::filesystem::remove_all(verify_dir, ec);

        if (run.test == "gc") {
            {
                KVStore db(verify_dir);
                insert_events(run, db, run.ops);
                gc_events(run, db, verify_dir, run.ops);
            }
            std::filesystem::remove_all(verify_dir, ec);
            return run.to_json();
        }

        {
            KVStore db(verify_dir);
            insert_events(run, db, run.ops);

            if (wants(run.test, "overwrite") || wants(run.test, "delete") ||
                wants(run.test, "flush") || wants(run.test, "compaction") ||
                wants(run.test, "bloom") || wants(run.test, "recovery")) {
                overwrite_events(run, db, run.ops);
            }

            if (wants(run.test, "delete") || wants(run.test, "flush") ||
                wants(run.test, "compaction") || wants(run.test, "bloom") ||
                wants(run.test, "recovery")) {
                delete_events(run, db, run.ops);
            }

            if (wants(run.test, "flush") || wants(run.test, "compaction") ||
                wants(run.test, "bloom") || wants(run.test, "recovery")) {
                flush_events(run, db);
            }

            if (wants(run.test, "compaction") || wants(run.test, "bloom") ||
                wants(run.test, "recovery")) {
                compaction_events(run, db, run.ops);
            }

            if (wants(run.test, "bloom")) {
                bloom_events(run, db);
            }
        }

        if (wants(run.test, "recovery")) {
            KVStore recovered(verify_dir);
            run.post_recovery_checked = run.verify_model(recovered, "after restart recovery");
            run.add_step("Restart recovery", run.mismatches.empty(),
                         std::to_string(run.post_recovery_checked) +
                         " live keys verified after reopening the database.");
        }

        std::filesystem::remove_all(verify_dir, ec);
    } catch (const std::exception& e) {
        run.ok = false;
        run.mismatch(std::string("exception: ") + e.what());
        run.add_step("Verification exception", false, e.what());
    }

    return run.to_json();
}
