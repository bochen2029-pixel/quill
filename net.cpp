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
// Two code paths compute the same sums in the same order (net.h), and --selftest holds both to the
// plain definition value for value.
//   dense   for a rotated basis: the sums are kept in registers, eight or sixteen values at a time, so a
//           column costs one multiply and one add per value and nothing waits on memory. SSE2 only: every
//           x64 processor has it, and without a fused multiply-add the bits are the same everywhere.
//   sparse  when under a quarter of W1 and W2 is set (an axis-aligned network): only the nonzero weights of
//           the columns whose coordinate is set are visited, and z goes back to b1 after the step.
namespace {
inline __m128d ld(const double* p) { return _mm_loadu_pd(p); }
inline void st(double* p, __m128d v) { _mm_storeu_pd(p, v); }
inline __m128d mla(__m128d acc, __m128d a, const double* b) { return _mm_add_pd(acc, _mm_mul_pd(a, _mm_loadu_pd(b))); }
}  // namespace

Kernel::Kernel(const Network& net) : d_(net.d), n_(net.n) {
    dp_ = (d_ + 7) & ~7;
    np_ = (n_ + 7) & ~7;
    const size_t T = (size_t)net.T(), d = (size_t)d_, n = (size_t)n_, dp = (size_t)dp_, np = (size_t)np_;
    size_t set = 0;
    for (double v : net.W1) set += v != 0;
    for (double v : net.W2) set += v != 0;
    sparse_ = n > 0 && set * 4 < 2 * n * d;                   // under a quarter of W1 and W2 is set: walk the nonzeros only
    b1_.assign(np, 0.0);
    for (size_t i = 0; i < n; ++i) b1_[i] = net.b1[i];
    wm_.assign(dp, 0.0);
    for (size_t j = 0; j < d; ++j) {
        wm_[j] = net.Wout[d + j] - net.Wout[j];
        if (wm_[j] != 0) wm_at_.push_back((int)j);
    }
    bm_ = net.bout[1] - net.bout[0];
    z_.assign(np + 2, 0.0);
    act_.assign(n + 1, 0);
    cols_.assign(d + 1, 0);
    if (!sparse_) {
        size_t start_set = 0;
        for (double v : net.r0) start_set += v != 0;
        full_ = start_set * 2 > d;                            // a rotated basis: every coordinate of every state is set
        E_.assign(T * dp, 0.0);
        for (size_t t = 0; t < T; ++t)
            for (size_t j = 0; j < d; ++j) E_[t * dp + j] = net.E[t * d + j];
        W1c_.assign(d * np, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < d; ++j) W1c_[j * np + i] = net.W1[i * d + j];
        W2n_.assign(n * dp, 0.0);
        for (size_t j = 0; j < d; ++j)
            for (size_t i = 0; i < n; ++i) W2n_[i * dp + j] = net.W2[j * n + i];
        return;
    }
    e_start_.assign(T + 1, 0);
    for (size_t t = 0; t < T; ++t) {
        for (size_t j = 0; j < d; ++j)
            if (net.E[t * d + j] != 0) {
                e_at_.push_back((int)j);
                e_val_.push_back(net.E[t * d + j]);
            }
        e_start_[t + 1] = (int)e_at_.size();
    }
    c_start_.assign(d + 1, 0);                                // W1 by column, neurons in increasing order
    for (size_t j = 0; j < d; ++j) {
        for (size_t i = 0; i < n; ++i)
            if (net.W1[i * d + j] != 0) {
                c_row_.push_back((int)i);
                c_val_.push_back(net.W1[i * d + j]);
            }
        c_start_[j + 1] = (int)c_row_.size();
    }
    n_start_.assign(n + 1, 0);                                // W2 by neuron, coordinates in increasing order
    for (size_t i = 0; i < n; ++i) {
        for (size_t k = 0; k < d; ++k)
            if (net.W2[k * n + i] != 0) {
                n_row_.push_back((int)k);
                n_val_.push_back(net.W2[k * n + i]);
            }
        n_start_[i + 1] = (int)n_row_.size();
        if (net.b1[i] > 0) always_.push_back((int)i);         // active even when no column reaches it
    }
    zs_.assign(net.b1.begin(), net.b1.end());
    mark_.assign(n, 0);
    act2_.assign(n + 1, 0);
}

void Kernel::load(const double* r, double* state) const {
    for (int j = 0; j < d_; ++j) state[j] = r[j];
    for (int j = d_; j < dp_; ++j) state[j] = 0.0;
}

void Kernel::step(double* r, int token, double* z) const {
    if (sparse_) step_sparse(r, token, z);
    else step_dense(r, token, z);
}
void Kernel::step(double* r, int token) const {
    if (sparse_) step_sparse(r, token, nullptr);
    else step_dense(r, token, z_.data());
}

// The same sums over the nonzero weights only. Each z[i] still takes its columns in increasing order
// and each r'[k] its active neurons in increasing order, so every value equals the dense one.
void Kernel::step_sparse(double* r, int token, double* z_out) const {
    for (int p = e_start_[(size_t)token]; p < e_start_[(size_t)token + 1]; ++p) r[e_at_[(size_t)p]] += e_val_[(size_t)p];
    int* cols = cols_.data();
    int nc = 0;
    for (int j = 0; j < d_; ++j) {
        cols[nc] = j;
        nc += r[j] != 0;
    }
    double* zs = zs_.data();                                  // holds b1 between steps
    char* mark = mark_.data();
    int* touched = act_.data();
    int nt = 0;
    for (int i : always_) {
        mark[i] = 1;
        touched[nt++] = i;
    }
    const int* cs = c_start_.data();
    for (int c = 0; c < nc; ++c) {
        const int j = cols[c];
        const double x = r[j];
        for (int p = cs[j]; p < cs[j + 1]; ++p) {
            const int i = c_row_[(size_t)p];
            if (!mark[i]) {
                mark[i] = 1;
                touched[nt++] = i;
            }
            zs[i] += c_val_[(size_t)p] * x;
        }
    }
    if (z_out) memcpy(z_out, zs, (size_t)n_ * sizeof(double));
    int* active = act2_.data();                               // the neurons with z > 0, to be taken in increasing order
    int na = 0;
    for (int k = 0; k < nt; ++k) {
        active[na] = touched[k];
        na += zs[touched[k]] > 0;
    }
    if (na <= 24) {
        for (int a = 1; a < na; ++a) {
            const int v = active[a];
            int b = a - 1;
            for (; b >= 0 && active[b] > v; --b) active[b + 1] = active[b];
            active[b + 1] = v;
        }
    } else {
        std::sort(active, active + na);
    }
    const int* ns = n_start_.data();
    for (int k = 0; k < na; ++k) {
        const int i = active[k];
        const double h = zs[i];
        for (int p = ns[i]; p < ns[i + 1]; ++p) r[n_row_[(size_t)p]] += n_val_[(size_t)p] * h;
    }
    for (int k = 0; k < nt; ++k) {                            // back to b1 for the next step
        const int i = touched[k];
        zs[i] = b1_[(size_t)i];
        mark[i] = 0;
    }
}

void Kernel::step_dense(double* r, int token, double* z) const {
    // x = r + E[token]
    const double* e = E_.data() + (size_t)token * (size_t)dp_;
    for (int j = 0; j < dp_; j += 2) st(r + j, _mm_add_pd(ld(r + j), ld(e + j)));
    // z = b1 + W1 x: for each neuron, the columns in increasing order (a zero x adds nothing)
    int* cols = cols_.data();
    int nc = d_;
    if (!full_) {                                             // few coordinates are set: find them first
        nc = 0;
        for (int j = 0; j < d_; ++j) {
            cols[nc] = j;
            nc += r[j] != 0;
        }
    }
    const double* b1 = b1_.data();
    int b = 0;
    for (; b + 16 <= np_; b += 16) {
        __m128d a0 = ld(b1 + b), a1 = ld(b1 + b + 2), a2 = ld(b1 + b + 4), a3 = ld(b1 + b + 6);
        __m128d a4 = ld(b1 + b + 8), a5 = ld(b1 + b + 10), a6 = ld(b1 + b + 12), a7 = ld(b1 + b + 14);
#define QUILL_COLUMN16(W, X)                                    \
    {                                                           \
        const __m128d vx = _mm_set1_pd(X);                      \
        a0 = mla(a0, vx, (W));                                  \
        a1 = mla(a1, vx, (W) + 2);                              \
        a2 = mla(a2, vx, (W) + 4);                              \
        a3 = mla(a3, vx, (W) + 6);                              \
        a4 = mla(a4, vx, (W) + 8);                              \
        a5 = mla(a5, vx, (W) + 10);                             \
        a6 = mla(a6, vx, (W) + 12);                             \
        a7 = mla(a7, vx, (W) + 14);                             \
    }
        if (nc == d_) {                                       // every coordinate is set (a rotated basis): walk straight through
            const double* w = W1c_.data() + b;
            for (int j = 0; j < d_; ++j, w += np_) QUILL_COLUMN16(w, r[j])
        } else {
            for (int c = 0; c < nc; ++c) QUILL_COLUMN16(W1c_.data() + (size_t)cols[c] * (size_t)np_ + b, r[cols[c]])
        }
#undef QUILL_COLUMN16
        st(z + b, a0); st(z + b + 2, a1); st(z + b + 4, a2); st(z + b + 6, a3);
        st(z + b + 8, a4); st(z + b + 10, a5); st(z + b + 12, a6); st(z + b + 14, a7);
    }
    if (b < np_) {                                            // np_ is a multiple of 8: one block of 8 is left
        __m128d a0 = ld(b1 + b), a1 = ld(b1 + b + 2), a2 = ld(b1 + b + 4), a3 = ld(b1 + b + 6);
#define QUILL_COLUMN8(W, X)                                     \
    {                                                           \
        const __m128d vx = _mm_set1_pd(X);                      \
        a0 = mla(a0, vx, (W));                                  \
        a1 = mla(a1, vx, (W) + 2);                              \
        a2 = mla(a2, vx, (W) + 4);                              \
        a3 = mla(a3, vx, (W) + 6);                              \
    }
        if (nc == d_) {
            const double* w = W1c_.data() + b;
            for (int j = 0; j < d_; ++j, w += np_) QUILL_COLUMN8(w, r[j])
        } else {
            for (int c = 0; c < nc; ++c) QUILL_COLUMN8(W1c_.data() + (size_t)cols[c] * (size_t)np_ + b, r[cols[c]])
        }
#undef QUILL_COLUMN8
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
    if (wm_at_.size() * 4 <= (size_t)dp_) {                   // few readout weights: take them one by one, same sums
        double s[4] = { 0, 0, 0, 0 };
        for (int j : wm_at_) s[j & 3] += wm_[(size_t)j] * r[j];
        return ((s[0] + s[1]) + (s[2] + s[3])) + bm_;
    }
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
