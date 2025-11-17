#pragma once
#include "merkleTree.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct AngelaNode : public MerkleNode {
    atomic<int> visited;

    AngelaNode(bool leaf = false)
        : MerkleNode(leaf), visited(0) {}
};

class AngelaAlgorithm {
public:
    template <typename TreeType>
    static void workerFunc(
        int tid,
        TreeType *tree,
        vector<pair<string, string>> *updates,
        unordered_set<string> *conflictPrefixes,
        atomic<size_t> *taskIndex,
        size_t total) {
        using Node = typename TreeType::NodeTypeAlias;

        while (true) {
            size_t idx = taskIndex->fetch_add(1);
            if (idx >= total)
                break;

            const string &key = (*updates)[idx].first;
            const string &val = (*updates)[idx].second;

            Node *leaf = tree->getLeafNode(key);
            if (!leaf)
                continue;

            // update leaf
            {
                lock_guard<mutex> lk(leaf->node_mutex);
                leaf->hash = computeHash(val);
            }

            // percolate upwards
            Node *cur = leaf;
            Node *root = tree->getRoot();

            while (cur != root) {
                Node *parent = static_cast<Node *>(cur->parent);
                if (!parent)
                    break;

                bool isConflict = conflictPrefixes->count(parent->key);

                if (isConflict) {
                    unique_lock<mutex> pl(parent->node_mutex);
                    int expected = 0;
                    if (parent->visited.compare_exchange_strong(expected, 1)) {
                        pl.unlock();
                        break;
                    }

                    string L = parent->left ? parent->left->hash : "";
                    string R = parent->right ? parent->right->hash : "";
                    parent->hash = computeHash(L + R);

                    cur = parent;
                    continue;
                }

                unique_lock<mutex> pl(parent->node_mutex);
                string L = parent->left ? parent->left->hash : "";
                string R = parent->right ? parent->right->hash : "";
                parent->hash = computeHash(L + R);

                cur = parent;
            }
        }
    }

    // ============================================================
    //  MAIN PROCESS BATCH
    // ============================================================
    template <typename TreeType>
    long long processBatch(
        TreeType &tree,
        const vector<pair<string, string>> &updates_in,
        int numThreads) {
        using Node = typename TreeType::NodeTypeAlias;

        if (updates_in.empty())
            return 0;

        // -----------------------------
        // SORT BY KEY
        // -----------------------------
        vector<pair<string, string>> updates = updates_in;

        sort(updates.begin(), updates.end(),
             [](auto &a, auto &b) { return a.first < b.first; });

        // -----------------------------
        // COMPUTE CONFLICT PREFIXES
        // -----------------------------
        unordered_set<string> conflictPrefixes;

        auto lcp = [&](const string &a, const string &b) {
            size_t n = min(a.size(), b.size());
            size_t i = 0;
            for (; i < n; ++i)
                if (a[i] != b[i])
                    break;
            return i;
        };

        for (size_t i = 0; i + 1 < updates.size(); ++i) {
            size_t cl = lcp(updates[i].first, updates[i + 1].first);
            conflictPrefixes.insert(updates[i].first.substr(0, cl));
        }

        // reset visited flags
        for (auto &prefix : conflictPrefixes) {
            Node *n = getNodeByPrefix(tree, prefix);
            if (n)
                n->visited.store(false);
        }

        // -----------------------------
        // PARALLEL EXECUTION
        // -----------------------------
        atomic<size_t> taskIndex(0);
        size_t total = updates.size();

        auto startTime = chrono::high_resolution_clock::now();

        vector<thread> workers;
        workers.reserve(numThreads);

        for (int i = 0; i < numThreads; i++) {
            workers.emplace_back(
                workerFunc<TreeType>,
                i,
                &tree,
                &updates,
                &conflictPrefixes,
                &taskIndex,
                total);
        }

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