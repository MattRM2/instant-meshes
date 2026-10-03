/*
    uvtransfer.h -- --uv transfer: UV sets of an original mesh carried over
    to its remeshed version (or a proxy), island by island.

    Every face of the new mesh takes the UV island found under its centre
    (nearest point on the original, triangles facing the same way first);
    its corners are then projected onto that island only. Inside the island
    the UVs are interpolated; past its border (a UV seam) they are extended
    linearly from the nearest triangle instead of jumping to another island,
    so that no face stretches across the texture. The result is per face
    corner (face-varying): seams come out split.
*/

#pragma once

#include "meshio.h"
#include <memory>

class UVTransfer {
public:
    /// The original: triangles (3 x T), positions and UV sets per triangle corner
    UVTransfer(const MatrixXu &F, const MatrixXf &V, const std::vector<UVSet> &uvs);
    ~UVTransfer();

    struct Stats {
        uint64_t corners = 0;        ///< polygon corners of the new mesh
        uint64_t extended = 0;       ///< of which past an island border (extrapolated)
        uint64_t flipped = 0;        ///< faces whose source triangle faces the other way
    };

    /// UV sets for an extracted mesh (F with 3 or 4 rows, see extracted_polygons()),
    /// one per original set, in the same order
    std::vector<CornerUVs> transfer(const MatrixXu &F, const MatrixXf &O, Stats *stats = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
