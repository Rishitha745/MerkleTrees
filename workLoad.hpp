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

vector<WorkloadEvent> generate_workload(int depth, int total_ops, int read_percent) {
    vector<WorkloadEvent> stream;
    stream.reserve(total_ops);

    vector<string> leaf_keys;
    for (int i = 0; i < (1 << depth); ++i) {
        string k = bitset<32>(i).to_string().substr(32 - depth);
        leaf_keys.push_back(k);
    }

    default_random_engine rng(random_device{}());
    exponential_distribution<double> exp_dist(1.0 / 20.0);
    auto gen_start = now_us();

    for (int i = 0; i < total_ops; i++) {
        OperationRequest op = generate_random_operation(depth, read_percent, leaf_keys);
        long long t = now_us() - gen_start;
        stream.emplace_back(op, t); 

        // random inter-arrival time
        double gap_us = exp_dist(rng); // µs
        this_thread::sleep_for(chrono::microseconds((int)gap_us));

        // fixed sleep time
        // this_thread::sleep_for(1us);
    }

    return stream;
}