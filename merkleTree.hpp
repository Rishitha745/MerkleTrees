#pragma once
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <openssl/sha.h>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

// Global stop vector for tracking when threads should stop
static constexpr int MAX_THREADS = 64; // Fixed size for stop_vector
static vector<atomic<int>> stop_vector(MAX_THREADS);

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

// Structure for MerkleTree nodes
struct MerkleNode {
    string hash;
    MerkleNode *left;
    MerkleNode *right;
    MerkleNode *parent;
    bool is_leaf;
    mutex node_mutex;
    string key;

    MerkleNode(bool leaf = false)
        : hash(), left(nullptr), right(nullptr), parent(nullptr), is_leaf(leaf), key("") {}

    ~MerkleNode() {
        delete left;
        delete right;
    }
};

template <typename NodeType>
class SparseMerkleTree {
public:
    using Node = NodeType;
    using NodeTypeAlias = NodeType;

private:
    NodeType *root;
    int depth;
    string default_leaf_hash;
    unordered_map<string, NodeType *> leaf_nodes;

    NodeType *buildCompleteTree(int d, NodeType *parent, string prefix) {
        NodeType *node = new NodeType(d == 0);
        node->key = prefix;
        node->parent = parent;

        if (d == 0) {
            node->hash = default_leaf_hash;
            leaf_nodes[prefix] = node;
            return node;
        }
        node->left = buildCompleteTree(d - 1, node, prefix + "0");
        node->right = buildCompleteTree(d - 1, node, prefix + "1");
        node->hash = computeHash(node->left->hash + node->right->hash);
        return node;
    }

public:
    SparseMerkleTree(int tree_depth)
        : depth(tree_depth), default_leaf_hash(computeHash("")) {
        root = buildCompleteTree(tree_depth, nullptr, "");
    }

    virtual ~SparseMerkleTree() { delete root; }

    int getDepth() const {
        return depth;
    }

    NodeType *getRoot() {
        return root;
    }

    string getRootHash() const {
        return root->hash;
    }

    size_t getLeafCount() const {
        return leaf_nodes.size();
    }

    NodeType *getLeafNode(const string &key) {
        auto it = leaf_nodes.find(key);
        if (it == leaf_nodes.end())
            return nullptr;
        return it->second;
    }

    void printLeafKeys() const {
        cout << "Leaf keys in the map: " << endl;
        for (const auto &pair : leaf_nodes) {
            cout << "  " << pair.first << endl;
        }
    }
};

template <typename TreeType>
void updateSerial(TreeType &tree, const string &key, const string &value) {
    using Node = typename TreeType::NodeTypeAlias;

    int depth = tree.getDepth();
    if ((int)key.length() != depth) {
        throw runtime_error("Invalid key length");
    }

    // Get leaf node
    Node *current = static_cast<Node *>(tree.getLeafNode(key));
    if (!current) {
        throw runtime_error("Leaf node not found for key: " + key);
    }

    // Update leaf hash
    if (!current->is_leaf) {
        throw runtime_error("Reached non-leaf node while updating leaf");
    }
    current->hash = computeHash(value);

    string childHash = current->hash;
    Node *root = static_cast<Node *>(tree.getRoot());

    // Percolate upwards
    while (current != root) {
        Node *parent = static_cast<Node *>(current->parent);
        if (!parent)
            break;

        Node *left = static_cast<Node *>(parent->left);
        Node *right = static_cast<Node *>(parent->right);

        bool isLeft = (current == left);
        string siblingHash = isLeft ? right->hash : left->hash;

        string parentHash = isLeft
                                ? (childHash + siblingHash)
                                : (siblingHash + childHash);

        parent->hash = computeHash(parentHash);

        current = parent;
        childHash = current->hash;
    }
}
