/*
    test_common.h -- Minimal test helpers shared by the unit tests (im_tests)
*/

#pragma once

#include "common.h"
#include <fstream>
#include <functional>

extern int g_failed, g_passed;

#define CHECK(cond) do { \
    if (cond) { ++g_passed; } else { \
        ++g_failed; \
        std::cerr << "  FAILED: " #cond "  (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
    } } while (0)

inline std::string data_path(const std::string &name) {
    return std::string(IM_TEST_DATA_DIR) + "/" + name;
}

inline std::string temp_path(const std::string &name) {
    return std::string(IM_TEST_TEMP_DIR) + "/" + name;
}

inline void write_file(const std::string &path, const std::string &content) {
    std::ofstream os(path, std::ios::binary);
    os << content;
}

inline void write_file(const std::string &path, const std::vector<uint8_t> &content) {
    std::ofstream os(path, std::ios::binary);
    os.write((const char *) content.data(), (std::streamsize) content.size());
}

inline std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream is(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(is)),
                                std::istreambuf_iterator<char>());
}

inline bool file_exists(const std::string &path) {
    return std::ifstream(path).good();
}

/* Runs 'fn' and returns the exception message ("" if nothing was thrown) */
inline std::string error_of(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "";
}

inline bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

void test_meshio();
void test_ogawa(int fuzz_scale);
void test_abc(int fuzz_scale);
