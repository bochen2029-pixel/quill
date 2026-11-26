// quill · npz.h — numpy's .npy and .npz, read and written with nothing but the standard library.
// Reading: a ZIP with stored or deflated entries (ZIP64 sizes included) and .npy headers 1.0 to 3.0.
// Writing: the layout numpy itself writes (stored entries, ZIP64 local headers, 1980-01-01 stamps),
// so the same arrays give the same bytes as numpy.savez.
#pragma once
#include "quill.h"

namespace quill {

uint32_t crc32(std::string_view bytes);

// RFC 1951. Fills `out` with exactly `expect` bytes, or says why not.
bool inflate(std::string_view in, size_t expect, std::string& out, std::string& err);

struct ZipEntry {
    std::string name;
    unsigned method = 0;          // 0 stored, 8 deflate
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, offset = 0;
};
// The central directory of a ZIP file. Throws Fail(E_FORMAT).
std::vector<ZipEntry> zip_list(std::string_view file, const std::string& label);
// One entry's bytes, inflated if needed, with its CRC-32 checked. Throws Fail(E_FORMAT).
std::string zip_extract(std::string_view file, const ZipEntry& e, const std::string& label);

struct NpyArray {
    std::string name;             // "W1" for the entry "W1.npy"
    std::string descr;            // "<f8"
    bool fortran = false;
    std::vector<size_t> shape;
    std::string data;             // the raw element bytes
    size_t count() const {
        size_t n = 1;
        for (size_t s : shape) n *= s;
        return n;
    }
    std::string shape_text() const;
};
NpyArray npy_parse(std::string_view bytes, const std::string& name, const std::string& label);
// The values as float64 in C order. Accepts float64 and float32 of either byte order.
std::vector<double> npy_f64(const NpyArray& a, const std::string& label);

// A float64 array as numpy.save writes it (format 1.0, C order, header padded to 64 bytes).
std::string npy_from_f64(const std::vector<size_t>& shape, const double* values);
// A stored .npz as numpy.savez writes it. `members` are (name without ".npy", .npy bytes).
std::string npz_store(const std::vector<std::pair<std::string, std::string>>& members);
// Every array of an .npz, in the file's order.
std::vector<NpyArray> npz_read(std::string_view file, const std::string& label);

}  // namespace quill
