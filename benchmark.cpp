#include "angela.hpp"
#include "liveUpdates2.hpp"
#include "merkleTree.hpp"
#include "utils.hpp"
#include "workLoad.hpp"

using namespace std;
using namespace std::chrono;

// Live Algorithm Thread Pool
template <typename TreeType, typename Algo>
class LiveThreadPool {
public:
    TreeType &tree;
    Algo &algo;

    int numThreads;
    atomic<bool> stop{false};
    atomic<int> processed{0};
    queue<WorkloadEvent> q;
    mutex q_mtx;
    condition_variable cv;
    vector<thread> workers;

    // response times are stored here
    vector<long long> response_times;
    mutex rt_mtx;

    static thread_local int update_counter;

    LiveThreadPool(TreeType &t, Algo &a, int threads)
        : tree(t), algo(a), numThreads(threads) {
        workers.reserve(threads);
        for (int i = 0; i < threads; i++)
            workers.emplace_back(&LiveThreadPool::worker, this, i);
    }

    ~LiveThreadPool() {
        {
            unique_lock<mutex> lk(q_mtx);
            stop = true;
        }
        cv.notify_all();

        for (auto &t : workers)
            if (t.joinable())
                t.join();
    }

    void enqueue(const OperationRequest &op, long long arrival_us) {
        {
            unique_lock<mutex> lk(q_mtx);
            q.push({op, arrival_us});
        }
        cv.notify_one();
    }

    void worker(int thread_id) {
        while (true) {
            WorkloadEvent job;

            // Wait for job
            {
                unique_lock<mutex> lk(q_mtx);
                cv.wait(lk, [&] { return !q.empty() || stop; });

                if (stop && q.empty())
                    return;

                job = q.front();
                q.pop();
            }

            long long start_exec = now_us();

            if (job.op.op_type == UPDATE) {
                update_counter++;
                ThreadUpdateId tid(thread_id);
                tid.update_count = update_counter;
                algo.update(tree, job.op.key, job.op.value, tid);
            } else if (job.op.op_type == READ_ROOT) {
                tree.getRootHash();
            } else if (job.op.op_type == READ_LEAF) {
                tree.getLeafNode(job.op.key);
            }

            long long finish = now_us();
            long long response = finish - job.arrival_us;

            {
                lock_guard<mutex> lg(rt_mtx);
                response_times.push_back(response);
            }

            processed++;
        }
    }
};

template <typename T, typename A>
thread_local int LiveThreadPool<T, A>::update_counter = 0;

// ===============================================================
//                         BENCHMARK MAIN
// ===============================================================

int main() {
    int depth = 10;
    int total_ops = 50000;
    int numThreads = 8;
    int batch_size = 200;
    double read_percent = 0;

    cout << "Benchmark Merkle Trees (Live vs Angela)\n";
    cout << "Enter tree depth, batch size, number of threads, and total operations: ";
    cin >> depth >> batch_size >> numThreads >> total_ops;

    cout << "Depth=" << depth << "  Ops=" << total_ops
         << "  Threads=" << numThreads << endl;

    // ===========================================================
    // 1. Generate workload with timestamps
    // ===========================================================
    cout << "\nGenerating workload...\n";
    vector<WorkloadEvent> stream = generate_workload(depth, total_ops, read_percent);

    cout << "Workload generated.\n";

    // ===========================================================
    // 2. Run Live Algorithm (real-time playback)
    // ===========================================================
    cout << "\nRunning Live Algorithm...\n";

    SparseMerkleTree<LiveUpdatesNode> liveTree(depth);
    LiveAlgorithm liveAlgo;

    LiveThreadPool<SparseMerkleTree<LiveUpdatesNode>, LiveAlgorithm> pool(liveTree, liveAlgo, numThreads);

    long long playback_start = now_us();

    for (auto &evt : stream) {
        long long target = playback_start + evt.arrival_us;
        while (now_us() < target)
            this_thread::sleep_for(50ns);

        pool.enqueue(evt.op, evt.arrival_us);
    }

    // Close pool
    {
        unique_lock<mutex> lk(pool.q_mtx);
        pool.stop = true;
    }
    pool.cv.notify_all();

    for (auto &t : pool.workers)
        if (t.joinable())
            t.join();

    cout << "Live Algorithm finished.\n";

    // ===========================================================
    // 3. Run Angela Algorithm (batch)
    // ===========================================================
    cout << "\nRunning Angela Algorithm in batches (batch_size = " << batch_size << ")...\n";

    SparseMerkleTree<AngelaNode> angelaTree(depth);
    AngelaAlgorithm angela;

    vector<pair<string, string>> current_batch;
    current_batch.reserve(batch_size);

    // We'll store the arrival_us for each update in the batch so we can compute response times.
    vector<long long> current_batch_arrival_us;
    current_batch_arrival_us.reserve(batch_size);

    vector<long long> angela_rt; // per-request response times for Angela
    angela_rt.reserve((size_t)total_ops);

    long long angela_total_ms_sum = 0; // sum of each batch's ms
    long long angela_first_batch_start = 0;
    long long angela_last_batch_finish = 0;
    bool first_batch = true;

    // Iterate the stream in order and form batches of UPDATE ops only
    for (auto &evt : stream) {
        if (evt.op.op_type != UPDATE)
            continue;

        current_batch.emplace_back(evt.op.key, evt.op.value);
        current_batch_arrival_us.push_back(evt.arrival_us);

        if ((int)current_batch.size() >= batch_size) {
            // process this batch
            long long batch_start = now_us();
            if (first_batch) {
                angela_first_batch_start = batch_start;
                first_batch = false;
            }

            long long batch_ms = angela.processBatch(angelaTree, current_batch, numThreads);
            angela_total_ms_sum += batch_ms;
            long long batch_finish = batch_start + batch_ms * 1000LL;
            angela_last_batch_finish = batch_finish;

            // record per-request response times for items in this batch
            for (size_t i = 0; i < current_batch.size(); ++i) {
                long long arrival = current_batch_arrival_us[i];
                long long resp = batch_finish - arrival;
                angela_rt.push_back(resp);
            }

            // clear batch
            current_batch.clear();
            current_batch_arrival_us.clear();
        }
    }

    // Process final (possibly smaller) batch if present
    if (!current_batch.empty()) {
        long long batch_start = now_us();
        if (first_batch) {
            angela_first_batch_start = batch_start;
            first_batch = false;
        }

        long long batch_ms = angela.processBatch(angelaTree, current_batch, numThreads);
        angela_total_ms_sum += batch_ms;
        long long batch_finish = batch_start + batch_ms * 1000LL;
        angela_last_batch_finish = batch_finish;

        for (size_t i = 0; i < current_batch.size(); ++i) {
            long long arrival = current_batch_arrival_us[i];
            long long resp = batch_finish - arrival;
            angela_rt.push_back(resp);
        }
        current_batch.clear();
        current_batch_arrival_us.clear();
    }

    // Optionally compute total wall-clock time taken for Angela batches
    long long angela_wall_clock_ms = 0;
    if (!first_batch) {
        angela_wall_clock_ms = (angela_last_batch_finish - angela_first_batch_start) / 1000LL; // in ms
    }
    cout << "Angela: processed " << angela_rt.size() << " updates in batches.\n";
    cout << "Angela sum(batch_ms) = " << angela_total_ms_sum << " ms (sum of per-batch reported times)\n";
    cout << "Angela wall-clock (first batch start -> last batch finish) = " << angela_wall_clock_ms << " ms\n";

    // ===========================================================
    // 4. Run Serial Algorithm
    // ===========================================================
    cout << "\nRunning Serial Algorithm...\n";

    SparseMerkleTree<MerkleNode> serialTree(depth);

    vector<long long> serial_rt;
    serial_rt.reserve(total_ops);

    long long serial_start = now_us();

    for (auto &evt : stream) {
        long long arrival = evt.arrival_us;

        long long op_start = now_us();

        if (evt.op.op_type == UPDATE) {
            updateSerial(serialTree, evt.op.key, evt.op.value);
        } else if (evt.op.op_type == READ_ROOT) {
            serialTree.getRootHash();
        } else if (evt.op.op_type == READ_LEAF) {
            serialTree.getLeafNode(evt.op.key);
        }

        long long op_finish = now_us();
        serial_rt.push_back(op_finish - arrival);
    }

    long long serial_finish = now_us();
    long long serial_total_ms = (serial_finish - serial_start) / 1000;

    cout << "Serial Algorithm finished in " << serial_total_ms << " ms\n";

    // ===========================================================
    // 5. Summary & Stats
    // ===========================================================
    cout << "\n===== RESULTS =====\n\n";

    auto &live_rt = pool.response_times;

    cout << "Live: " << live_rt.size() << " responses\n";
    cout << "Angela: " << angela_rt.size() << " responses\n\n";

    cout << "Live Avg RT (us)   : " << accumulate(live_rt.begin(), live_rt.end(), 0LL) / live_rt.size() << endl;
    cout << "Angela Avg RT (us) : " << accumulate(angela_rt.begin(), angela_rt.end(), 0LL) / angela_rt.size() << endl;
    cout << "\nSerial Avg RT (us)   : " << accumulate(serial_rt.begin(), serial_rt.end(), 0LL) / serial_rt.size() << endl;

    cout << "\nPercentiles:\n";
    cout << "   Live P50:  " << percentile(live_rt, 0.50) << " us\n";
    cout << "   Live P90:  " << percentile(live_rt, 0.90) << " us\n";
    cout << "   Live P99:  " << percentile(live_rt, 0.99) << " us\n";

    cout << " Angela P50:  " << percentile(angela_rt, 0.50) << " us\n";
    cout << " Angela P90:  " << percentile(angela_rt, 0.90) << " us\n";
    cout << " Angela P99:  " << percentile(angela_rt, 0.99) << " us\n";

    cout << "   Serial P50:  " << percentile(serial_rt, 0.50) << " us\n";
    cout << "   Serial P90:  " << percentile(serial_rt, 0.90) << " us\n";
    cout << "   Serial P99:  " << percentile(serial_rt, 0.99) << " us\n";

    // ===========================================================
    // 6. Root Hash Verification
    // ===========================================================
    cout << "\n===== ROOT HASH VERIFICATION =====\n";

    string live_root = liveTree.getRootHash();
    string angela_root = angelaTree.getRootHash();
    string serial_root = serialTree.getRootHash();

    cout << "Live Root   : " << live_root << endl;
    cout << "Angela Root : " << angela_root << endl;
    cout << "Serial Root : " << serial_root << endl;

    bool live_ok = (live_root == serial_root);
    bool angela_ok = (angela_root == serial_root);

    cout << "\nLive vs Serial   : " << (live_ok ? "MATCH ✓" : "MISMATCH ✗") << endl;
    cout << "Angela vs Serial : " << (angela_ok ? "MATCH ✓" : "MISMATCH ✗") << endl;

    // ===========================================================
    // 7. Dump CSV files for plotting
    // ===========================================================
    dump_csv("live_response_times.csv", live_rt);
    dump_csv("angela_response_times.csv", angela_rt);
    dump_csv("serial_response_times.csv", serial_rt);

    cout << "\nCSV files written:\n";
    cout << "  live_response_times.csv\n";
    cout << "  angela_response_times.csv\n";
    cout << "  serial_response_times.csv\n";

    cout << "\nDone.\n";
    return 0;
}
