// One-shot helper: hash baserom.us.z64 with XXH3_64bits (the same function
// librecomp's check_hash uses) and print the result. Built and run by hand
// the first time; the resulting constant goes into main.cpp's GameEntry.
//
// Usage:
//   hash_rom.exe <path-to-rom>

#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <rom-path>\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "Cannot open %s\n", argv[1]);
        return 1;
    }
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);

    // Mirror select_rom's "pad to 4 bytes" step before hashing.
    buf.resize((buf.size() + 3) & ~size_t{3});

    uint64_t h = XXH3_64bits(buf.data(), buf.size());
    std::printf("0x%016llXULL\n", static_cast<unsigned long long>(h));
    return 0;
}
