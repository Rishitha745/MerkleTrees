#include "merkleTree.hpp"

using namespace std;
using namespace std::chrono;

// Get current time in µs
long long now_us() {
    return duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

// CSV Dumper
void dump_csv(const string &filename, const vector<long long> &data) {
    ofstream f(filename);
    for (auto &x : data)
        f << x << "\n";
    f.close();
}

// Compute percentiles (P50, P90, P99)
long long percentile(vector<long long> v, double p) {
    if (v.empty())
        return 0;
    sort(v.begin(), v.end());
    size_t idx = (size_t)(p * v.size());
    if (idx >= v.size())
        idx = v.size() - 1;
    return v[idx];
}