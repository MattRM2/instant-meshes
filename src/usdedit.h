/*
    usdedit.h: edits of an existing USD layer that keep everything else as
    it is: references prepended to root prims (--proxy hooks its proxy layer
    into the scene this way).

    .usda: the text is edited where the references go, every other byte is
    kept. .usdc: the file is kept whole and its values stay where they are;
    the new values and new structural sections (tokens, strings, fields,
    field sets, paths, specs) are appended, then the table of contents is
    pointed at them (the old sections remain, unused).
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usd {

/// A reference prepended to a root prim: prepend references = @asset@<target>
struct RootReference {
    std::string prim;     ///< name of the root prim ("World"); an over is created if the layer has none
    std::string asset;    ///< asset path ("./scene_proxy.usda")
    std::string target;   ///< prim of the asset ("/World")
};

/// A .usda layer's text with the references added ('changed': false if
/// they were all there already)
std::string usda_add_references(const std::string &text, const std::vector<RootReference> &refs,
                                bool *changed = nullptr);

/// A .usdc layer ('filename', or the range of a package) with the
/// references added: the original bytes, then what is appended to them
/// (the table of contents offset, bytes 16-23, is updated in the result)
std::vector<uint8_t> usdc_add_references(const std::string &filename, const std::vector<RootReference> &refs,
                                         bool *changed = nullptr, uint64_t start = 0, uint64_t size = 0);

} // namespace usd
