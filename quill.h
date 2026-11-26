// quill · quill.h — the shared base: version, diagnostics, number text, SHA-256, files, JSON.
// Pure std here; windows.h stays inside the .cpp files.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quill {

constexpr const char* kVersion = "0.1.1";

// The one tolerance of the tool (BRIEF section 5), for arithmetic that rounds: two states may be merged,
// a field is clean, and a threshold argument may be on an edge, only when the numbers agree to this.
// prove uses no tolerance at all when its arithmetic is exact, and narrower ones where it can (prove.cpp).
constexpr double kTol = 1e-9;

std::string ssprintf(const char* f, ...);

// ---- diagnostics -------------------------------------------------------------------------------
// One problem with an input. `code` is stable (E_SPEC, E_ARGS, E_IO, E_FORMAT, E_UNSUPPORTED, E_LIMIT,
// E_EXISTS, E_INTERNAL; W_* for warnings); line and column are 1-based, 0 when they do not apply.
struct Issue {
    std::string code, message;
    int line = 0, column = 0;
    std::string source;   // the offending line of the input, when there is one
    std::string hint;     // what to write instead, when quill can tell
};

// Thrown for anything wrong with the input. Carries every issue found, at least one.
struct Fail {
    std::vector<Issue> issues;
    Fail() = default;
    Fail(std::string code, std::string message, std::string hint = "");
    explicit Fail(Issue i);
    const Issue& first() const { return issues.front(); }
};

// ---- numbers as text ---------------------------------------------------------------------------
std::string num(double v);                 // shortest text that reads back as the same double (ECMAScript form)
std::string show(double v);                // for people: snapped to the 1e-9 grid, then shortest
std::string sci(double v);                 // for small residuals: 0, or two significant digits ("4.4e-16")
std::string commas(unsigned long long v);  // 2000000 -> "2,000,000"
bool to_double(std::string_view s, double& out);        // the whole string, finite, correctly rounded
bool to_u64(std::string_view s, unsigned long long& out);
bool to_i64(std::string_view s, long long& out);

// ---- text --------------------------------------------------------------------------------------
std::string join(const std::vector<std::string>& v, const char* sep);
std::string trim(std::string_view s);
bool valid_utf8(std::string_view s);
std::string clean_utf8(std::string_view s);              // ill-formed bytes become U+FFFD
int edit_distance(std::string_view a, std::string_view b);
// The closest of `have` to `want` when it is close enough to be a likely typo, else "".
std::string nearest(std::string_view want, const std::vector<std::string>& have);

// ---- SHA-256 -----------------------------------------------------------------------------------
std::string sha256_hex(std::string_view bytes);
struct Sha256 {                            // the same hash, fed in pieces
    uint32_t h[8];
    unsigned char buf[64] = {};
    size_t fill = 0;
    unsigned long long total = 0;
    Sha256();
    void update(const void* data, size_t n);
    std::string hex();                     // may be called once the last piece is in
};

// ---- random numbers ----------------------------------------------------------------------------
// xoshiro256** seeded through splitmix64: quill's only source of random numbers, so that a seed
// means the same stream on every machine. It is not numpy's generator.
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed);
    uint64_t next();
    double uniform() { return (double)(next() >> 11) * 0x1.0p-53; }     // in [0, 1)
    uint32_t below(uint32_t n);                                         // uniform in [0, n), no bias
};

// ---- files (paths are UTF-8; either slash) -----------------------------------------------------
bool read_file(const std::string& path, std::string& out, std::string& err);
bool write_file(const std::string& path, std::string_view bytes, std::string& err);   // atomic replace
struct OutFile {                           // a file written in pieces (run --dump)
    void* handle = nullptr;
    ~OutFile();
    bool open(const std::string& path, std::string& err);
    bool write(const void* data, size_t n, std::string& err);
    void close();
};
bool is_dir(const std::string& path);
bool is_file(const std::string& path);
bool make_dirs(const std::string& path, std::string& err);
std::string path_join(const std::string& a, const std::string& b);
std::string exe_path();
std::string exe_dir();
double now_ms();                                         // a monotonic clock, for timings only
std::string narrow(const wchar_t* w);                    // UTF-16 -> UTF-8
std::string slurp(const std::string& path, const char* what);   // read_file, or Fail(E_IO)

// ---- JSON --------------------------------------------------------------------------------------
// A small value type. Objects keep insertion order; the canonical writer sorts keys.
struct JV {
    enum T : uint8_t { Null, Bool, Int, Num, Str, Arr, Obj };
    T t = Null;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::string s;
    std::vector<JV> arr;
    std::vector<std::pair<std::string, JV>> obj;

    JV() = default;
    JV(bool v) : t(Bool), b(v) {}
    JV(int v) : t(Int), i(v), d(v) {}
    JV(unsigned v) : t(Int), i(v), d(v) {}
    JV(long long v) : t(Int), i(v), d((double)v) {}
    JV(unsigned long long v) : t(Int), i((long long)v), d((double)v) {}
    JV(double v) : t(Num), d(v) {}
    JV(const char* v) : t(Str), s(v) {}
    JV(const std::string& v) : t(Str), s(v) {}
    JV(std::string&& v) : t(Str), s(std::move(v)) {}

    static JV object() { JV v; v.t = Obj; return v; }
    static JV array() { JV v; v.t = Arr; return v; }
    static JV strings(const std::vector<std::string>& v) { JV a = array(); for (const auto& x : v) a.push(x); return a; }
    static JV numbers(const std::vector<double>& v) { JV a = array(); for (double x : v) a.push(x); return a; }

    JV& set(const std::string& k, JV v);                 // appends, or replaces an existing key
    JV& push(JV v) { arr.push_back(std::move(v)); return *this; }
    const JV* get(const char* k) const;
    bool is_num() const { return t == Int || t == Num; }
    bool is_str() const { return t == Str; }
    double number(double dflt = 0) const { return is_num() ? d : dflt; }
    const std::string& str() const { return s; }
};

// Strict RFC 8259. On failure `err` says what and where ("line 3, column 7: expected ':'").
bool json_parse(std::string_view text, JV& out, std::string& err);
std::string json_compact(const JV& v);                   // one line, insertion order
std::string json_canonical(const JV& v);                 // RFC 8785: sorted keys, shortest numbers
std::string json_pretty(const JV& v, int indent = 1);    // for files people read

}  // namespace quill
