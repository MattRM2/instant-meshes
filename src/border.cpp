/*
    border.cpp -- --keep-border: snaps the open border of a remeshed mesh
    back onto the border of its input (see border.h)
*/

#include "border.h"
#include "meshio.h"
#include <unordered_map>
#include <algorithm>
#include <map>
#include <set>

static const uint32_t NONE = (uint32_t) -1;

static inline uint64_t edge_key(uint32_t a, uint32_t b) {
    return ((uint64_t) a << 32) | b;
}

/* Border points bending less than this are straight runs: nothing to insert */
static const Float CORNER_ANGLE = (Float) (0.1 * M_PI / 180);

/* Positions this close to a border point (fraction of the segment) snap to it */
static const Float SNAP_TO_POINT = (Float) 1e-4;

BorderCurves extract_border(const MatrixXu &F, const MatrixXf &V) {
    BorderCurves result;
    std::unordered_map<uint64_t, uint32_t> count;
    count.reserve((size_t) F.cols() * 3);
    for (uint32_t f = 0; f < F.cols(); ++f)
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = F(j, f), b = F((j + 1) % 3, f);
            if (a != b)
                count[edge_key(a, b)]++;
        }

    /* Directed border edges, sorted so that the result does not depend on
       the hash table order */
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    for (const auto &kv : count) {
        const uint32_t a = (uint32_t) (kv.first >> 32), b = (uint32_t) kv.first;
        if (kv.second == 1 && count.find(edge_key(b, a)) == count.end())
            edges.emplace_back(a, b);
    }
    std::sort(edges.begin(), edges.end());

    std::unordered_map<uint32_t, std::vector<uint32_t>> out;
    std::unordered_map<uint32_t, uint32_t> incoming;
    for (uint32_t i = 0; i < edges.size(); ++i) {
        out[edges[i].first].push_back(i);
        incoming[edges[i].second]++;
    }

    std::vector<bool> used(edges.size(), false);
    auto walk = [&](uint32_t e) {
        const uint32_t start = edges[e].first;
        std::vector<uint32_t> ids { start };
        bool closed = false;
        while (true) {
            used[e] = true;
            const uint32_t b = edges[e].second;
            if (b == start) {
                closed = true;
                break;
            }
            ids.push_back(b);
            uint32_t next = NONE;
            auto it = out.find(b);
            if (it != out.end())
                for (uint32_t k : it->second)
                    if (!used[k]) {
                        next = k;
                        break;
                    }
            if (next == NONE)
                break;
            e = next;
        }
        if (ids.size() < 2)
            return;

        BorderCurves::Chain c;
        c.closed = closed;
        const size_t n = ids.size();
        for (uint32_t id : ids)
            c.points.push_back(V.col(id));
        c.s.resize(n);
        c.s[0] = 0;
        for (size_t k = 1; k < n; ++k)
            c.s[k] = c.s[k - 1] + (c.points[k] - c.points[k - 1]).norm();
        c.length = c.s[n - 1] + (closed ? (c.points[0] - c.points[n - 1]).norm() : 0);
        c.corner.resize(n);
        for (size_t k = 0; k < n; ++k) {
            if (!closed && (k == 0 || k == n - 1)) {
                c.corner[k] = true;
                continue;
            }
            const Vector3f d0 = c.points[k] - c.points[(k + n - 1) % n];
            const Vector3f d1 = c.points[(k + 1) % n] - c.points[k];
            c.corner[k] = d0.squaredNorm() > 0 && d1.squaredNorm() > 0 &&
                          std::atan2(d0.cross(d1).norm(), d0.dot(d1)) > CORNER_ANGLE;
        }
        result.chains.push_back(std::move(c));
    };

    /* Open chains from their first point, then the loops */
    for (uint32_t i = 0; i < edges.size(); ++i)
        if (!used[i] && incoming.find(edges[i].first) == incoming.end())
            walk(i);
    for (uint32_t i = 0; i < edges.size(); ++i)
        if (!used[i])
            walk(i);
    return result;
}

namespace {

/* A position on a border chain: segment 'index' (point index -> next
   point) at parameter 't' in [0, 1); t == 0 is the point itself */
struct Snap {
    uint32_t chain = NONE, index = 0;
    Float t = 0, s = 0;
    Vector3f p;
};

/* Uniform grid over the border segments, cell size >= the search radius */
class SegmentGrid {
public:
    SegmentGrid(const BorderCurves &border, Float radius)
        : mBorder(border), mRadius(radius), mCell(radius * 1.25f) {
        for (uint32_t c = 0; c < border.chains.size(); ++c) {
            const BorderCurves::Chain &chain = border.chains[c];
            const uint32_t n = (uint32_t) chain.points.size();
            const uint32_t segments = chain.closed ? n : n - 1;
            for (uint32_t i = 0; i < segments; ++i) {
                const uint32_t id = (uint32_t) mSegments.size();
                mSegments.emplace_back(c, i);
                /* Samples every quarter cell: every point of the segment is
                   within an eighth of a cell of one of them */
                const Vector3f p0 = chain.points[i], p1 = chain.points[(i + 1) % n];
                const Float len = (p1 - p0).norm();
                const uint64_t steps = std::max<uint64_t>(1, (uint64_t) std::ceil(len / (mCell / 4)));
                for (uint64_t k = 0; k <= steps; ++k) {
                    std::vector<uint32_t> &cell = mGrid[key(p0 + (p1 - p0) * (Float) ((double) k / steps))];
                    if (cell.empty() || cell.back() != id)
                        cell.push_back(id);
                }
            }
        }
    }

    /* Nearest border position within the radius (chain == NONE if none) */
    Snap nearest(const Vector3f &q) const {
        Snap best;
        Float bestDist = mRadius;
        const Vector3f c = q / mCell;
        const int64_t cx = (int64_t) std::floor(c.x()), cy = (int64_t) std::floor(c.y()),
                      cz = (int64_t) std::floor(c.z());
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    auto it = mGrid.find(pack(cx + dx, cy + dy, cz + dz));
                    if (it == mGrid.end())
                        continue;
                    for (uint32_t id : it->second) {
                        const BorderCurves::Chain &chain = mBorder.chains[mSegments[id].first];
                        const uint32_t i = mSegments[id].second, n = (uint32_t) chain.points.size();
                        const Vector3f p0 = chain.points[i], d = chain.points[(i + 1) % n] - p0;
                        const Float dd = d.squaredNorm();
                        const Float t = dd > 0 ? std::min((Float) 1, std::max((Float) 0, (q - p0).dot(d) / dd)) : 0;
                        const Float dist = (p0 + d * t - q).norm();
                        if (dist <= bestDist) {
                            bestDist = dist;
                            best.chain = mSegments[id].first;
                            best.index = i;
                            best.t = t;
                        }
                    }
                }
        if (best.chain == NONE)
            return best;

        /* Normalize: at a point, t == 0 on the segment that starts there */
        const BorderCurves::Chain &chain = mBorder.chains[best.chain];
        const uint32_t n = (uint32_t) chain.points.size();
        if (best.t <= SNAP_TO_POINT) {
            best.t = 0;
        } else if (best.t >= 1 - SNAP_TO_POINT) {
            best.index = chain.closed ? (best.index + 1) % n : best.index + 1;
            best.t = 0;
        }
        if (best.t == 0) {
            best.p = chain.points[best.index];
            best.s = chain.s[best.index];
        } else {
            const Vector3f p0 = chain.points[best.index], p1 = chain.points[(best.index + 1) % n];
            best.p = p0 + (p1 - p0) * best.t;
            best.s = chain.s[best.index] + (p1 - p0).norm() * best.t;
        }
        return best;
    }

private:
    static uint64_t pack(int64_t x, int64_t y, int64_t z) {
        const uint64_t m = (1ull << 21) - 1;
        return (((uint64_t) x & m) << 42) | (((uint64_t) y & m) << 21) | ((uint64_t) z & m);
    }
    uint64_t key(const Vector3f &p) const {
        const Vector3f c = p / mCell;
        return pack((int64_t) std::floor(c.x()), (int64_t) std::floor(c.y()), (int64_t) std::floor(c.z()));
    }

    const BorderCurves &mBorder;
    Float mRadius, mCell;
    std::vector<std::pair<uint32_t, uint32_t>> mSegments;
    std::unordered_map<uint64_t, std::vector<uint32_t>> mGrid;
};

/* Corner points of a chain strictly between two snapped positions, in the
   direction of the shorter way along the chain (empty when that way is not
   plausible for two neighbouring border vertices) */
std::vector<uint32_t> corners_between(const BorderCurves::Chain &chain, const Snap &a,
                                      const Snap &b, Float maxArc) {
    std::vector<uint32_t> result;
    const uint32_t n = (uint32_t) chain.points.size();
    bool forward;
    Float arc;
    if (chain.closed) {
        Float df = std::fmod(b.s - a.s + chain.length, chain.length);
        if (!(df >= 0))
            df = 0;
        forward = df <= chain.length - df;
        arc = forward ? df : chain.length - df;
    } else {
        forward = b.s >= a.s;
        arc = std::abs(b.s - a.s);
    }
    if (arc > maxArc)
        return result;

    if (forward) {
        /* Points a.index+1 .. b.index (b.index itself only if b is past it) */
        const uint32_t count = chain.closed ? (b.index + n - a.index) % n
                                            : (b.index >= a.index ? b.index - a.index : 0);
        for (uint32_t m = 1; m <= count; ++m) {
            const uint32_t k = (a.index + m) % n;
            if (m == count && b.t == 0)
                break;
            if (chain.corner[k])
                result.push_back(k);
        }
    } else {
        /* Points a.index (only if a is past it) down to b.index+1 */
        const uint32_t count = chain.closed ? (a.index + n - b.index) % n
                                            : (a.index >= b.index ? a.index - b.index : 0);
        for (uint32_t m = 0; m < count; ++m) {
            const uint32_t k = (a.index + n - m) % n;
            if (m == 0 && a.t == 0)
                continue;
            if (chain.corner[k])
                result.push_back(k);
        }
    }
    return result;
}

} // namespace

BorderSnapStats snap_to_border(const BorderCurves &border, Float maxDistance,
                               MatrixXu &F, MatrixXf &O, MatrixXf &Nf) {
    BorderSnapStats stats;
    if (border.empty() || !(maxDistance > 0) || F.cols() == 0)
        return stats;
    const int R = (int) F.rows();
    const bool faceNormals = Nf.cols() == F.cols();

    /* Directed edges of the output: (a, b, column, corner; -1 = irregular) */
    struct DEdge { uint32_t a, b, col; int corner; };
    std::vector<DEdge> edges;
    for (uint32_t f = 0; f < F.cols(); ++f) {
        if (R == 4 && F(2, f) == F(3, f)) {
            if (F(0, f) != F(1, f))
                edges.push_back({ F(0, f), F(1, f), f, -1 });
            continue;
        }
        for (int j = 0; j < R; ++j) {
            const uint32_t a = F(j, f), b = F((j + 1) % R, f);
            if (a != b)
                edges.push_back({ a, b, f, j });
        }
    }
    std::unordered_map<uint64_t, uint32_t> present;
    present.reserve(edges.size());
    for (const DEdge &e : edges)
        present[edge_key(e.a, e.b)]++;

    std::vector<uint32_t> borderEdges;
    std::unordered_map<uint32_t, Snap> snaps;
    for (uint32_t i = 0; i < edges.size(); ++i)
        if (present.find(edge_key(edges[i].b, edges[i].a)) == present.end()) {
            borderEdges.push_back(i);
            snaps[edges[i].a];
            snaps[edges[i].b];
        }
    if (borderEdges.empty())
        return stats;

    /* Snap the border vertices (sorted for a deterministic vertex order) */
    const SegmentGrid grid(border, maxDistance);
    std::vector<uint32_t> borderVertices;
    for (const auto &kv : snaps)
        borderVertices.push_back(kv.first);
    std::sort(borderVertices.begin(), borderVertices.end());
    stats.borderVertices = (uint32_t) borderVertices.size();
    for (uint32_t v : borderVertices) {
        Snap s = grid.nearest(O.col(v));
        if (s.chain == NONE) {
            stats.tooFar++;
        } else {
            stats.snapped++;
        }
        snaps[v] = s;
    }

    /* Corners to insert along each border edge */
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> newVertex;   /* (chain, point) -> vertex */
    std::vector<Vector3f> added;
    std::map<uint32_t, std::vector<std::pair<int, std::vector<uint32_t>>>> byColumn;
    for (uint32_t i : borderEdges) {
        const DEdge &e = edges[i];
        const Snap &a = snaps[e.a], &b = snaps[e.b];
        if (a.chain == NONE || a.chain != b.chain)
            continue;
        const BorderCurves::Chain &chain = border.chains[a.chain];
        const Float maxArc = 4 * (b.p - a.p).norm() + 2 * maxDistance;
        std::vector<uint32_t> corners = corners_between(chain, a, b, maxArc);
        if (corners.empty())
            continue;
        std::vector<uint32_t> ids;
        for (uint32_t k : corners) {
            auto it = newVertex.find({ a.chain, k });
            if (it == newVertex.end()) {
                it = newVertex.insert({ { a.chain, k }, (uint32_t) (O.cols() + added.size()) }).first;
                added.push_back(chain.points[k]);
            }
            ids.push_back(it->second);
        }
        byColumn[e.col].emplace_back(e.corner, std::move(ids));
    }

    /* Move the snapped vertices, append the corners */
    for (uint32_t v : borderVertices)
        if (snaps[v].chain != NONE)
            O.col(v) = snaps[v].p;
    if (!added.empty()) {
        const uint32_t base = (uint32_t) O.cols();
        O.conservativeResize(3, base + added.size());
        for (size_t i = 0; i < added.size(); ++i)
            O.col(base + i) = added[i];
    }
    stats.inserted = (uint32_t) added.size();

    /* Rebuild the faces that received corners */
    std::vector<Eigen::Matrix<uint32_t, Eigen::Dynamic, 1>> newCols;
    std::vector<uint32_t> newColSource;
    std::set<uint32_t> usedIds;
    auto emit = [&](uint32_t f, bool first, const Eigen::Matrix<uint32_t, Eigen::Dynamic, 1> &col) {
        if (first) {
            F.col(f) = col;
        } else {
            newCols.push_back(col);
            newColSource.push_back(f);
        }
    };
    for (auto &item : byColumn) {
        const uint32_t f = item.first;
        stats.faces++;
        Eigen::Matrix<uint32_t, Eigen::Dynamic, 1> col(R);

        if (R == 4 && F(2, f) == F(3, f)) {
            /* Edge of an irregular polygon: split it, same polygon id */
            const uint32_t id = F(2, f);
            std::vector<uint32_t> chainIds { F(0, f) };
            for (auto &ins : item.second)
                chainIds.insert(chainIds.end(), ins.second.begin(), ins.second.end());
            chainIds.push_back(F(1, f));
            for (size_t i = 0; i + 1 < chainIds.size(); ++i) {
                col << chainIds[i], chainIds[i + 1], id, id;
                emit(f, i == 0, col);
            }
            continue;
        }

        std::vector<uint32_t> polygon;
        for (int j = 0; j < R; ++j) {
            polygon.push_back(F(j, f));
            for (auto &ins : item.second)
                if (ins.first == j)
                    polygon.insert(polygon.end(), ins.second.begin(), ins.second.end());
        }
        if (R == 4) {
            /* Quad -> polygon, stored as its directed edges; its id is one
               of its new corners that no other polygon uses as id (else an
               unused vertex is added for it) */
            uint32_t id = NONE;
            for (auto &ins : item.second)
                for (uint32_t v : ins.second)
                    if (id == NONE && usedIds.insert(v).second)
                        id = v;
            if (id == NONE) {
                id = (uint32_t) O.cols();
                O.conservativeResize(3, id + 1);
                O.col(id) = O.col(polygon[0]);
            }
            for (size_t i = 0; i < polygon.size(); ++i) {
                col << polygon[i], polygon[(i + 1) % polygon.size()], id, id;
                emit(f, i == 0, col);
            }
        } else {
            std::vector<Vector3f> positions;
            for (uint32_t v : polygon)
                positions.push_back(O.col(v));
            std::vector<uint32_t> tris;
            triangulate_polygon(positions, tris);
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                col << polygon[tris[i]], polygon[tris[i + 1]], polygon[tris[i + 2]];
                emit(f, i == 0, col);
            }
        }
    }

    if (!newCols.empty()) {
        const uint32_t base = (uint32_t) F.cols();
        F.conservativeResize(R, base + newCols.size());
        for (size_t i = 0; i < newCols.size(); ++i)
            F.col(base + i) = newCols[i];
        if (faceNormals) {
            Nf.conservativeResize(3, base + newCols.size());
            for (size_t i = 0; i < newCols.size(); ++i)
                Nf.col(base + i) = Nf.col(newColSource[i]);
        }
    }
    return stats;
}
