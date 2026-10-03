/*
    test_main.cpp -- Unit test runner (target im_tests). Exit code 0 = all passed.

    Usage: im_tests          quick run (a few seconds, run after every build)
           im_tests --long   10x more fuzzing iterations
*/

#include "test_common.h"

int g_failed = 0, g_passed = 0;

int main(int argc, char **argv) {
    const bool long_run = argc > 1 && std::string(argv[1]) == "--long";
    test_meshio();
    test_ogawa(long_run ? 10 : 1);
    test_abc(long_run ? 10 : 1);
    test_objscene();
    test_border();
    test_uv();
    std::cout << std::endl << g_passed << " passed, " << g_failed << " failed" << std::endl;
    return g_failed == 0 ? 0 : 1;
}
