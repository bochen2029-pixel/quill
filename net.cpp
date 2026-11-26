// quill · net.cpp — the kit's file format and the step kernel.
#include "net.h"
#include "npz.h"

#include <emmintrin.h>

namespace quill {

size_t Network::weights() const {
    return E.size() + W1.size() + b1.size() + W2.size() + r0.size() + Wout.size() + bout.size();
}
size_t Network::nonzero() const {
    size_t c = 0;
    for (const auto* a : { &E, &W1, &b1, &W2, &r0, &Wout, &bout })
        for (double v : *a)
            if (v != 0) ++c;
    return c;
}

std::string weights_npz(const Network& net) {
    const size_t T = (size_t)net.T(), d = (size_t)net.d, n = (size_t)net.n;
    std::vector<std::pair<std::string, std::string>> m;
    m.emplace_back("E", npy_from_f64({ T, d }, net.E.data()));
    m.emplace_back("W1", npy_from_f64({ n, d }, net.W1.data()));
    m.emplace_back("b1", npy_from_f64({ n }, net.b1.data()));
    m.emplace_back("W2", npy_from_f64({ d, n }, net.W2.data()));
    m.emplace_back("r0", npy_from_f64({ d }, net.r0.data()));
    m.emplace_back("Wout", npy_from_f64({ 2, d }, net.Wout.data()));
    m.emplace_back("bout", npy_from_f64({ 2 }, net.bout.data()));
    return npz_store(m);
}

void arrays_from_npz(std::string_view file, const std::string& label, Network& net) {
    const std::vector<NpyArray> arrays = npz_read(file, label);
    const auto find = [&](const char* name) -> const NpyArray& {
        for (const auto& a : arrays)
            if (a.name == name) return a;
        throw Fail("E_FORMAT", label + ": the array '" + name + "' is missing (a network needs E, W1, b1, W2, r0, Wout, bout)");
    };
    for (const auto& a : arrays) {
        if (a.name.rfind("head", 0) == 0)
            throw Fail("E_UNSUPPORTED", label + ": it holds attention heads ('" + a.name + "'); quill " + kVersion +
                                            " runs the recurrent class only (one step per token, no attention)");
    }
    const auto take = [&](const char* name, std::vector<size_t> want, std::vector<double>& out) {
        const NpyArray& a = find(name);
        if (a.descr.size() < 2 || a.descr[1] != 'f')
            throw Fail("E_FORMAT", label + ": the array '" + name + "' has element type '" + a.descr + "'; it must be float64 or float32");
        NpyArray shape;
        shape.shape = want;
        if (a.shape != want)
            throw Fail("E_FORMAT", label + ": the array '" + name + "' has shape " + a.shape_text() + " but " + shape.shape_text() +
                                       " is needed (width " + std::to_string(net.d) + ", " + std::to_string(net.n) + " neurons)");
        out = npy_f64(a, label);
        for (double v : out)
            if (!(v == v) || v - v != 0)
                throw Fail("E_FORMAT", label + ": the array '" + std::string(name) + "' holds a NaN or an infinity");
    };
    const NpyArray& e = find("E");
    const NpyArray& w1 = find("W1");
    if (e.shape.size() != 2 || e.shape[1] == 0 || e.shape[0] == 0)
        throw Fail("E_FORMAT", label + ": the array 'E' has shape " + e.shape_text() + "; it must be (tokens, width)");
    if (w1.shape.size() != 2)
        throw Fail("E_FORMAT", label + ": the array 'W1' has shape " + w1.shape_text() + "; it must be (neurons, width)");
    if (e.shape[1] > 65536 || w1.shape[0] > 1000000 || e.shape[0] > 65536)
        throw Fail("E_LIMIT", label + ": the network is larger than quill handles (width 65,536, 1,000,000 neurons, 65,536 tokens)");
    const size_t T = e.shape[0], d = e.shape[1], n = w1.shape[0];
    net.d = (int)d;
    net.n = (int)n;
    take("E", { T, d }, net.E);
    take("W1", { n, d }, net.W1);
    take("b1", { n }, net.b1);
    take("W2", { d, n }, net.W2);
    take("r0", { d }, net.r0);
    take("Wout", { 2, d }, net.Wout);
    take("bout", { 2 }, net.bout);
    net.tokens.clear();
    for (size_t t = 0; t < T; ++t) net.tokens.push_back(std::to_string(t));
}

Folder load_folder(const std::string& dir) {
    Folder f;
    f.dir = dir;
    if (!is_dir(dir)) throw Fail("E_IO", "'" + dir + "' is not a folder");
    const std::string npath = path_join(dir, "network.json"), wpath = path_join(dir, "weights.npz"),
                      mpath = path_join(dir, "manifest.json");
    if (!is_file(npath) || !is_file(wpath))
        throw Fail("E_IO", "the folder '" + dir + "' is not a network folder: it needs weights.npz and network.json",
                   "quill build SPEC -o DIR writes one");
    const std::string ntext = slurp(npath, "network.json");
    f.network_sha256 = sha256_hex(ntext);
    std::string err;
    if (!json_parse(ntext, f.network, err) || f.network.t != JV::Obj)
        throw Fail("E_FORMAT", "network.json: " + (err.empty() ? std::string("it is not a JSON object") : err));
    const JV& nj = f.network;
    const auto integer = [&](const char* key, long long dflt) {
        const JV* v = nj.get(key);
        return v && v->t == JV::Int ? v->i : dflt;
    };
    const JV* family = nj.get("family");
    if ((family && family->is_str() && family->s != "R") || integer("n_heads", 0) != 0 || integer("context", 1) != 1)
        throw Fail("E_UNSUPPORTED", std::string("network.json describes a sequence network (family '") +
                                        (family && family->is_str() ? family->s : "?") + "', " +
                                        std::to_string(integer("n_heads", 0)) + " attention heads, context " +
                                        std::to_string(integer("context", 1)) + "); quill " + kVersion +
                                        " runs the recurrent class only (one step per token, no attention)");
    const std::string wbytes = slurp(wpath, "weights.npz");
    f.weights_sha256 = sha256_hex(wbytes);
    if (const JV* h = nj.get("weights_sha256"); h && h->is_str() && h->s != f.weights_sha256)
        throw Fail("E_FORMAT", "weights.npz has sha256 " + f.weights_sha256 + " but network.json records " + h->s +
                                   ": the two files do not belong together");
    arrays_from_npz(wbytes, "weights.npz", f.net);
    const JV* name = nj.get("name");
    f.net.name = name && name->is_str() && !name->s.empty() ? name->s : std::string("network");
    if (const JV* toks = nj.get("tokens")) {
        if (toks->t != JV::Arr || (int)toks->arr.size() != f.net.T())
            throw Fail("E_FORMAT", ssprintf("network.json lists %zu tokens but E in weights.npz has %d rows",
                                            toks->t == JV::Arr ? toks->arr.size() : (size_t)0, f.net.T()));
        for (size_t t = 0; t < toks->arr.size(); ++t) {
            if (!toks->arr[t].is_str() || toks->arr[t].s.empty())
                throw Fail("E_FORMAT", "network.json: every entry of tokens must be a non-empty string");
            f.net.tokens[t] = toks->arr[t].s;
        }
    }
    const auto agree = [&](const char* key, long long have, const char* what) {
        const long long want = integer(key, have);
        if (want != have)
            throw Fail("E_FORMAT", ssprintf("network.json says %s = %lld but weights.npz has %s %lld", key, want, what, have));
    };
    agree("n_embd", f.net.d, "width");
    agree("n_input_vocab", f.net.T(), "token count");
    agree("n_neurons", f.net.n, "neuron count");
    if (is_file(mpath)) {
        const std::string mtext = slurp(mpath, "manifest.json");
        f.manifest_sha256 = sha256_hex(mtext);
        if (!json_parse(mtext, f.manifest, err) || f.manifest.t != JV::Obj)
            throw Fail("E_FORMAT", "manifest.json: " + (err.empty() ? std::string("it is not a JSON object") : err));
    }
    return f;
}

// ======================================================================
// the kernel
// ======================================================================
// The sums are kept in registers, eight or sixteen values at a time, so that a column costs one
// multiply and one add per value and nothing waits on memory. SSE2 only: every x64 processor has it,
// and without a fused multiply-add the bits are the same everywhere.
namespace {
inline __m128d ld(const double* p) { return _mm_loadu_pd(p); }
inline void st(double* p, __m128d v) { _mm_storeu_pd(p, v); }
inline __m128d mla(__m128d acc, __m128d a, const double* b) { return _mm_add_pd(acc, _mm_mul_pd(a, _mm_loadu_pd(b))); }
}  // namespace

Kernel::Kernel(const Network& net) : d_(net.d), n_(net.n) {
    dp_ = (d_ + 7) & ~7;
    np_ = (n_ + 7) & ~7;
    const size_t T = (size_t)net.T(), d = (size_t)d_, n = (size_t)n_, dp = (size_t)dp_, np = (size_t)np_;
    E_.assign(T * dp, 0.0);
    for (size_t t = 0; t < T; ++t)
        for (size_t j = 0; j < d; ++j) E_[t * dp + j] = net.E[t * d + j];
    W1c_.assign(d * np, 0.0);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < d; ++j) W1c_[j * np + i] = net.W1[i * d + j];
    b1_.assign(np, 0.0);
    for (size_t i = 0; i < n; ++i) b1_[i] = net.b1[i];
    W2n_.assign(n * dp, 0.0);
    for (size_t j = 0; j < d; ++j)
        for (size_t i = 0; i < n; ++i) W2n_[i * dp + j] = net.W2[j * n + i];
    wm_.assign(dp, 0.0);
    for (size_t j = 0; j < d; ++j) wm_[j] = net.Wout[d + j] - net.Wout[j];
    bm_ = net.bout[1] - net.bout[0];
    z_.assign(np + 2, 0.0);
    act_.assign(n + 1, 0);
}

void Kernel::load(const double* r, double* state) const {
    for (int j = 0; j < d_; ++j) state[j] = r[j];
    for (int j = d_; j < dp_; ++j) state[j] = 0.0;
}

void Kernel::step(double* r, int token, double* z) const {
    // x = r + E[token]
    const double* e = E_.data() + (size_t)token * (size_t)dp_;
    for (int j = 0; j < dp_; j += 2) st(r + j, _mm_add_pd(ld(r + j), ld(e + j)));
    // z = b1 + W1 x: for each neuron, the columns in increasing order (a zero x adds nothing)
    const double* b1 = b1_.data();
    int b = 0;
    for (; b + 16 <= np_; b += 16) {
        __m128d a0 = ld(b1 + b), a1 = ld(b1 + b + 2), a2 = ld(b1 + b + 4), a3 = ld(b1 + b + 6);
        __m128d a4 = ld(b1 + b + 8), a5 = ld(b1 + b + 10), a6 = ld(b1 + b + 12), a7 = ld(b1 + b + 14);
        const double* w = W1c_.data() + b;
        for (int j = 0; j < d_; ++j, w += np_) {
            const double x = r[j];
            if (x == 0) continue;
            const __m128d vx = _mm_set1_pd(x);
            a0 = mla(a0, vx, w);
            a1 = mla(a1, vx, w + 2);
            a2 = mla(a2, vx, w + 4);
            a3 = mla(a3, vx, w + 6);
            a4 = mla(a4, vx, w + 8);
            a5 = mla(a5, vx, w + 10);
            a6 = mla(a6, vx, w + 12);
            a7 = mla(a7, vx, w + 14);
        }
        st(z + b, a0); st(z + b + 2, a1); st(z + b + 4, a2); st(z + b + 6, a3);
        st(z + b + 8, a4); st(z + b + 10, a5); st(z + b + 12, a6); st(z + b + 14, a7);
    }
    if (b < np_) {                                            // np_ is a multiple of 8: one block of 8 is left
        __m128d a0 = ld(b1 + b), a1 = ld(b1 + b + 2), a2 = ld(b1 + b + 4), a3 = ld(b1 + b + 6);
        const double* w = W1c_.data() + b;
        for (int j = 0; j < d_; ++j, w += np_) {
            const double x = r[j];
            if (x == 0) continue;
            const __m128d vx = _mm_set1_pd(x);
            a0 = mla(a0, vx, w);
            a1 = mla(a1, vx, w + 2);
            a2 = mla(a2, vx, w + 4);
            a3 = mla(a3, vx, w + 6);
        }
        st(z + b, a0); st(z + b + 2, a1); st(z + b + 4, a2); st(z + b + 6, a3);
    }
    // r' = x + W2 max(z, 0): for each coordinate, the active neurons in increasing order
    int* act = act_.data();
    int na = 0;
    for (int i = 0; i < n_; ++i) {
        act[na] = i;
        na += z[i] > 0;
    }
    if (!na) return;
    const double* w2 = W2n_.data();
    for (int c = 0; c < dp_; c += 8) {
        __m128d a0 = ld(r + c), a1 = ld(r + c + 2), a2 = ld(r + c + 4), a3 = ld(r + c + 6);
        for (int k = 0; k < na; ++k) {
            const int i = act[k];
            const __m128d vh = _mm_set1_pd(z[i]);
            const double* w = w2 + (size_t)i * (size_t)dp_ + c;
            a0 = mla(a0, vh, w);
            a1 = mla(a1, vh, w + 2);
            a2 = mla(a2, vh, w + 4);
            a3 = mla(a3, vh, w + 6);
        }
        st(r + c, a0); st(r + c + 2, a1); st(r + c + 4, a2); st(r + c + 6, a3);
    }
}

// Four interleaved partial sums s0..s3 (coordinate j goes to s[j mod 4], in increasing j), then
// ((s0 + s1) + (s2 + s3)) + bias.
double Kernel::margin(const double* r) const {
    const double* w = wm_.data();
    __m128d a01 = _mm_setzero_pd(), a23 = _mm_setzero_pd();
    for (int j = 0; j < dp_; j += 4) {
        a01 = _mm_add_pd(a01, _mm_mul_pd(ld(w + j), ld(r + j)));
        a23 = _mm_add_pd(a23, _mm_mul_pd(ld(w + j + 2), ld(r + j + 2)));
    }
    double s[4];
    st(s, a01);
    st(s + 2, a23);
    return ((s[0] + s[1]) + (s[2] + s[3])) + bm_;
}

}  // namespace quill
