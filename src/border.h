/*
    border.h -- --keep-border: the open border of a remeshed mesh is snapped
    back onto the border of its input, so that objects touching along their
    borders (floor plates, tiles) still touch after being remeshed separately
*/

#pragma once

#include "common.h"

/// Open border of a mesh (edges used by a single face), as polylines
struct BorderCurves {
    struct Chain {
        std::vector<Vector3f> points;
        std::vector<Float> s;          ///< arc length at each point
        std::vector<bool> corner;      ///< kept between snapped vertices
        bool closed = false;
        Float length = 0;              ///< total, including the closing segment
    };
    std::vector<Chain> chains;

    bool empty() const { return chains.empty(); }
};

/// Border of a triangle mesh (edges used by a single triangle)
extern BorderCurves extract_border(const MatrixXu &F, const MatrixXf &V);

struct BorderSnapStats {
    uint32_t borderVertices = 0;   ///< open border vertices of the output
    uint32_t snapped = 0;          ///< moved onto the input border
    uint32_t tooFar = 0;           ///< left in place: farther than 'maxDistance'
    uint32_t inserted = 0;         ///< input border corners added
    uint32_t faces = 0;            ///< faces that received corners
};

/**
 * Snaps the open border vertices of an extracted mesh (F with 3 or 4 rows,
 * see extracted_polygons()) onto the nearest input border within
 * 'maxDistance', then inserts the input border corners found between two
 * snapped neighbours. Quads that receive corners become polygons (4-row F)
 * or are triangulated (3-row F); 'Nf' follows the new columns.
 */
extern BorderSnapStats snap_to_border(const BorderCurves &border, Float maxDistance,
                                      MatrixXu &F, MatrixXf &O, MatrixXf &Nf);
