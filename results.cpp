#include "liveUpdates.hpp"
#include "merkleTree.hpp"
#include "utils.hpp"
#include "workLoad.hpp"

struct Result {
    long long avg_live, avg_angela, avg_serial;
    long long exec_live, exec_angela, exec_serial;
    string live_root, angela_root, serial_root;
};

template <typename T>
Result run_benchmark(
    int depth,
    int total_ops,
    int numThreads,
    int batch_size,
    const vector<WorkloadEvent> &workload) {
    Result R;

    // =============================
    // 1. LIVE ALGORITHM
    // =============================
    SparseMerkleTree<LiveUpdatesNode> liveTree(depth);
    LiveAlgorithm liveAlgo;

    LiveThreadPool pool(liveTree, liveAlgo, numThreads);

    long long playback_start = now_us();
    pool.playback_start_time = playback_start;

    for (auto &evt : stream) {
        long long target_us = playback_start + evt.arrival_us;
        while (now_us() < target_us)
            this_thread::sleep_for(50ns);

        pool.enqueue(evt.op, evt.arrival_us);
    }

    // stop pool
    {
        lock_guard<mutex> lk(pool.q_mtx);
        pool.stop = true;
    }
    pool.cv.notify_all();
    for (auto &t : pool.workers)
        if (t.joinable())
            t.join();

    long long live_finish = now_us();
    R.exec_live = (live_finish - playback_start) / 1000; // ms

    vector<long long> live_rt;
    for (auto &vec : pool.response_times_per_thread)
        live_rt.insert(live_rt.end(), vec.begin(), vec.end());

    R.avg_live = accumulate(live_rt.begin(), live_rt.end(), 0LL) /
                 (double)live_rt.size();

    R.live_root = liveTree.getRootHash();

    // =============================
    // 2. ANGELA ALGORITHM (BATCHED)
    // =============================
    SparseMerkleTree<AngelaNode> angelaTree(depth);
    AngelaAlgorithm angela;

    vector<long long> angela_rt;
    angela_rt.reserve(total_ops);

    vector<pair<string, string>> batch;
    vector<long long> batch_arr;
    batch.reserve(batch_size);
    batch_arr.reserve(batch_size);

    long long exec_start = now_us();

    for (auto &evt : stream) {
        if (evt.op.op_type != UPDATE)
            continue;

        long long target_us = exec_start + evt.arrival_us;
        while (now_us() < target_us)
            this_thread::sleep_for(50ns);

        batch.emplace_back(evt.op.key, evt.op.value);
        batch_arr.push_back(evt.arrival_us);

        if ((int)batch.size() == batch_size) {
            long long start = now_us();
            angela.processBatch(angelaTree, batch, numThreads);
            long long finish = now_us();

            for (size_t i = 0; i < batch.size(); i++)
                angela_rt.push_back(finish - exec_start - batch_arr[i]);

            batch.clear();
            batch_arr.clear();
        }
    }

    if (!batch.empty()) {
        long long start = now_us();
        angela.processBatch(angelaTree, batch, numThreads);
        long long finish = now_us();

        for (size_t i = 0; i < batch.size(); i++)
            angela_rt.push_back(finish - exec_start - batch_arr[i]);
    }

    long long angela_finish = now_us();
    R.exec_angela = (angela_finish - exec_start) / 1000; // ms
    R.avg_angela = accumulate(angela_rt.begin(), angela_rt.end(), 0LL) /
                   (double)angela_rt.size();

    R.angela_root = angelaTree.getRootHash();

    // =============================
    // 3. SERIAL ALGORITHM
    // =============================
    SparseMerkleTree<MerkleNode> serialTree(depth);

    vector<long long> serial_rt;
    serial_rt.reserve(total_ops);

    exec_start = now_us();

    for (auto &evt : stream) {
        long long target_us = exec_start + evt.arrival_us;
        while (now_us() < target_us)
            this_thread::sleep_for(50ns);

        if (evt.op.op_type == UPDATE)
            updateSerial(serialTree, evt.op.key, evt.op.value);
        else if (evt.op.op_type == READ_ROOT)
            serialTree.getRootHash();
        else
            serialTree.getLeafNode(evt.op.key);

        long long finish = now_us();
        serial_rt.push_back(finish - exec_start - evt.arrival_us);
    }

    long long serial_finish = now_us();
    R.exec_serial = (serial_finish - exec_start) / 1000; // ms
    R.avg_serial = accumulate(serial_rt.begin(), serial_rt.end(), 0LL) /
                   (double)serial_rt.size();

    R.serial_root = serialTree.getRootHash();

    return R;
}

int main() {

    int total_ops = 100000;
    int batch_size = 1024;
    double read_percent = 0;

    // ================================
    // EXPERIMENT 1: Fix depth=16, Vary threads
    // ================================
    vector<int> thread_list = {2, 4, 8, 16, 32, 64};

    cout << "Generating workload ONCE for depth=16...\n";
    workload_start = now_us();
    vector<WorkloadEvent> workload_depth16 =
        generate_workload(16, total_ops, read_percent, workload_start);

    ofstream csv1("threads_depth16_results.csv");
    csv1 << "threads,avg_live,avg_angela,avg_serial,"
            "exec_live,exec_angela,exec_serial\n";

    for (int th : thread_list) {
        cout << "\nRunning depth=16 threads=" << th << "...\n";

        Result r = run_benchmark(
            16, total_ops, th, batch_size, workload_depth16);

        csv1 << th << ","
             << r.avg_live << "," << r.avg_angela << "," << r.avg_serial << ","
             << r.exec_live << "," << r.exec_angela << "," << r.exec_serial
             << "\n";
    }

    csv1.close();

    // ================================
    // EXPERIMENT 2: Fix threads=32, Vary depth
    // ================================
    vector<int> depth_list = {12, 16, 20, 24};

    ofstream csv2("depth_threads32_results.csv");
    csv2 << "depth,avg_live,avg_angela,avg_serial,"
            "exec_live,exec_angela,exec_serial\n";

    for (int depth : depth_list) {
        cout << "\nGenerating workload for depth=" << depth << "...\n";

        workload_start = now_us();
        vector<WorkloadEvent> workload_d =
            generate_workload(depth, total_ops, read_percent, workload_start);

        cout << "Running depth=" << depth << " threads=32...\n";

        Result r = run_benchmark(depth, total_ops, 32, batch_size, workload_d);

        csv2 << depth << ","
             << r.avg_live << "," << r.avg_angela << "," << r.avg_serial << ","
             << r.exec_live << "," << r.exec_angela << "," << r.exec_serial
             << "\n";
    }

    csv2.close();

    cout << "\nAll experiments completed.\n";
    return 0;
}
