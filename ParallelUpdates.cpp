#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <openssl/sha.h>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

// Global stop vector for tracking when threads should stop
static constexpr int MAX_THREADS = 64; // Fixed size for stop_vector
static vector<atomic<int>> stop_vector(MAX_THREADS);

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

// Structure for Update IDs
struct ThreadUpdateId {
    int thread_index;
    int update_count;

    ThreadUpdateId(int index = -1) : thread_index(index), update_count(0) {}

    string to_string() const {
        return std::to_string(thread_index) + "_" + std::to_string(update_count);
    }

    bool operator==(const ThreadUpdateId &other) const {
        return thread_index == other.thread_index && update_count == other.update_count;
    }
};

// Structure for MerkleTree nodes
struct MerkleNode {
    string hash;
    MerkleNode *left;
    MerkleNode *right;
    MerkleNode *parent;
    ThreadUpdateId last_updated_thread_index;
    ThreadUpdateId left_child_thread_index;
    ThreadUpdateId right_child_thread_index;
    bool is_leaf;
    mutex node_mutex;
    string key;

    MerkleNode(bool leaf = false)
        : hash(), left(nullptr), right(nullptr), parent(nullptr),
          last_updated_thread_index(), left_child_thread_index(), right_child_thread_index(),
          is_leaf(leaf), key("") {}

    ~MerkleNode() {
        delete left;
        delete right;
    }
};

// Hash function using SHA256
string computeHash(const string &data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.c_str()), data.length(), hash);
    string result;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        result += buf;
    }
    return result;
}

class SparseMerkleTree {
private:
    MerkleNode *root;
    int depth;
    const string default_leaf_hash = computeHash("");
    unordered_map<string, MerkleNode *> leaf_nodes;

    MerkleNode *buildCompleteTree(int current_depth, MerkleNode *parent = nullptr, string current_path = "") {
        MerkleNode *node = new MerkleNode(current_depth == 0);
        node->key = current_path;
        node->parent = parent;

        if (current_depth == 0) {
            node->hash = default_leaf_hash;
            node->is_leaf = true;
            leaf_nodes[current_path] = node;
        } else {
            node->left = buildCompleteTree(current_depth - 1, node, current_path + "0");
            node->right = buildCompleteTree(current_depth - 1, node, current_path + "1");
            string left_hash = node->left->hash;
            string right_hash = node->right->hash;
            node->hash = computeHash(left_hash + right_hash);
        }
        return node;
    }

public:
    SparseMerkleTree(int tree_depth) : depth(tree_depth) {
        if (tree_depth < 0) {
            throw runtime_error("Tree depth must be non-negative");
        }
        // Initialize stop_vector elements to 0
        for (auto &val : stop_vector) {
            val.store(0);
        }
        root = buildCompleteTree(tree_depth);
    }

    ~SparseMerkleTree() {
        delete root;
    }

    string getRootHash() const {
        return root->hash;
    }

    MerkleNode *getLeafNode(const string &key) {
        auto it = leaf_nodes.find(key);
        if (it != leaf_nodes.end()) {
            return it->second;
        }
        return nullptr;
    }

    size_t getLeafCount() const {
        return leaf_nodes.size();
    }

    void printLeafKeys() const {
        cout << "Leaf keys in the map: " << endl;
        for (const auto &pair : leaf_nodes) {
            cout << "  " << pair.first << endl;
        }
    }

    void update(const string &key, const string &value, ThreadUpdateId thread_index) {
        if (key.length() != depth) {
            throw runtime_error("Invalid key length");
        }

        MerkleNode *current = nullptr;
        {
            auto it = leaf_nodes.find(key);
            if (it == leaf_nodes.end()) {
                throw runtime_error("Leaf node not found for key: " + key);
            }
            current = it->second;
        }

        {
            lock_guard<mutex> lock(current->node_mutex);
            if (!current->is_leaf) {
                throw runtime_error("Reached non-leaf node");
            }
            // If node was updated by another thread, mark it to stop
            if (current->last_updated_thread_index.thread_index != thread_index.thread_index &&
                current->last_updated_thread_index.thread_index >= 0) {
                int old_count = stop_vector[current->last_updated_thread_index.thread_index].load();
                int new_count = current->last_updated_thread_index.update_count;
                while (new_count > old_count &&
                       !stop_vector[current->last_updated_thread_index.thread_index].compare_exchange_weak(old_count, new_count)) {
                    old_count = stop_vector[current->last_updated_thread_index.thread_index].load();
                }
            }
            current->hash = computeHash(value);
            current->last_updated_thread_index = thread_index;
        }

        while (current != root) {
            // Check if thread should stop before locking parent
            // if (thread_index.thread_index >= 0 && stop_vector[thread_index.thread_index].load() >= thread_index.update_count) {
            //     return;
            // }

            int isLeft = -1;
            string leftHash = "", rightHash = "";
            MerkleNode *parent = current->parent;
            MerkleNode *left = NULL, *right = NULL;
            ThreadUpdateId left_updated_by, right_updated_by;
            lock_guard<mutex> parent_lock(parent->node_mutex);
            // Check stop condition after acquiring parent lock
            if (thread_index.thread_index >= 0 && stop_vector[thread_index.thread_index].load() >= thread_index.update_count) {
                return;
            }

            if (current == parent->left) {
                isLeft = 1;
                right = parent->right;
                left = current;
                if (parent->left_child_thread_index == thread_index) {
                    return;
                }
            } else {
                isLeft = 0;
                left = parent->left;
                right = current;
                if (parent->right_child_thread_index == thread_index) {
                    return;
                }
            }

            {
                lock_guard<mutex> leftChildLock(left->node_mutex);
                lock_guard<mutex> rightChildLock(right->node_mutex);
                leftHash = left->hash;
                left_updated_by = left->last_updated_thread_index;
                rightHash = right->hash;
                right_updated_by = right->last_updated_thread_index;
            }

            // If parent was updated by another thread, mark it to stop
            if (parent->last_updated_thread_index.thread_index != thread_index.thread_index &&
                parent->last_updated_thread_index.thread_index >= 0) {
                int old_count = stop_vector[parent->last_updated_thread_index.thread_index].load();
                int new_count = parent->last_updated_thread_index.update_count;
                while (new_count > old_count &&
                       !stop_vector[parent->last_updated_thread_index.thread_index].compare_exchange_weak(old_count, new_count)) {
                    old_count = stop_vector[parent->last_updated_thread_index.thread_index].load();
                }
            }

            string parentHash = leftHash + rightHash;
            parent->hash = computeHash(parentHash);
            parent->left_child_thread_index = left_updated_by;
            parent->right_child_thread_index = right_updated_by;
            parent->last_updated_thread_index = thread_index;
            current = parent;
        }
    }

    void updateSerial(const string &key, const string &value) {
        if (key.length() != depth) {
            throw runtime_error("Invalid key length");
        }
        MerkleNode *current = nullptr;
        {
            auto it = leaf_nodes.find(key);
            if (it == leaf_nodes.end()) {
                throw runtime_error("Leaf node not found for key: " + key);
            }
            current = it->second;
        }
        string childHash = "";
        {
            if (!current->is_leaf) {
                throw runtime_error("Reached non-leaf node");
            }
            current->hash = computeHash(value);
            childHash = current->hash;
        }

        while (current != root) {
            int isLeft = -1;
            string siblingHash = "";
            MerkleNode *parent = current->parent;
            MerkleNode *sibling = NULL;
            if (current == parent->left) {
                isLeft = 1;
                sibling = parent->right;
            } else {
                isLeft = 0;
                sibling = parent->left;
            }
            {
                siblingHash = sibling->hash;
            }
            string parentHash = isLeft ? (childHash + siblingHash) : (siblingHash + childHash);
            parent->hash = computeHash(parentHash);
            current = parent;
            childHash = current->hash;
        }
    }

    string readRootHash() {
        lock_guard<mutex> lock(root->node_mutex);
        return root->hash;
    }

    string readLeafHash(const string &key) {
        MerkleNode *leaf = getLeafNode(key);
        if (!leaf)
            throw runtime_error("Leaf not found for key: " + key);
        lock_guard<mutex> lock(leaf->node_mutex);
        return leaf->hash;
    }
};

// Thread pool
class MerkleThreadPool {
private:
    SparseMerkleTree &tree;
    queue<OperationRequest> request_queue;
    mutex queue_mutex;
    condition_variable cv;
    atomic<bool> stop_threads;
    atomic<int> processed_ops;
    int total_ops;
    static thread_local int thread_update_counter;

    void worker_function(int index) {
        while (true) {
            OperationRequest request(UPDATE);
            {
                unique_lock<mutex> lock(queue_mutex);
                cv.wait(lock, [this] { return !request_queue.empty() || stop_threads; });

                if (stop_threads && request_queue.empty())
                    return;

                if (!request_queue.empty()) {
                    request = request_queue.front();
                    request_queue.pop();
                } else {
                    continue;
                }
            }

            if (request.op_type == UPDATE) {
                thread_update_counter++;
                ThreadUpdateId thread_index(index);
                thread_index.update_count = thread_update_counter;
                tree.update(request.key, request.value, thread_index);
            } else if (request.op_type == READ_ROOT) {
                string hash = tree.readRootHash();
                (void)hash;
            } else if (request.op_type == READ_LEAF) {
                string hash = tree.readLeafHash(request.key);
                (void)hash;
            }

            processed_ops++;

            //  Auto shutdown when done
            if (processed_ops >= total_ops) {
                {
                    unique_lock<mutex> lock(queue_mutex);
                    stop_threads = true;
                }
                cv.notify_all(); // Wake up all threads stuck on queue
                return;
            }
        }
    }

public:
    vector<thread> workers;

    MerkleThreadPool(SparseMerkleTree &tree, int num_threads, int total_ops)
        : tree(tree), stop_threads(false), processed_ops(0), total_ops(total_ops) {
        for (int i = 0; i < num_threads; ++i)
            workers.emplace_back(&MerkleThreadPool::worker_function, this, i);
    }

    ~MerkleThreadPool() {
        {
            unique_lock<mutex> lock(queue_mutex);
            stop_threads = true;
        }
        cv.notify_all();
        for (auto &t : workers)
            if (t.joinable())
                t.join();
    }

    void enqueue_operation(const OperationRequest &req) {
        {
            unique_lock<mutex> lock(queue_mutex);
            request_queue.push(req);
        }
        cv.notify_one();
    }

    int get_processed_ops() const { return processed_ops.load(); }
};

thread_local int MerkleThreadPool::thread_update_counter = 0;

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

long long verify_with_serial_execution(const vector<OperationRequest> &operations, int tree_depth, SparseMerkleTree &tree) {
    cout << "\n==== Starting Serial Verification ====\n";

    SparseMerkleTree serial_tree(tree_depth);
    cout << "Initial root hash (serial): " << serial_tree.getRootHash() << endl;
    auto serial_start = chrono::high_resolution_clock::now();

    for (const auto &req : operations) {
        if (req.op_type == UPDATE) {
            serial_tree.updateSerial(req.key, req.value);
        } else if (req.op_type == READ_ROOT) {
            string hash = serial_tree.readRootHash();
            (void)hash; // simulate read
        } else if (req.op_type == READ_LEAF) {
            string hash = serial_tree.readLeafHash(req.key);
            (void)hash; // simulate read
        }
    }

    auto serial_end = chrono::high_resolution_clock::now();
    auto serial_duration = chrono::duration_cast<chrono::milliseconds>(serial_end - serial_start).count();
    cout << "Final root hash (serial): " << serial_tree.getRootHash() << endl;
    cout << "Serial execution time: " << serial_duration << " ms" << endl;
    cout << "Throughput: " << ((double)operations.size() / serial_duration) << " ops/millisec" << endl;
    cout << "==== Serial Verification Complete ====\n";

    if (serial_tree.getRootHash() == tree.getRootHash()) { // Typo fix coming below
        cout << "Hash verification: PASSED - Parallel and serial hashes match" << endl;
    } else {
        cout << "Hash verification: FAILED - Parallel and serial hashes do not match" << endl;
    }

    return serial_duration;
}

int main() {
    srand(time(nullptr));
    int tree_depth, num_threads, total_ops;
    double read_percentage;

    cout << "Enter tree depth, read percentage, number of threads, and total operations: ";
    cin >> tree_depth >> read_percentage >> num_threads >> total_ops;

    if (num_threads > MAX_THREADS) {
        cout << "Number of threads exceeds maximum limit of " << MAX_THREADS << endl;
        return 1;
    }

    if (tree_depth < 0 || read_percentage < 0 || read_percentage > 100 || num_threads <= 0 || total_ops <= 0) {
        cout << "Invalid input values." << endl;
        return 1;
    }

    SparseMerkleTree tree(tree_depth);
    cout << "Initial Tree State (Root Hash): " << tree.getRootHash() << endl;
    cout << "Total leaf nodes: " << tree.getLeafCount() << endl;
    cout << "------------------------" << endl;

    // vector to store all operations
    vector<OperationRequest> all_operations;

    MerkleThreadPool pool(tree, num_threads, total_ops);
    auto start_time = chrono::high_resolution_clock::now();

    all_operations.reserve(total_ops);
    cout << "Generating and enqueueing " << total_ops << " operations (mix of reads and updates)..." << endl;

    vector<string> leaf_keys;
    for (int i = 0; i < (1 << tree_depth); ++i) {
        string key = bitset<32>(i).to_string().substr(32 - tree_depth);
        leaf_keys.push_back(key);
    }

    for (int i = 0; i < total_ops; i++) {
        OperationRequest op = generate_random_operation(tree_depth, read_percentage, leaf_keys);
        pool.enqueue_operation(op);
        all_operations.push_back(op); // Store the operation for serial verification later
        if ((i + 1) % 10000 == 0) {
            cout << "Generated " << (i + 1) << " operations of " << total_ops << endl;
        }
        if (i % 100 == 0) {
            this_thread::sleep_for(chrono::microseconds(1));
        }
    }

    cout << "All operations have been enqueued. Waiting for threads to complete..." << endl;

    // Wait for threads to finish (they will stop themselves)
    for (auto &worker : pool.workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    auto end_time = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time).count();

    cout << "------------------------" << endl;
    cout << "Final Tree State (Root Hash): " << tree.getRootHash() << endl;
    cout << "Parallel execution time: " << duration << " ms" << endl;
    cout << "Total processed operations: " << pool.get_processed_ops() << endl;
    cout << "Throughput: " << ((double)pool.get_processed_ops() / duration) << " ops/millisec" << endl;
    cout << "------------------------" << endl;

    cout << "Verifying with serial execution..." << endl;
    auto serial_time = verify_with_serial_execution(all_operations, tree_depth, tree);

    cout << "------------------------" << endl;
    cout << "Speedup: " << ((double)serial_time / duration) << endl;
    cout << "------------------------" << endl;

    return 0;
}