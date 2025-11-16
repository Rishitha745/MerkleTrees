#pragma once
#include "merkleTree.hpp"

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
struct LiveUpdatesNode : public MerkleNode {
    ThreadUpdateId last_updated_thread_index;
    ThreadUpdateId left_child_thread_index;
    ThreadUpdateId right_child_thread_index;

    LiveUpdatesNode(bool leaf = false)
        : MerkleNode(leaf), last_updated_thread_index(), left_child_thread_index(), right_child_thread_index() {}
};

class LiveAlgorithm {
public:
    /**
     * Update a single leaf and percolate the hash up using the "live" parallel logic.
     *
     * Requirements:
     *  - TreeType::NodeType inherits from MerkleNode
     *  - NodeType contains:
     *        last_updated_thread_index
     *        left_child_thread_index
     *        right_child_thread_index
     *        hash
     *        parent, left, right pointers
     *        node_mutex
     *  - TreeType must implement getDepth(), getLeafNode(key), getRoot()
     */
    template <typename TreeType>
    void update(TreeType &tree, const string &key, const string &value, ThreadUpdateId incoming_req) {
        using Node = typename TreeType::NodeTypeAlias;

        // Validate key length against tree depth
        int depth = tree.getDepth();
        if ((int)key.length() != depth) {
            throw runtime_error("Invalid key length");
        }

        // Locate leaf node via tree API
        Node *current = static_cast<Node *>(tree.getLeafNode(key));
        if (!current) {
            throw runtime_error("Leaf node not found for key: " + key);
        }

        // Update the leaf under its lock
        {
            lock_guard<mutex> lock(current->node_mutex);
            if (!current->is_leaf) {
                throw runtime_error("Reached non-leaf node while updating leaf");
            }

            // If the leaf already has already been updated by an newer update request, incoming request is stale -> stop
            if (incoming_req.update_count <= current->last_updated_thread_index.update_count) {
                return; // stale — stop immediately, do not overwrite leaf
            }

            // If node was updated by another thread, mark it to stop
            if (current->last_updated_thread_index.thread_index != incoming_req.thread_index &&
                current->last_updated_thread_index.thread_index >= 0) {
                int old_modifier = current->last_updated_thread_index.thread_index;
                int old_count = stop_vector[old_modifier].load();
                int new_count = current->last_updated_thread_index.update_count;
                while (new_count > old_count &&
                       !stop_vector[old_modifier].compare_exchange_weak(old_count, new_count)) {
                    old_count = stop_vector[old_modifier].load();
                }
            }

            current->hash = computeHash(value);
            current->last_updated_thread_index = incoming_req;
        }

        // Percolate upwards
        Node *root = static_cast<Node *>(tree.getRoot());
        while (current != root) {

            string leftHash = "", rightHash = "";
            ThreadUpdateId left_updated_by, right_updated_by;
            Node *parent = static_cast<Node *>(current->parent);
            if (!parent)
                break;

            // Acquire parent lock first
            lock_guard<mutex> parent_lock(parent->node_mutex);

            // Check stop condition for this thread
            if (incoming_req.thread_index >= 0 &&
                stop_vector[incoming_req.thread_index].load() >= incoming_req.update_count) {
                return;
            }

            // Identify sibling and left/right children
            Node *left = static_cast<Node *>(parent->left);
            Node *right = static_cast<Node *>(parent->right);

            // If parent already has this thread as child updater, early exit
            if (current == parent->left) {
                if (parent->left_child_thread_index == incoming_req) {
                    return;
                }
            } else {
                if (parent->right_child_thread_index == incoming_req) {
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
            if (parent->last_updated_thread_index.thread_index != incoming_req.thread_index &&
                parent->last_updated_thread_index.thread_index >= 0) {
                int old_modifier = parent->last_updated_thread_index.thread_index;
                int old_count = stop_vector[old_modifier].load();
                int new_count = parent->last_updated_thread_index.update_count;
                while (new_count > old_count &&
                       !stop_vector[old_modifier].compare_exchange_weak(old_count, new_count)) {
                    old_count = stop_vector[old_modifier].load();
                }
            }

            // Recompute parent hash and update metadata
            parent->hash = computeHash(leftHash + rightHash);
            parent->left_child_thread_index = left_updated_by;
            parent->right_child_thread_index = right_updated_by;
            parent->last_updated_thread_index = incoming_req;

            // Move up
            current = parent;
        }
    }
};
