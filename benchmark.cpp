#include "angela.hpp"
#include "liveUpdates.hpp"
#include "merkleTree.hpp"
#include "utils.hpp"
#include "workLoad.hpp"

#include <iomanip>
#include <numeric>

using namespace std;
using namespace std::chrono;

// =========================================================
// GLOBAL TIMING BASELINE
// All timestamps = now_us() - workload_start
// =========================================================
long long workload_start = 0;

// =========================================================
// LiveThreadPool — corrected timing: NO NEGATIVES EVER
// =========================================================
template <typename TreeType, typename Algo>
class LiveThreadPool {
public:
    TreeType &tree;
    Algo &algo;

    int numThreads;
    atomic<bool> stop{false};
    queue<WorkloadEvent> q;
    mutex q_mtx;
    condition_variable cv;
    vector<thread> workers;

    vector<vector<long long>> response_times_per_thread;
    static thread_local int update_counter;

    LiveThreadPool(TreeType &t, Algo &a, int threads)
        : tree(t), algo(a), numThreads(threads) {
        response_times_per_thread.resize(threads);
        workers.reserve(threads);
        for (int i = 0; i < threads; i++)
            workers.emplace_back(&LiveThreadPool::worker, this, i);
    }

    ~LiveThreadPool() {
        {
            lock_guard<mutex> lk(q_mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto &w : workers)
            if (w.joinable())
                w.join();
    }

    void enqueue(const OperationRequest &op, long long arrival_us) {
        lock_guard<mutex> lk(q_mtx);
        q.push({op, arrival_us});
        cv.notify_one();
    }

    void worker(int tid) {
        while (true) {
            WorkloadEvent job;

            // wait for job
            {
                unique_lock<mutex> lk(q_mtx);
                cv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop && q.empty())
                    return;

                job = q.front();
                q.pop();
            }

            // -----------------------------------------------------
            // CORRECT TIMING:
            // exec_start and finish_us both relative to workload_start
            // -----------------------------------------------------
            long long exec_start = now_us() - workload_start;

            if (job.op.op_type == UPDATE) {
                update_counter++;
                ThreadUpdateId id(tid);
                id.update_count = update_counter;
                algo.update(tree, job.op.key, job.op.value, id);
            } else if (job.op.op_type == READ_ROOT) {
                tree.getRootHash();
            } else if (job.op.op_type == READ_LEAF) {
                tree.getLeafNode(job.op.key);
            }

            long long finish_us = now_us() - workload_start;
            long long resp = finish_us - job.arrival_us;

            response_times_per_thread[tid].push_back(resp);
        }
    }
};

template <typename T, typename A>
thread_local int LiveThreadPool<T, A>::update_counter = 0;

// ===============================================================
//                          MAIN
// ===============================================================
int main() {
    int depth = 10;
    int total_ops = 50000;
    int numThreads = 8;
    int batch_size = 200;
    double read_percent = 0;

    cout << "Benchmark Merkle Trees (Live vs Angela)\n";
    cout << "Enter depth, batch_size, threads, total_ops: ";
    cin >> depth >> batch_size >> numThreads >> total_ops;

    cout << "Depth=" << depth << " Threads=" << numThreads
         << " Ops=" << total_ops << endl;

    // -----------------------------------------------------------
    // Generate workload
    // -----------------------------------------------------------
    cout << "\nGenerating workload...\n";
    workload_start = now_us(); // ZERO POINT for all timestamps

    vector<WorkloadEvent> stream =
        generate_workload(depth, total_ops, read_percent, workload_start);

    cout << "Workload generated.\n";

    // ===========================================================
    // 2. LIVE ALGORITHM (real-time playback)
    // ===========================================================
    cout << "\nRunning Live Algorithm...\n";

    SparseMerkleTree<LiveUpdatesNode> liveTree(depth);
    LiveAlgorithm liveAlgo;

    LiveThreadPool pool(liveTree, liveAlgo, numThreads);

    long long playback_start = now_us();

    for (auto &evt : stream) {
        long long target_us = workload_start + evt.arrival_us;
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

    long long live_total_ms =
        (now_us() - playback_start) / 1000;
    cout << "Live finished in " << live_total_ms << " ms\n";

    // ===========================================================
    // 3. ANGELA (batch)
    // ===========================================================
    cout << "\nRunning Angela Algorithm...\n";

    SparseMerkleTree<AngelaNode> angelaTree(depth);
    AngelaAlgorithm angela;

    vector<long long> angela_rt;
    angela_rt.reserve(total_ops);

    vector<pair<string, string>> batch;
    vector<long long> batch_arrivals;
    batch.reserve(batch_size);
    batch_arrivals.reserve(batch_size);

    long long angela_batch_start, angela_batch_finish;
    long long exec_start = now_us() - workload_start;

    for (auto &evt : stream) {
        if (evt.op.op_type != UPDATE)
            continue;

        batch.emplace_back(evt.op.key, evt.op.value);
        batch_arrivals.push_back(evt.arrival_us);

        if ((int)batch.size() == batch_size) {
            angela_batch_start = now_us() - workload_start;

            long long ms = angela.processBatch(angelaTree, batch, numThreads);
            angela_batch_finish = now_us() - workload_start;

            for (size_t i = 0; i < batch.size(); i++)
                angela_rt.push_back(angela_batch_finish - exec_start - batch_arrivals[i]);

            batch.clear();
            batch_arrivals.clear();
        }
    }

    if (!batch.empty()) {
        long long s = now_us() - workload_start;
        long long ms = angela.processBatch(angelaTree, batch, numThreads);
        long long f = now_us() - workload_start;

        for (size_t i = 0; i < batch.size(); i++)
            angela_rt.push_back(f - batch_arrivals[i]);
    }

    cout << "Angela processed " << angela_rt.size() << " updates.\n";

    // ===========================================================
    // 4. SERIAL
    // ===========================================================
    cout << "\nRunning Serial Algorithm...\n";

    SparseMerkleTree<MerkleNode> serialTree(depth);
    vector<long long> serial_rt;
    serial_rt.reserve(total_ops);

    exec_start = now_us() - workload_start;
    for (auto &evt : stream) {

        if (evt.op.op_type == UPDATE)
            updateSerial(serialTree, evt.op.key, evt.op.value);
        else if (evt.op.op_type == READ_ROOT)
            serialTree.getRootHash();
        else
            serialTree.getLeafNode(evt.op.key);

        long long finish_us = now_us() - workload_start;
        serial_rt.push_back(finish_us - exec_start - evt.arrival_us);
    }

    cout << "Serial done.\n";

    // ===========================================================
    // 5. SUMMARY TABLE
    // ===========================================================
    cout << "\n==== RESULTS ====\n";

    vector<long long> live_rt;
    for (auto &vec : pool.response_times_per_thread)
        live_rt.insert(live_rt.end(), vec.begin(), vec.end());

    auto avg_live = accumulate(live_rt.begin(), live_rt.end(), 0LL) / (double)live_rt.size();
    auto avg_angela = accumulate(angela_rt.begin(), angela_rt.end(), 0LL) / (double)angela_rt.size();
    auto avg_serial = accumulate(serial_rt.begin(), serial_rt.end(), 0LL) / (double)serial_rt.size();

    cout << fixed << setprecision(2);
    cout << "Live Avg    : " << avg_live << " us\n";
    cout << "Angela Avg  : " << avg_angela << " us\n";
    cout << "Serial Avg  : " << avg_serial << " us\n";

    // Write CSV summary
    ofstream summary("summary_metrics.csv");
    summary << "depth,threads,batch,ops,avg_live,avg_angela,avg_serial\n";
    summary << depth << "," << numThreads << "," << batch_size << ","
            << total_ops << ","
            << avg_live << ","
            << avg_angela << ","
            << avg_serial << "\n";

    cout << "Wrote summary_metrics.csv\n";
    cout << "Done.\n";

    // ===========================================================
    // 7. ROOT HASH VERIFICATION
    // ===========================================================
    cout << "\n=============================================\n";
    cout << "            ROOT HASH VERIFICATION           \n";
    cout << "=============================================\n";

    string live_root = liveTree.getRootHash();
    string angela_root = angelaTree.getRootHash();
    string serial_root = serialTree.getRootHash();

    cout << "Live Root   : " << live_root << "\n";
    cout << "Angela Root : " << angela_root << "\n";
    cout << "Serial Root : " << serial_root << "\n\n";

    cout << "Live   vs Serial : "
         << (live_root == serial_root ? "MATCH ✓" : "✗ MISMATCH") << "\n";

    cout << "Angela vs Serial : "
         << (angela_root == serial_root ? "MATCH ✓" : "✗ MISMATCH") << "\n";

    cout << "=============================================\n\n";

    // ===========================================================
    // 8. WRITE CSV FILES FOR PLOTTING
    // ===========================================================
    dump_csv("live_response_times.csv", live_rt);
    dump_csv("angela_response_times.csv", angela_rt);
    dump_csv("serial_response_times.csv", serial_rt);

    cout << "CSV files written:\n";
    cout << "   live_response_times.csv\n";
    cout << "   angela_response_times.csv\n";
    cout << "   serial_response_times.csv\n";
    cout << "   summary_metrics.csv\n";

    cout << "\nDone.\n";
    return 0;
}
