/*
    association.h: .imd files opened by Instant Meshes with a double click
    (Windows: registered for the current user only, no administrator
    rights; the icon is resource 101 of the executable)
*/

#pragma once

#include <string>

/// Registers .imd for the current user; false and 'error' on failure
bool register_imd_files(std::string &error);

/// Whether .imd files open with this executable
bool imd_files_registered();
