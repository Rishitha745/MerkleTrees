#pragma once
#include "merkleTree.hpp"

// Enum to differentiate operation types
enum OperationType { UPDATE,
                     READ_ROOT,
                     READ_LEAF };

// Struct for operation requests (update or read)
struct OperationRequest {
    OperationType op_type;
    string key;   // For update or read_leaf
    string value; // For update

    OperationRequest(OperationType t, const string &k = "", const string &v = "")
        : op_type(t), key(k), value(v) {}
};

// Random operation generator
OperationRequest generate_random_operation(int tree_depth, double read_percentage, const vector<string> &leaf_keys) {
    double p = (rand() % 10000) / 100.0;
    if (p < read_percentage) {
        if (rand() % 2) {
            return OperationRequest(READ_ROOT);
        } else {
            string random_leaf = leaf_keys[rand() % leaf_keys.size()];
            return OperationRequest(READ_LEAF, random_leaf);
        }
    } else {
        string key;
        for (int i = 0; i < tree_depth; ++i)
            key += (rand() % 2) ? '1' : '0';
        string value = to_string(rand() % 1000);
        return OperationRequest(UPDATE, key, value);
    }
}

// A timed workload event
struct WorkloadEvent {
    long long arrival_us; // Timestamp when this request arrives
    OperationRequest op;  // Operation (update, read root, read leaf)

    WorkloadEvent() : op(OperationRequest(UPDATE)), arrival_us(0) {}

    WorkloadEvent(OperationRequest o, long long t)
        : op(o), arrival_us(t) {}
};

vector<WorkloadEvent> generate_workload(
    int depth,
    int total_ops,
    double read_percent,
    long long workload_start) {
    vector<WorkloadEvent> stream;
    stream.reserve(total_ops);

    // ----------------------------------------------------------
    // Generate full list of leaf keys (fixed for any depth)
    // ----------------------------------------------------------
    vector<string> leaf_keys;
    leaf_keys.reserve(1 << depth);

    for (int i = 0; i < (1 << depth); ++i) {
        string k = bitset<32>(i).to_string().substr(32 - depth);
        leaf_keys.push_back(k);
    }

    // ----------------------------------------------------------
    // RNG for inter-arrival time
    // ----------------------------------------------------------
    default_random_engine rng(random_device{}());

    // Log-normal distribution gives bursty values like real mempool traffic
    double mean_gap_us = 2000.0; // average 2 ms
    double sigma = 0.5;          // burstiness
    lognormal_distribution<double> gap_dist(log(mean_gap_us), sigma);

    for (int i = 0; i < total_ops; i++) {

        OperationRequest op =
            generate_random_operation(depth, read_percent, leaf_keys);

        long long arrival_us = now_us() - workload_start;
        stream.emplace_back(op, arrival_us);

        double gap_us = gap_dist(rng);

        // clamp to avoid unrealistic values
        if (gap_us < 200.0)
            gap_us = 200.0; // 0.2 ms minimum
        if (gap_us > 20000.0)
            gap_us = 20000.0; // 20 ms max allowed

        this_thread::sleep_for(chrono::microseconds((int)gap_us));
    }

    return stream;
}
