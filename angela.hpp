#pragma once
#include "merkleTree.hpp"

// Structure for MerkleTree nodes
struct AngelaNode : public MerkleNode {
    atomic<bool> visited;

    AngelaNode(bool leaf = false)
        : MerkleNode(leaf), visited(false) {}
};

class AngelaAlgorithm {
public:
    /**
     * Process a batch of updates using Angela's conflict-based parallel algorithm.
     *
     * Requirements for TreeType:
     *  - TreeType::NodeType must inherit from MerkleNode and contain:
     *        atomic<bool> visited;
     *  - TreeType must implement:
     *        getLeafNode(key)
     *        getRoot()
     *        getDepth()
     *  - Node must contain:
     *        key  (binary path string)
     *        left, right, parent pointers
     *        node_mutex
     *        hash
     */
    template <typename TreeType>
    long long processBatch(TreeType &tree, const vector<pair<string, string>> &updates_in, int numThreads) {
        using Node = typename TreeType::NodeTypeAlias;

        if (updates_in.empty())
            return 0;

        int depth = tree.getDepth();

        // -----------------------------------------------------
        // STEP 1 — Sort updates by key
        // -----------------------------------------------------
        vector<pair<string, string>> updates = updates_in;
        sort(updates.begin(), updates.end(),
             [](auto &a, auto &b) { return a.first < b.first; });

        // -----------------------------------------------------
        // STEP 2 — Compute conflict prefixes
        // -----------------------------------------------------
        unordered_set<string> conflictPrefixes;

        auto lcp = [&](const string &a, const string &b) {
            size_t n = min(a.size(), b.size());
            size_t i = 0;
            for (; i < n; ++i)
                if (a[i] != b[i])
                    break;
            return i; // common prefix length
        };

        for (size_t i = 0; i + 1 < updates.size(); ++i) {
            size_t common_len = lcp(updates[i].first, updates[i + 1].first);
            string prefix = updates[i].first.substr(0, common_len);
            conflictPrefixes.insert(prefix);
        }

        // -----------------------------------------------------
        // STEP 3 — Reset the visited flag for all conflict nodes
        // -----------------------------------------------------
        for (const auto &prefix : conflictPrefixes) {
            Node *n = getNodeByPrefix(tree, prefix);
            if (n)
                n->visited.store(false);
        }

        // -----------------------------------------------------
        // STEP 4 — Parallel update execution
        // -----------------------------------------------------
        atomic<size_t> taskIndex(0);
        size_t total = updates.size();

        auto startTime = chrono::high_resolution_clock::now();

        vector<thread> workers;
        workers.reserve(numThreads);

        // -----------------------------------------------------
        // Worker function (each thread)
        // -----------------------------------------------------
        auto worker = [&](int tid) {
            while (true) {
                size_t idx = taskIndex.fetch_add(1);
                if (idx >= total)
                    break;

                const string &key = updates[idx].first;
                const string &val = updates[idx].second;

                Node *leaf = tree.getLeafNode(key);
                if (!leaf)
                    continue;

                // UPDATE LEAF
                {
                    lock_guard<mutex> lk(leaf->node_mutex);
                    leaf->hash = computeHash(val);
                }

                // -----------------------------------------------------
                // PERCOLATE UPWARDS
                // -----------------------------------------------------
                Node *cur = leaf;
                Node *root = tree.getRoot();

                while (cur != root) {
                    Node *parent = static_cast<Node *>(cur->parent);
                    if (!parent)
                        break;

                    bool isConflict = conflictPrefixes.count(parent->key);

                    if (isConflict) {
                        unique_lock<mutex> pl(parent->node_mutex);

                        bool wasVisited = parent->visited.load();
                        if (!wasVisited) {
                            parent->visited.store(true);
                            // STOP THREAD HERE
                            pl.unlock();
                            break;
                        }

                        // otherwise: combine hashes
                        string L = parent->left ? parent->left->hash : "";
                        string R = parent->right ? parent->right->hash : "";
                        parent->hash = computeHash(L + R);

                        cur = parent;
                        continue;
                    }

                    // NON-CONFLICT NODE: Just recompute hash
                    {
                        unique_lock<mutex> pl(parent->node_mutex);
                        string L = parent->left ? parent->left->hash : "";
                        string R = parent->right ? parent->right->hash : "";
                        parent->hash = computeHash(L + R);
                    }

                    cur = parent;
                }
            }
        };

        // Launch threads
        for (int i = 0; i < numThreads; i++)
            workers.emplace_back(worker, i);

        for (auto &t : workers)
            t.join();

        auto endTime = chrono::high_resolution_clock::now();
        return chrono::duration_cast<chrono::milliseconds>(endTime - startTime).count();
    }

private:
    template <typename TreeType>
    typename TreeType::NodeTypeAlias *getNodeByPrefix(TreeType &tree, const string &prefix) {
        using Node = typename TreeType::NodeTypeAlias;
        Node *cur = tree.getRoot();
        for (char c : prefix) {
            if (!cur)
                return nullptr;
            cur = (c == '0')
                      ? static_cast<Node *>(cur->left)
                      : static_cast<Node *>(cur->right);
        }
        return cur;
    }
};