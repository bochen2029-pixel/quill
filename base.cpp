// quill · base.cpp — the shared base behind quill.h: formatting, SHA-256, files, JSON.
#include "quill.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <charconv>
#include <cmath>

namespace quill {

std::string ssprintf(const char* f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int n = vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string out;
    if (n > 0) {
        out.resize((size_t)n);
        vsnprintf(out.data(), (size_t)n + 1, f, ap2);
    }
    va_end(ap2);
    return out;
}

Fail::Fail(std::string code, std::string message, std::string hint) {
    Issue i;
    i.code = std::move(code);
    i.message = std::move(message);
    i.hint = std::move(hint);
    issues.push_back(std::move(i));
}
Fail::Fail(Issue i) { issues.push_back(std::move(i)); }

// ======================================================================
// numbers as text
// ======================================================================
// ECMAScript Number::toString on the shortest round-trip digits (the form RFC 8785 requires):
// 0.5, 1.5, 1e-9, 2.0000000000000004, 200000, 1e+21.
std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    if (v == 0) return "0";
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    const std::string_view sv(buf, (size_t)(r.ptr - buf));   // [-]d[.ddd]e[+-]dd
    std::string out;
    size_t p = 0;
    if (sv[0] == '-') { out += '-'; p = 1; }
    const size_t e = sv.find('e');
    std::string digits;
    for (size_t k = p; k < e; ++k)
        if (sv[k] != '.') digits += sv[k];
    int ex = 0;
    const char* exp_at = sv.data() + e + 1;
    if (*exp_at == '+') ++exp_at;
    std::from_chars(exp_at, sv.data() + sv.size(), ex);
    const int k = (int)digits.size();
    const int n = ex + 1;                                     // the decimal point sits after n digits
    if (k <= n && n <= 21) {
        out += digits;
        out.append((size_t)(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, (size_t)n);
        out += '.';
        out.append(digits, (size_t)n, std::string::npos);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append((size_t)(-n), '0');
        out += digits;
    } else {
        out += digits[0];
        if (k > 1) {
            out += '.';
            out.append(digits, 1, std::string::npos);
        }
        out += 'e';
        out += (n - 1 >= 0 ? '+' : '-');
        out += std::to_string(std::abs(n - 1));
    }
    return out;
}

std::string show(double v) {
    if (!std::isfinite(v)) return "nan";
    if (std::fabs(v) < 1e6) {
        double s = std::round(v * 1e9) / 1e9;
        if (s == 0) s = 0;                                    // no "-0"
        return num(s);
    }
    return num(v);
}

std::string sci(double v) {
    if (v == 0) return "0";
    if (!std::isfinite(v)) return "nan";
    return ssprintf("%.1e", v);
}

std::string commas(unsigned long long v) {
    std::string d = std::to_string(v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

bool to_double(std::string_view s, double& out) {
    if (s.empty()) return false;
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}
bool to_u64(std::string_view s, unsigned long long& out) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    unsigned long long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}
bool to_i64(std::string_view s, long long& out) {
    if (s.empty()) return false;
    long long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

// ======================================================================
// text
// ======================================================================
std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return std::string(s.substr(a, b - a));
}

// Length of the well-formed UTF-8 sequence at s[i], or 0 when there is none.
static size_t utf8_len(std::string_view s, size_t i) {
    const auto at = [&](size_t k) { return (unsigned char)s[k]; };
    const unsigned char c = at(i);
    if (c < 0x80) return 1;
    const auto cont = [&](size_t k) { return k < s.size() && (at(k) & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(i + 1) || !cont(i + 2)) return 0;
        if (c == 0xE0 && at(i + 1) < 0xA0) return 0;         // overlong
        if (c == 0xED && at(i + 1) > 0x9F) return 0;         // surrogates
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
        if (c == 0xF0 && at(i + 1) < 0x90) return 0;         // overlong
        if (c == 0xF4 && at(i + 1) > 0x8F) return 0;         // beyond U+10FFFF
        return 4;
    }
    return 0;
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_len(s, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

std::string clean_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_len(s, i);
        if (n) {
            out.append(s, i, n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

int edit_distance(std::string_view a, std::string_view b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string nearest(std::string_view want, const std::vector<std::string>& have) {
    std::string best;
    int best_d = 1 << 30;
    for (const auto& h : have) {
        const int d = edit_distance(want, h);
        if (d < best_d) { best_d = d; best = h; }
    }
    const int limit = want.size() <= 3 ? 1 : 2;
    return best_d <= limit ? best : std::string();
}

// ======================================================================
// SHA-256 (FIPS 180-4)
// ======================================================================
namespace {
const uint32_t kSha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha_block(uint32_t h[8], const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = k + s1 + ch + kSha[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + mj;
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
}  // namespace

Sha256::Sha256() {
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(h, init, sizeof h);
}

void Sha256::update(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    total += n;
    if (fill) {
        const size_t take = std::min(n, (size_t)64 - fill);
        memcpy(buf + fill, p, take);
        fill += take;
        p += take;
        n -= take;
        if (fill < 64) return;
        sha_block(h, buf);
        fill = 0;
    }
    for (; n >= 64; n -= 64, p += 64) sha_block(h, p);
    memcpy(buf, p, n);
    fill = n;
}

std::string Sha256::hex() {
    unsigned char tail[128] = {};
    memcpy(tail, buf, fill);
    tail[fill] = 0x80;
    const size_t len = fill + 1 + 8 <= 64 ? 64 : 128;
    const unsigned long long bits = total * 8ull;
    for (int i = 0; i < 8; ++i) tail[len - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
    uint32_t out_h[8];
    memcpy(out_h, h, sizeof h);
    sha_block(out_h, tail);
    if (len == 128) sha_block(out_h, tail + 64);
    std::string out;
    for (uint32_t x : out_h) out += ssprintf("%08x", x);
    return out;
}

std::string sha256_hex(std::string_view bytes) {
    Sha256 s;
    s.update(bytes.data(), bytes.size());
    return s.hex();
}

// ======================================================================
// random numbers
// ======================================================================
Rng::Rng(uint64_t seed) {
    uint64_t x = seed;
    for (auto& v : s) {                                       // splitmix64
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        v = z ^ (z >> 31);
    }
}
uint64_t Rng::next() {
    const auto rotl = [](uint64_t v, int k) { return (v << k) | (v >> (64 - k)); };
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}
uint32_t Rng::below(uint32_t n) {
    if (n <= 1) return 0;
    const uint32_t limit = (uint32_t)(0x100000000ull - (0x100000000ull % n));   // reject the uneven tail
    for (;;) {
        const uint32_t v = (uint32_t)(next() >> 32);
        if (limit == 0 || v < limit) return v % n;
    }
}

// ======================================================================
// files
// ======================================================================
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string win_error(DWORD code) {
    wchar_t buf[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 511, nullptr);
    std::string s = trim(narrow(buf));
    return s.empty() ? ssprintf("error %lu", (unsigned long)code) : s;
}

bool read_file(const std::string& path, std::string& out, std::string& err) {
    const HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = win_error(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        err = win_error(GetLastError());
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    size_t got = 0;
    while (got < out.size()) {
        const DWORD want = (DWORD)std::min<size_t>(out.size() - got, (size_t)1 << 28);
        DWORD n = 0;
        if (!ReadFile(h, out.data() + got, want, &n, nullptr) || n == 0) {
            err = n == 0 ? std::string("the file ended early") : win_error(GetLastError());
            CloseHandle(h);
            return false;
        }
        got += n;
    }
    CloseHandle(h);
    return true;
}

bool write_file(const std::string& path, std::string_view bytes, std::string& err) {
    const std::wstring target = widen(path);
    const std::wstring temp = target + L".tmp" + std::to_wstring(GetCurrentProcessId());
    const HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = win_error(GetLastError());
        return false;
    }
    size_t put = 0;
    while (put < bytes.size()) {
        const DWORD want = (DWORD)std::min<size_t>(bytes.size() - put, (size_t)1 << 28);
        DWORD n = 0;
        if (!WriteFile(h, bytes.data() + put, want, &n, nullptr)) {
            err = win_error(GetLastError());
            CloseHandle(h);
            DeleteFileW(temp.c_str());
            return false;
        }
        put += n;
    }
    CloseHandle(h);
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = win_error(GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

OutFile::~OutFile() { close(); }
bool OutFile::open(const std::string& path, std::string& err) {
    close();
    const HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = win_error(GetLastError());
        return false;
    }
    handle = h;
    return true;
}
bool OutFile::write(const void* data, size_t n, std::string& err) {
    const char* p = (const char*)data;
    while (n) {
        const DWORD want = (DWORD)std::min<size_t>(n, (size_t)1 << 28);
        DWORD put = 0;
        if (!handle || !WriteFile((HANDLE)handle, p, want, &put, nullptr)) {
            err = handle ? win_error(GetLastError()) : std::string("the file is not open");
            return false;
        }
        p += put;
        n -= put;
    }
    return true;
}
void OutFile::close() {
    if (handle) CloseHandle((HANDLE)handle);
    handle = nullptr;
}

bool is_dir(const std::string& path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_file(const std::string& path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& path, std::string& err) {
    if (path.empty() || is_dir(path)) return true;
    std::string p = path;
    for (auto& c : p)
        if (c == '\\') c = '/';
    for (size_t i = 1; i <= p.size(); ++i) {
        if (i != p.size() && p[i] != '/') continue;
        const std::string part = p.substr(0, i);
        if (part.empty() || part.back() == ':' || part.back() == '/' || is_dir(part)) continue;
        if (!CreateDirectoryW(widen(part).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = win_error(GetLastError());
            return false;
        }
    }
    return is_dir(path);
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

std::string exe_path() {
    wchar_t buf[1024] = L"";
    GetModuleFileNameW(nullptr, buf, 1023);
    return narrow(buf);
}
std::string exe_dir() {
    const std::string p = exe_path();
    const size_t cut = p.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : p.substr(0, cut);
}

double now_ms() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

std::string slurp(const std::string& path, const char* what) {
    std::string out, err;
    if (!read_file(path, out, err))
        throw Fail("E_IO", ssprintf("cannot read %s '%s': %s", what, path.c_str(), err.c_str()));
    return out;
}

// ======================================================================
// JSON
// ======================================================================
JV& JV::set(const std::string& k, JV v) {
    for (auto& kv : obj)
        if (kv.first == k) {
            kv.second = std::move(v);
            return *this;
        }
    obj.emplace_back(k, std::move(v));
    return *this;
}
const JV* JV::get(const char* k) const {
    for (const auto& kv : obj)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

namespace {

void put_string(std::string& out, std::string_view raw) {
    const std::string s = valid_utf8(raw) ? std::string(raw) : clean_utf8(raw);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += ssprintf("\\u%04x", c);
                else out += (char)c;
        }
    }
    out += '"';
}

// RFC 8785 orders keys by UTF-16 code units. For ASCII that is byte order; otherwise decode.
std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_len(s, i);
        uint32_t cp = 0xFFFD;
        const auto b = [&](size_t k) { return (uint32_t)(unsigned char)s[i + k]; };
        if (n == 1) cp = b(0);
        else if (n == 2) cp = ((b(0) & 0x1F) << 6) | (b(1) & 0x3F);
        else if (n == 3) cp = ((b(0) & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
        else if (n == 4) cp = ((b(0) & 0x07) << 18) | ((b(1) & 0x3F) << 12) | ((b(2) & 0x3F) << 6) | (b(3) & 0x3F);
        i += n ? n : 1;
        if (cp < 0x10000) out += (char16_t)cp;
        else {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    return out;
}

void put(std::string& out, const JV& v, bool canonical) {
    switch (v.t) {
        case JV::Null: out += "null"; break;
        case JV::Bool: out += v.b ? "true" : "false"; break;
        case JV::Int: out += std::to_string(v.i); break;
        case JV::Num: out += num(v.d); break;
        case JV::Str: put_string(out, v.s); break;
        case JV::Arr:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                put(out, v.arr[i], canonical);
            }
            out += ']';
            break;
        case JV::Obj: {
            std::vector<const std::pair<std::string, JV>*> order;
            order.reserve(v.obj.size());
            for (const auto& kv : v.obj) order.push_back(&kv);
            if (canonical)
                std::stable_sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return utf16(a->first) < utf16(b->first); });
            out += '{';
            for (size_t i = 0; i < order.size(); ++i) {
                if (i) out += ',';
                put_string(out, order[i]->first);
                out += ':';
                put(out, order[i]->second, canonical);
            }
            out += '}';
            break;
        }
    }
}

void put_pretty(std::string& out, const JV& v, int indent, int level) {
    const auto pad = [&](int l) { out.append((size_t)(indent * l), ' '); };
    if (v.t == JV::Obj && !v.obj.empty()) {
        out += "{\n";
        for (size_t i = 0; i < v.obj.size(); ++i) {
            pad(level + 1);
            put_string(out, v.obj[i].first);
            out += ": ";
            put_pretty(out, v.obj[i].second, indent, level + 1);
            out += i + 1 < v.obj.size() ? ",\n" : "\n";
        }
        pad(level);
        out += '}';
        return;
    }
    if (v.t == JV::Arr && !v.arr.empty()) {
        bool scalars = true;
        for (const auto& x : v.arr)
            if (x.t == JV::Arr || x.t == JV::Obj) scalars = false;
        if (scalars) {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ", ";
                put(out, v.arr[i], false);
            }
            out += ']';
            return;
        }
        out += "[\n";
        for (size_t i = 0; i < v.arr.size(); ++i) {
            pad(level + 1);
            put_pretty(out, v.arr[i], indent, level + 1);
            out += i + 1 < v.arr.size() ? ",\n" : "\n";
        }
        pad(level);
        out += ']';
        return;
    }
    put(out, v, false);
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    bool fail(const char* what) {
        if (err.empty()) {
            int line = 1, col = 1;
            for (size_t k = 0; k < i && k < s.size(); ++k) {
                if (s[k] == '\n') { ++line; col = 1; }
                else ++col;
            }
            err = ssprintf("line %d, column %d: %s", line, col, what);
        }
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool hex4(unsigned& cp) {
        if (i + 4 > s.size()) return fail("a \\u escape needs four hex digits");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hex(s[i + (size_t)k]);
            if (h < 0) return fail("a \\u escape needs four hex digits");
            cp = cp * 16 + (unsigned)h;
        }
        i += 4;
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) { --i; return fail("a control character inside a string must be escaped"); }
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        const size_t keep = i;
                        i += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else i = keep;
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;       // a lone surrogate
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xF0 | (cp >> 18));
                        out += (char)(0x80 | ((cp >> 12) & 0x3F));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: --i; return fail("unknown escape in a string");
            }
        }
        return fail("a string is not closed");
    }
    bool number(JV& out) {
        const size_t start = i;
        const auto digit = [&](size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; };
        if (i < s.size() && s[i] == '-') ++i;
        if (!digit(i)) return fail("expected a value");
        if (s[i] == '0') ++i;
        else while (digit(i)) ++i;
        bool integral = true;
        if (i < s.size() && s[i] == '.') {
            integral = false;
            ++i;
            if (!digit(i)) return fail("a digit must follow the decimal point");
            while (digit(i)) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            integral = false;
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!digit(i)) return fail("a digit must follow the exponent");
            while (digit(i)) ++i;
        }
        const std::string_view tok = s.substr(start, i - start);
        double v = 0;
        const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
        if (r.ec != std::errc() || !std::isfinite(v)) { i = start; return fail("the number is out of range"); }
        long long iv = 0;
        if (integral && to_i64(tok, iv)) out = JV(iv);
        else out = JV(v);
        return true;
    }
    bool value(JV& out, int depth) {
        if (depth > 64) return fail("nested too deeply");
        ws();
        if (i >= s.size()) return fail("expected a value");
        const char c = s[i];
        if (c == '{') {
            out = JV::object();
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return fail("expected a key in quotes");
                if (out.get(key.c_str())) return fail("an object has the same key twice");
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                JV v;
                if (!value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out = JV::array();
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                JV v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out = JV("");
            return string(out.s);
        }
        if (s.compare(i, 4, "true") == 0) { out = JV(true); i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = JV(false); i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { out = JV(); i += 4; return true; }
        return number(out);
    }
};

}  // namespace

bool json_parse(std::string_view text, JV& out, std::string& err) {
    Parser p;
    p.s = text;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;    // a byte-order mark
    if (!p.value(out, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) {
        p.fail("unexpected text after the value");
        err = p.err;
        return false;
    }
    return true;
}

std::string json_compact(const JV& v) {
    std::string out;
    put(out, v, false);
    return out;
}
std::string json_canonical(const JV& v) {
    std::string out;
    put(out, v, true);
    return out;
}
std::string json_pretty(const JV& v, int indent) {
    std::string out;
    put_pretty(out, v, indent, 0);
    out += '\n';
    return out;
}

}  // namespace quill
