// quill · net.h — a recurrent network as the kit stores it (seven float64 arrays), the folder
// around it, and the kernel that runs it.
#pragma once
#include "quill.h"

namespace quill {

struct Network {
    std::string name;
    std::vector<std::string> tokens;
    int d = 0;                 // width (the kit's n_embd)
    int n = 0;                 // neurons
    // Row-major, as in weights.npz:  E tokens x d,  W1 n x d,  b1 n,  W2 d x n,  r0 d,  Wout 2 x d,  bout 2
    std::vector<double> E, W1, b1, W2, r0, Wout, bout;
    int T() const { return (int)tokens.size(); }
    size_t weights() const;    // every stored number: the kit's n_weights
    size_t nonzero() const;
};

std::string weights_npz(const Network& net);                                  // the bytes of weights.npz
// Fills the arrays, d and n from the bytes of a weights.npz. Throws Fail(E_FORMAT, E_UNSUPPORTED).
void arrays_from_npz(std::string_view file, const std::string& label, Network& net);

// A network folder read from disk.
struct Folder {
    std::string dir;
    Network net;
    JV network, manifest;      // network.json; manifest.json (Null when absent)
    std::string weights_sha256, network_sha256, manifest_sha256;   // of the three files; "" when absent
};
// Reads and cross-checks weights.npz and network.json; manifest.json is optional. Throws Fail.
Folder load_folder(const std::string& dir);

// One step per token:
//     x = r + E[token];   z = b1 + W1 x;   r' = x + W2 max(z, 0);   margin = (Wout[emit] - Wout[hold]) . r' + bias
// with one rounded multiply and one rounded add per term, nothing fused, in this fixed order:
//     z[i]   starts at b1[i] and takes W1[i][j] * x[j] for j = 0, 1, 2, ...
//     r'[k]  starts at x[k] and takes W2[k][i] * z[i] for the neurons i with z[i] > 0, in increasing i
//     margin is ((s0 + s1) + (s2 + s3)) + bias, where s[j mod 4] takes w[j] * r'[j] for j = 0, 1, 2, ...
// Terms that are exactly zero may be skipped, which changes no value. So the same file gives the
// same bits on every machine, and when no operation rounds, the bits are the real-number answer.
class Kernel {
public:
    explicit Kernel(const Network& net);
    int width() const { return d_; }
    int neurons() const { return n_; }
    int padded() const { return dp_; }                        // length of a state buffer (width rounded up to 8)
    int padded_neurons() const { return np_; }                // length of a z buffer
    void load(const double* r, double* state) const;          // width values in, a padded state out
    void step(double* state, int token, double* z) const;     // in place; z receives the pre-activations
    void step(double* state, int token) const { step(state, token, z_.data()); }
    double margin(const double* state) const;                 // emit minus hold
private:
    int d_ = 0, n_ = 0, dp_ = 0, np_ = 0;
    std::vector<double> E_, W1c_, b1_, W2n_;                  // E by token; W1 by column; W2 by neuron
    std::vector<double> wm_;
    double bm_ = 0;
    mutable std::vector<double> z_;
    mutable std::vector<int> act_;
};

}  // namespace quill
