/*
    uvunwrap.h -- --uv unwrap: new UVs for a remeshed mesh, computed by
    xatlas (ext/xatlas, MIT): charts grown on the surface, flattened (LSCM),
    packed into one square atlas, normalized to [0, 1].
*/

#pragma once

#include "meshio.h"

struct UnwrapStats {
    uint32_t charts = 0;       ///< UV islands
    uint32_t resolution = 0;   ///< atlas size in texels (the larger side)
    float utilization = 0;     ///< share of the UV square covered by the faces
    uint32_t split = 0;        ///< polygons xatlas had cut between charts, made whole (then re-packed)
    uint32_t mirrored = 0;     ///< charts xatlas had mirrored, flipped back in place
};

/// One UV set for an extracted mesh (F with 3 or 4 rows, see extracted_polygons())
extern CornerUVs unwrap_uvs(const MatrixXu &F, const MatrixXf &O, const std::string &name = "UVMap",
                            UnwrapStats *stats = nullptr);
