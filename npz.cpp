// quill · npz.cpp — CRC-32, inflate, ZIP and .npy: just enough to share a weight file with numpy.
#include "npz.h"

namespace quill {

// ======================================================================
// CRC-32 (the ZIP polynomial, reflected)
// ======================================================================
uint32_t crc32(std::string_view bytes) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ======================================================================
// inflate (RFC 1951). Bit by bit: these files are a few kilobytes.
// ======================================================================
namespace {

struct Bits {
    const unsigned char* p = nullptr;
    size_t n = 0, pos = 0;
    unsigned buf = 0;
    int cnt = 0;
    bool ran_out = false;
    unsigned bit() {
        if (cnt == 0) {
            if (pos >= n) { ran_out = true; return 0; }
            buf = p[pos++];
            cnt = 8;
        }
        const unsigned b = buf & 1u;
        buf >>= 1;
        --cnt;
        return b;
    }
    unsigned bits(int need) {
        unsigned v = 0;
        for (int k = 0; k < need; ++k) v |= bit() << k;
        return v;
    }
};

// A canonical Huffman code: how many codes of each length, and the symbols in code order.
struct Huff {
    uint16_t count[16] = {};
    uint16_t symbol[288] = {};
};

// Returns 0 for a complete code, a positive number for an incomplete one, negative if over-subscribed.
int build(Huff& h, const uint8_t* len, int n) {
    for (auto& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) h.count[len[s]]++;
    if (h.count[0] == n) return 0;                  // no codes at all
    int left = 1;
    for (int l = 1; l <= 15; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    uint16_t offs[16] = {};
    for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s)
        if (len[s]) h.symbol[offs[len[s]]++] = (uint16_t)s;
    return left;
}

int decode(Bits& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; ++l) {
        code |= (int)s.bit();
        const int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

const char* codes(Bits& s, std::string& out, size_t expect, const Huff& lencode, const Huff& distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (s.ran_out) return "the deflate stream ends early";
        if (sym < 0) return "a bad literal or length code";
        if (sym < 256) {
            if (out.size() >= expect) return "the deflate stream holds more than its stated size";
            out += (char)sym;
            continue;
        }
        if (sym == 256) return nullptr;
        sym -= 257;
        if (sym >= 29) return "a bad length code";
        const size_t len = (size_t)kLenBase[sym] + s.bits(kLenExtra[sym]);
        const int dsym = decode(s, distcode);
        if (dsym < 0 || dsym >= 30) return "a bad distance code";
        const size_t dist = (size_t)kDistBase[dsym] + s.bits(kDistExtra[dsym]);
        if (s.ran_out) return "the deflate stream ends early";
        if (dist > out.size()) return "a distance reaches before the start of the data";
        if (out.size() + len > expect) return "the deflate stream holds more than its stated size";
        for (size_t k = 0; k < len; ++k) out += out[out.size() - dist];
    }
}

}  // namespace

bool inflate(std::string_view in, size_t expect, std::string& out, std::string& err) {
    Bits s;
    s.p = (const unsigned char*)in.data();
    s.n = in.size();
    out.clear();
    out.reserve(expect);
    const auto fail = [&](const char* what) { err = what; return false; };
    for (;;) {
        const unsigned last = s.bit();
        const unsigned type = s.bits(2);
        if (s.ran_out) return fail("the deflate stream ends early");
        if (type == 0) {
            s.cnt = 0;                               // stored: skip to the byte boundary
            if (s.pos + 4 > s.n) return fail("the deflate stream ends early");
            const unsigned len = s.p[s.pos] | (s.p[s.pos + 1] << 8);
            const unsigned nlen = s.p[s.pos + 2] | (s.p[s.pos + 3] << 8);
            s.pos += 4;
            if (len != (~nlen & 0xFFFFu)) return fail("a stored block's length check fails");
            if (s.pos + len > s.n) return fail("the deflate stream ends early");
            if (out.size() + len > expect) return fail("the deflate stream holds more than its stated size");
            out.append((const char*)s.p + s.pos, len);
            s.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lencode, distcode;
            uint8_t lengths[320] = {};
            if (type == 1) {
                int k = 0;
                for (; k < 144; ++k) lengths[k] = 8;
                for (; k < 256; ++k) lengths[k] = 9;
                for (; k < 280; ++k) lengths[k] = 7;
                for (; k < 288; ++k) lengths[k] = 8;
                build(lencode, lengths, 288);
                for (k = 0; k < 30; ++k) lengths[k] = 5;
                build(distcode, lengths, 30);
            } else {
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                const int nlen = (int)s.bits(5) + 257, ndist = (int)s.bits(5) + 1, ncode = (int)s.bits(4) + 4;
                if (s.ran_out) return fail("the deflate stream ends early");
                if (nlen > 286 || ndist > 30) return fail("a dynamic block declares too many codes");
                for (int k = 0; k < ncode; ++k) lengths[order[k]] = (uint8_t)s.bits(3);
                for (int k = ncode; k < 19; ++k) lengths[order[k]] = 0;
                Huff cl;
                if (build(cl, lengths, 19) != 0) return fail("a dynamic block's code-length code is not complete");
                int idx = 0;
                while (idx < nlen + ndist) {
                    const int sym = decode(s, cl);
                    if (s.ran_out) return fail("the deflate stream ends early");
                    if (sym < 0) return fail("a bad code-length code");
                    if (sym < 16) {
                        lengths[idx++] = (uint8_t)sym;
                        continue;
                    }
                    uint8_t prev = 0;
                    int rep = 0;
                    if (sym == 16) {
                        if (idx == 0) return fail("a repeat code with nothing to repeat");
                        prev = lengths[idx - 1];
                        rep = 3 + (int)s.bits(2);
                    } else if (sym == 17) rep = 3 + (int)s.bits(3);
                    else rep = 11 + (int)s.bits(7);
                    if (idx + rep > nlen + ndist) return fail("a repeat code runs past the code lengths");
                    while (rep--) lengths[idx++] = prev;
                }
                if (lengths[256] == 0) return fail("a dynamic block has no end-of-block code");
                int left = build(lencode, lengths, nlen);
                if (left < 0 || (left > 0 && nlen - lencode.count[0] != 1)) return fail("a bad literal and length code set");
                left = build(distcode, lengths + nlen, ndist);
                if (left < 0 || (left > 0 && ndist - distcode.count[0] != 1)) return fail("a bad distance code set");
            }
            if (const char* what = codes(s, out, expect, lencode, distcode)) return fail(what);
        } else {
            return fail("a bad block type");
        }
        if (last) break;
    }
    if (out.size() != expect) return fail("the deflate stream holds less than its stated size");
    return true;
}

// ======================================================================
// ZIP
// ======================================================================
namespace {

unsigned u16(std::string_view f, size_t at) { return (unsigned char)f[at] | ((unsigned)(unsigned char)f[at + 1] << 8); }
uint32_t u32(std::string_view f, size_t at) { return (uint32_t)u16(f, at) | ((uint32_t)u16(f, at + 2) << 16); }
uint64_t u64(std::string_view f, size_t at) { return (uint64_t)u32(f, at) | ((uint64_t)u32(f, at + 4) << 32); }

[[noreturn]] void bad(const std::string& label, const std::string& what) {
    throw Fail("E_FORMAT", label + ": " + what);
}

}  // namespace

std::vector<ZipEntry> zip_list(std::string_view f, const std::string& label) {
    if (f.size() < 22) bad(label, "not a ZIP file (shorter than its end record)");
    size_t eocd = std::string::npos;
    const size_t lowest = f.size() > 22 + 65535 ? f.size() - 22 - 65535 : 0;
    for (size_t at = f.size() - 22 + 1; at-- > lowest;)
        if (u32(f, at) == 0x06054b50u) { eocd = at; break; }
    if (eocd == std::string::npos) bad(label, "not a ZIP file (no end record)");
    uint64_t count = u16(f, eocd + 10), cd_size = u32(f, eocd + 12), cd_off = u32(f, eocd + 16);
    if (u16(f, eocd + 4) != 0 || u16(f, eocd + 6) != 0) bad(label, "a ZIP file split over several disks is not supported");
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {      // ZIP64 end record
        if (eocd < 20 || u32(f, eocd - 20) != 0x07064b50u) bad(label, "the ZIP64 end locator is missing");
        const uint64_t at = u64(f, eocd - 20 + 8);
        if (at + 56 > f.size() || u32(f, (size_t)at) != 0x06064b50u) bad(label, "the ZIP64 end record is missing");
        count = u64(f, (size_t)at + 32);
        cd_size = u64(f, (size_t)at + 40);
        cd_off = u64(f, (size_t)at + 48);
    }
    if (cd_off > f.size() || cd_size > f.size() - cd_off) bad(label, "the ZIP directory lies outside the file");
    std::vector<ZipEntry> out;
    size_t at = (size_t)cd_off;
    const size_t end = (size_t)(cd_off + cd_size);
    for (uint64_t k = 0; k < count; ++k) {
        if (at + 46 > end || u32(f, at) != 0x02014b50u) bad(label, "a ZIP directory entry is damaged");
        ZipEntry e;
        const unsigned flags = u16(f, at + 8);
        e.method = u16(f, at + 10);
        e.crc = u32(f, at + 16);
        e.csize = u32(f, at + 20);
        e.usize = u32(f, at + 24);
        const size_t nlen = u16(f, at + 28), xlen = u16(f, at + 30), clen = u16(f, at + 32);
        e.offset = u32(f, at + 42);
        if (at + 46 + nlen + xlen + clen > end) bad(label, "a ZIP directory entry is damaged");
        e.name = std::string(f.substr(at + 46, nlen));
        if (flags & 1u) bad(label, "the entry '" + e.name + "' is encrypted");
        for (size_t x = at + 46 + nlen, xend = x + xlen; x + 4 <= xend;) {         // ZIP64 sizes
            const unsigned id = u16(f, x), size = u16(f, x + 2);
            size_t p = x + 4;
            if (id == 1) {
                if (e.usize == 0xFFFFFFFFu && p + 8 <= xend) { e.usize = u64(f, p); p += 8; }
                if (e.csize == 0xFFFFFFFFu && p + 8 <= xend) { e.csize = u64(f, p); p += 8; }
                if (e.offset == 0xFFFFFFFFu && p + 8 <= xend) { e.offset = u64(f, p); p += 8; }
            }
            x += 4 + (size_t)size;
        }
        out.push_back(std::move(e));
        at += 46 + nlen + xlen + clen;
    }
    return out;
}

std::string zip_extract(std::string_view f, const ZipEntry& e, const std::string& label) {
    const std::string who = label + ": entry '" + e.name + "'";
    if (e.offset > f.size() || f.size() - e.offset < 30 || u32(f, (size_t)e.offset) != 0x04034b50u)
        bad(who, "its local header is missing");
    const size_t at = (size_t)e.offset;
    const size_t start = at + 30 + u16(f, at + 26) + u16(f, at + 28);
    if (start > f.size() || e.csize > f.size() - start) bad(who, "its data lies outside the file");
    const std::string_view raw = f.substr(start, (size_t)e.csize);
    std::string out;
    if (e.method == 0) {
        if (e.csize != e.usize) bad(who, "a stored entry whose two sizes differ");
        out = std::string(raw);
    } else if (e.method == 8) {
        std::string err;
        if (!inflate(raw, (size_t)e.usize, out, err)) bad(who, err);
    } else {
        bad(who, ssprintf("compression method %u is not supported (stored and deflate are)", e.method));
    }
    if (crc32(out) != e.crc) bad(who, "the CRC-32 does not match: the file is damaged");
    return out;
}

// ======================================================================
// .npy
// ======================================================================
std::string NpyArray::shape_text() const {
    std::string s = "(";
    for (size_t i = 0; i < shape.size(); ++i) s += (i ? ", " : "") + std::to_string(shape[i]);
    if (shape.size() == 1) s += ",";
    return s + ")";
}

namespace {

// The text of one value in numpy's header dictionary: {'descr': '<f8', 'fortran_order': False, 'shape': (4, 32), }
bool header_value(std::string_view h, const char* key, std::string& out) {
    for (const char q : { '\'', '"' }) {
        const std::string k = std::string(1, q) + key + q;
        size_t at = h.find(k);
        if (at == std::string::npos) continue;
        at += k.size();
        while (at < h.size() && (h[at] == ' ' || h[at] == ':')) ++at;
        if (at >= h.size()) return false;
        if (h[at] == '\'' || h[at] == '"') {
            const size_t close = h.find(h[at], at + 1);
            if (close == std::string::npos) return false;
            out = std::string(h.substr(at + 1, close - at - 1));
            return true;
        }
        if (h[at] == '(') {
            const size_t close = h.find(')', at);
            if (close == std::string::npos) return false;
            out = std::string(h.substr(at + 1, close - at - 1));
            return true;
        }
        size_t stop = at;
        while (stop < h.size() && h[stop] != ',' && h[stop] != '}') ++stop;
        out = trim(h.substr(at, stop - at));
        return true;
    }
    return false;
}

size_t item_size(const std::string& descr) {
    if (descr.size() < 3) return 0;
    unsigned long long n = 0;
    if (!to_u64(std::string_view(descr).substr(2), n)) return 0;
    const char kind = descr[1];
    if (kind == 'f') return (n == 4 || n == 8) ? (size_t)n : 0;
    if (kind == 'i' || kind == 'u') return (n == 1 || n == 2 || n == 4 || n == 8) ? (size_t)n : 0;
    if (kind == 'b') return n == 1 ? 1 : 0;
    return 0;
}

}  // namespace

NpyArray npy_parse(std::string_view b, const std::string& name, const std::string& label) {
    const std::string who = label + ": array '" + name + "'";
    if (b.size() < 10 || b.compare(0, 6, "\x93NUMPY") != 0) bad(who, "not a .npy file");
    const unsigned major = (unsigned char)b[6];
    size_t hlen = 0, hstart = 0;
    if (major == 1) { hlen = u16(b, 8); hstart = 10; }
    else if (major == 2 || major == 3) {
        if (b.size() < 12) bad(who, "not a .npy file");
        hlen = u32(b, 8);
        hstart = 12;
    } else bad(who, ssprintf(".npy format %u is not supported (1 to 3 are)", major));
    if (hstart + hlen > b.size()) bad(who, "its header runs past the end of the file");
    const std::string_view h = b.substr(hstart, hlen);
    NpyArray a;
    a.name = name;
    std::string order, shape;
    if (!header_value(h, "descr", a.descr) || !header_value(h, "fortran_order", order) || !header_value(h, "shape", shape))
        bad(who, "its header lacks descr, fortran_order or shape");
    a.fortran = order == "True";
    for (size_t i = 0; i < shape.size();) {
        size_t j = shape.find(',', i);
        if (j == std::string::npos) j = shape.size();
        const std::string part = trim(std::string_view(shape).substr(i, j - i));
        if (!part.empty()) {
            unsigned long long v = 0;
            if (!to_u64(part, v)) bad(who, "its shape is not a tuple of integers");
            a.shape.push_back((size_t)v);
        }
        i = j + 1;
    }
    const size_t item = item_size(a.descr);
    if (!item) bad(who, "its element type '" + a.descr + "' is not supported (plain floats and integers are)");
    a.data = std::string(b.substr(hstart + hlen));
    if (a.data.size() != a.count() * item)
        bad(who, ssprintf("it holds %zu bytes of data but its shape %s of '%s' needs %zu", a.data.size(),
                          a.shape_text().c_str(), a.descr.c_str(), a.count() * item));
    return a;
}

std::vector<double> npy_f64(const NpyArray& a, const std::string& label) {
    const std::string who = label + ": array '" + a.name + "'";
    const size_t item = item_size(a.descr), n = a.count();
    const char kind = a.descr[1];
    const bool swap = a.descr[0] == '>';
    std::vector<double> flat(n);
    for (size_t k = 0; k < n; ++k) {
        unsigned char raw[8] = {};
        for (size_t x = 0; x < item; ++x) raw[x] = (unsigned char)a.data[k * item + (swap ? item - 1 - x : x)];
        double v = 0;
        if (kind == 'f' && item == 8) memcpy(&v, raw, 8);
        else if (kind == 'f') { float f32 = 0; memcpy(&f32, raw, 4); v = f32; }
        else if (kind == 'u' || kind == 'b') { uint64_t u = 0; memcpy(&u, raw, 8); v = (double)u; }
        else {
            uint64_t u = 0;
            memcpy(&u, raw, 8);
            if (item < 8 && (u >> (8 * item - 1)) & 1u) u |= ~0ull << (8 * item);      // sign-extend
            v = (double)(int64_t)u;
        }
        flat[k] = v;
    }
    if (!a.fortran || a.shape.size() < 2) return flat;
    std::vector<double> c(n);                               // Fortran order -> C order
    std::vector<size_t> idx(a.shape.size(), 0);
    for (size_t k = 0; k < n; ++k) {
        size_t off = 0, stride = 1;
        for (size_t m = 0; m < a.shape.size(); ++m) {
            off += idx[m] * stride;
            stride *= a.shape[m];
        }
        if (off >= n) bad(who, "a damaged Fortran-order array");
        c[k] = flat[off];
        for (size_t m = a.shape.size(); m-- > 0;) {
            if (++idx[m] < a.shape[m]) break;
            idx[m] = 0;
        }
    }
    return c;
}

std::string npy_from_f64(const std::vector<size_t>& shape, const double* values) {
    NpyArray a;
    a.shape = shape;
    std::string header = "{'descr': '<f8', 'fortran_order': False, 'shape': " + a.shape_text() + ", }";
    // numpy pads so that magic + version + length + header is a multiple of 64 and ends in a newline
    const size_t pad = 64 - ((10 + header.size() + 1) % 64);
    header.append(pad, ' ');
    header += '\n';
    std::string out("\x93NUMPY\x01\x00", 8);
    out += (char)(header.size() & 0xFF);
    out += (char)((header.size() >> 8) & 0xFF);
    out += header;
    const size_t n = a.count();
    if (n) out.append((const char*)values, n * sizeof(double));      // x64 is little-endian
    return out;
}

std::string npz_store(const std::vector<std::pair<std::string, std::string>>& members) {
    const auto p16 = [](std::string& s, unsigned v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); };
    const auto p32 = [&](std::string& s, uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); };
    const auto p64 = [&](std::string& s, uint64_t v) { p32(s, (uint32_t)v); p32(s, (uint32_t)(v >> 32)); };
    std::string out, dir;
    for (const auto& m : members) {
        const std::string name = m.first + ".npy";
        const uint32_t crc = crc32(m.second);
        const uint32_t at = (uint32_t)out.size();
        p32(out, 0x04034b50u);
        p16(out, 45); p16(out, 0); p16(out, 0);              // version 4.5, no flags, stored
        p16(out, 0); p16(out, 0x21);                         // 00:00:00, 1980-01-01
        p32(out, crc);
        p32(out, 0xFFFFFFFFu); p32(out, 0xFFFFFFFFu);        // the sizes are in the ZIP64 field below
        p16(out, (unsigned)name.size()); p16(out, 20);
        out += name;
        p16(out, 1); p16(out, 16);
        p64(out, m.second.size()); p64(out, m.second.size());
        out += m.second;
        p32(dir, 0x02014b50u);
        p16(dir, 45); p16(dir, 45); p16(dir, 0); p16(dir, 0);
        p16(dir, 0); p16(dir, 0x21);
        p32(dir, crc);
        p32(dir, (uint32_t)m.second.size()); p32(dir, (uint32_t)m.second.size());
        p16(dir, (unsigned)name.size()); p16(dir, 0); p16(dir, 0);
        p16(dir, 0); p16(dir, 0);
        p32(dir, 0x01800000u);                               // the file mode zipfile gives a new entry
        p32(dir, at);
        dir += name;
    }
    const uint32_t dir_at = (uint32_t)out.size();
    out += dir;
    p32(out, 0x06054b50u);
    p16(out, 0); p16(out, 0);
    p16(out, (unsigned)members.size()); p16(out, (unsigned)members.size());
    p32(out, (uint32_t)dir.size());
    p32(out, dir_at);
    p16(out, 0);
    return out;
}

std::vector<NpyArray> npz_read(std::string_view file, const std::string& label) {
    std::vector<NpyArray> out;
    for (const auto& e : zip_list(file, label)) {
        std::string name = e.name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".npy") == 0) name.resize(name.size() - 4);
        else continue;                                       // not an array
        out.push_back(npy_parse(zip_extract(file, e, label), name, label));
    }
    return out;
}

}  // namespace quill
