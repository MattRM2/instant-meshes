/*
    uvtransfer.cpp -- --uv transfer (see uvtransfer.h)
*/

#include "uvtransfer.h"
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <atomic>
#include <algorithm>
#include <limits>

namespace {

const uint32_t NONE = (uint32_t) -1;

/* Nearest point of triangle abc to p (Ericson, Real-Time Collision
   Detection 5.1.5): barycentric weights, and where it lies: inside, on a
   vertex (k) or on an edge (k: from corner k to corner k + 1) */
struct Closest {
    enum Region { Inside, Vertex, Edge };
    Region region = Inside;
    int k = 0;
    Float b[3] = { 1, 0, 0 };
};

void closest_point(const Vector3f &p, const Vector3f &a, const Vector3f &b, const Vector3f &c,
                   Closest &out, Vector3f &point) {
    const Vector3f ab = b - a, ac = c - a, ap = p - a;
    const Float d1 = ab.dot(ap), d2 = ac.dot(ap);
    auto set = [&](Closest::Region region, int k, Float w0, Float w1, Float w2) {
        out.region = region;
        out.k = k;
        out.b[0] = w0;
        out.b[1] = w1;
        out.b[2] = w2;
        point = a * w0 + b * w1 + c * w2;
    };
    if (d1 <= 0 && d2 <= 0)
        return set(Closest::Vertex, 0, 1, 0, 0);
    const Vector3f bp = p - b;
    const Float d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3)
        return set(Closest::Vertex, 1, 0, 1, 0);
    const Float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const Float v = d1 / (d1 - d3);
        return set(Closest::Edge, 0, 1 - v, v, 0);
    }
    const Vector3f cp = p - c;
    const Float d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6)
        return set(Closest::Vertex, 2, 0, 0, 1);
    const Float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const Float w = d2 / (d2 - d6);
        return set(Closest::Edge, 2, 1 - w, 0, w);
    }
    const Float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const Float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return set(Closest::Edge, 1, 0, 1 - w, w);
    }
    const Float denom = 1 / (va + vb + vc);
    const Float v = vb * denom, w = vc * denom;
    set(Closest::Inside, 0, 1 - v - w, v, w);
}

/* Barycentric weights of p projected onto the plane of abc, unclamped
   (outside the triangle: a linear extension); false if degenerate */
bool plane_barycentric(const Vector3f &p, const Vector3f &a, const Vector3f &b, const Vector3f &c, Float w[3]) {
    const Vector3f v0 = b - a, v1 = c - a, v2 = p - a;
    const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1);
    const double d20 = v2.dot(v0), d21 = v2.dot(v1);
    const double denom = d00 * d11 - d01 * d01;
    if (!(std::abs(denom) > 1e-30))
        return false;
    const double v = (d11 * d20 - d01 * d21) / denom, x = (d00 * d21 - d01 * d20) / denom;
    w[0] = (Float) (1 - v - x);
    w[1] = (Float) v;
    w[2] = (Float) x;
    return std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]);
}

} // namespace

struct UVTransfer::Impl {
    MatrixXu F;
    MatrixXf V, N;                    /* N: unit normal of each triangle (0 if degenerate) */
    std::vector<UVSet> uvs;
    std::vector<std::vector<uint32_t>> island;   /* per set, per triangle */
    std::vector<std::vector<uint8_t>> border;    /* per set, per triangle: bit k = edge k is an island border */
    std::vector<std::vector<uint32_t>> next;     /* per set, 3 per triangle: the same-island neighbour across edge k */

    /* Bounding volume hierarchy over the triangles */
    struct Node {
        Vector3f lo, hi;
        uint32_t start = 0, count = 0;   /* leaf: triangles order[start .. start + count) */
        uint32_t right = 0;              /* inner node: right child (the left one follows) */
    };
    std::vector<Node> nodes;
    std::vector<uint32_t> order;

    Vector3f corner(uint32_t t, int k) const { return V.col(F(k, t)); }

    Vector2f uv(size_t s, uint32_t t, int k) const {
        return uvs[s].values.col(uvs[s].corners[3 * (size_t) t + k]);
    }

    void build(uint32_t node, uint32_t begin, uint32_t end, const MatrixXf &centers) {
        Node &n = nodes[node];
        n.lo.setConstant(std::numeric_limits<Float>::infinity());
        n.hi.setConstant(-std::numeric_limits<Float>::infinity());
        Vector3f clo = n.lo, chi = n.hi;
        for (uint32_t i = begin; i < end; ++i) {
            const uint32_t t = order[i];
            for (int k = 0; k < 3; ++k) {
                n.lo = n.lo.cwiseMin(corner(t, k));
                n.hi = n.hi.cwiseMax(corner(t, k));
            }
            clo = clo.cwiseMin(centers.col(t));
            chi = chi.cwiseMax(centers.col(t));
        }
        if (end - begin <= 4) {
            n.start = begin;
            n.count = end - begin;
            return;
        }
        int axis;
        (chi - clo).maxCoeff(&axis);
        const uint32_t mid = (begin + end) / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                         [&](uint32_t x, uint32_t y) { return centers(axis, x) < centers(axis, y); });
        const uint32_t left = (uint32_t) nodes.size();
        nodes.emplace_back();
        build(left, begin, mid, centers);
        const uint32_t right = (uint32_t) nodes.size();
        nodes.emplace_back();
        nodes[node].right = right;     /* 'n' may have moved */
        build(right, mid, end, centers);
    }

    static Float box_distance2(const Node &n, const Vector3f &p) {
        const Vector3f d = (n.lo - p).cwiseMax(p - n.hi).cwiseMax(Vector3f::Zero());
        return d.squaredNorm();
    }

    /* Nearest triangle accepted by 'accept' (false if none) */
    template <typename Accept>
    bool nearest(const Vector3f &p, const Accept &accept, uint32_t &best, Closest &hit) const {
        if (nodes.empty())
            return false;
        Float bestD2 = std::numeric_limits<Float>::infinity();
        bool found = false;
        uint32_t stack[128];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node &n = nodes[stack[--top]];
            if (box_distance2(n, p) >= bestD2)
                continue;
            if (n.count > 0) {
                for (uint32_t i = n.start; i < n.start + n.count; ++i) {
                    const uint32_t t = order[i];
                    if (!accept(t))
                        continue;
                    Closest c;
                    Vector3f q;
                    closest_point(p, corner(t, 0), corner(t, 1), corner(t, 2), c, q);
                    const Float d2 = (q - p).squaredNorm();
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        best = t;
                        hit = c;
                        found = true;
                    }
                }
                continue;
            }
            const uint32_t left = (uint32_t) (&n - nodes.data()) + 1, right = n.right;
            const Float dl = box_distance2(nodes[left], p), dr = box_distance2(nodes[right], p);
            if (top + 2 > 128)
                throw std::runtime_error("UV transfer: hierarchy too deep!");
            if (dl < dr) {
                stack[top++] = right;
                stack[top++] = left;
            } else {
                stack[top++] = left;
                stack[top++] = right;
            }
        }
        return found;
    }

    /* UV islands and their borders, per set: triangles sharing an edge are
       in the same island when their UVs agree at both of its vertices */
    void islands() {
        const uint32_t T = (uint32_t) F.cols();
        struct EdgeRef {
            uint64_t key;
            uint32_t t;
            int k;
        };
        std::vector<EdgeRef> edges;
        edges.reserve(3 * (size_t) T);
        for (uint32_t t = 0; t < T; ++t)
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = F(k, t), b = F((k + 1) % 3, t);
                if (a != b)
                    edges.push_back({ ((uint64_t) std::min(a, b) << 32) | std::max(a, b), t, k });
            }
        std::sort(edges.begin(), edges.end(), [](const EdgeRef &x, const EdgeRef &y) {
            return x.key < y.key || (x.key == y.key && (x.t < y.t || (x.t == y.t && x.k < y.k)));
        });

        island.assign(uvs.size(), std::vector<uint32_t>());
        border.assign(uvs.size(), std::vector<uint8_t>(T, 7));
        next.assign(uvs.size(), std::vector<uint32_t>(3 * (size_t) T, NONE));
        for (size_t s = 0; s < uvs.size(); ++s) {
            std::vector<uint32_t> parent(T);
            for (uint32_t t = 0; t < T; ++t)
                parent[t] = t;
            auto find = [&](uint32_t x) {
                while (parent[x] != x)
                    x = parent[x] = parent[parent[x]];
                return x;
            };
            /* UV of vertex 'v' in triangle t */
            auto uv_at = [&](uint32_t t, uint32_t v) {
                for (int k = 0; k < 3; ++k)
                    if (F(k, t) == v)
                        return uv(s, t, k);
                return Vector2f(Vector2f::Constant(std::numeric_limits<Float>::quiet_NaN()));
            };
            for (size_t i = 0; i < edges.size();) {
                size_t j = i;
                while (j < edges.size() && edges[j].key == edges[i].key)
                    ++j;
                for (size_t x = i; x < j; ++x)
                    for (size_t y = x + 1; y < j; ++y) {
                        const uint32_t tx = edges[x].t, ty = edges[y].t;
                        const uint32_t a = (uint32_t) (edges[x].key >> 32), b = (uint32_t) edges[x].key;
                        if (uv_at(tx, a) == uv_at(ty, a) && uv_at(tx, b) == uv_at(ty, b)) {
                            parent[find(tx)] = find(ty);
                            border[s][tx] &= (uint8_t) ~(1 << edges[x].k);
                            border[s][ty] &= (uint8_t) ~(1 << edges[y].k);
                            next[s][3 * (size_t) tx + edges[x].k] = ty;
                            next[s][3 * (size_t) ty + edges[y].k] = tx;
                        }
                    }
                i = j;
            }
            island[s].resize(T);
            for (uint32_t t = 0; t < T; ++t)
                island[s][t] = find(t);
        }
    }

    /* Squared distance from p to triangle t */
    Float distance2(uint32_t t, const Vector3f &p) const {
        Closest c;
        Vector3f q;
        closest_point(p, corner(t, 0), corner(t, 1), corner(t, 2), c, q);
        return (q - p).squaredNorm();
    }

    /* The part of the island of 'source' (set s) around a face: triangles
       reached from 'source' through same-island edges, within 'radius' of
       'centre'. Corners are only projected there: an island that wraps
       around (a cylinder strip) is not reached again from its other end */
    void local_patch(size_t s, uint32_t source, const Vector3f &centre, Float radius,
                     std::vector<uint32_t> &stamp, uint32_t &generation, std::vector<uint32_t> &patch) const {
        if (++generation == 0) {
            std::fill(stamp.begin(), stamp.end(), 0);
            generation = 1;
        }
        const Float r2 = radius * radius;
        patch.clear();
        patch.push_back(source);
        stamp[source] = generation;
        for (size_t i = 0; i < patch.size() && patch.size() < 200000; ++i) {
            const uint32_t t = patch[i];
            for (int k = 0; k < 3; ++k) {
                const uint32_t n = next[s][3 * (size_t) t + k];
                if (n == NONE || stamp[n] == generation)
                    continue;
                stamp[n] = generation;
                if (distance2(n, centre) <= r2)
                    patch.push_back(n);
            }
        }
    }

    /* Nearest triangle of 'patch' (non-degenerate, facing the normal if possible) */
    bool nearest_in(const std::vector<uint32_t> &patch, const Vector3f &p, const Vector3f &normal,
                    uint32_t &best, Closest &hit) const {
        bool found = false;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            Float bestD2 = std::numeric_limits<Float>::infinity();
            for (uint32_t t : patch) {
                const Vector3f n = N.col(t);
                if (n.squaredNorm() == 0 || (pass == 0 && n.dot(normal) <= 0))
                    continue;
                Closest c;
                Vector3f q;
                closest_point(p, corner(t, 0), corner(t, 1), corner(t, 2), c, q);
                const Float d2 = (q - p).squaredNorm();
                if (d2 < bestD2) {
                    bestD2 = d2;
                    best = t;
                    hit = c;
                    found = true;
                }
            }
        }
        return found;
    }

    /* UV of p on triangle t of set s, given its nearest point; extended
       linearly past an island border */
    Vector2f lookup(size_t s, uint32_t t, const Closest &c, const Vector3f &p, bool &extended) const {
        Float w[3] = { c.b[0], c.b[1], c.b[2] };
        const uint8_t edges = border[s][t];
        bool outward = false;
        if (c.region == Closest::Edge)
            outward = (edges >> c.k) & 1;
        else if (c.region == Closest::Vertex)
            outward = ((edges >> c.k) & 1) || ((edges >> ((c.k + 2) % 3)) & 1);
        extended = false;
        if (outward) {
            Float e[3];
            if (plane_barycentric(p, corner(t, 0), corner(t, 1), corner(t, 2), e) &&
                std::max(std::abs(e[0]), std::max(std::abs(e[1]), std::abs(e[2]))) < 4) {
                w[0] = e[0];
                w[1] = e[1];
                w[2] = e[2];
                extended = true;
            }
        }
        return uv(s, t, 0) * w[0] + uv(s, t, 1) * w[1] + uv(s, t, 2) * w[2];
    }
};

UVTransfer::UVTransfer(const MatrixXu &F, const MatrixXf &V, const std::vector<UVSet> &uvs) : d(new Impl()) {
    if (F.rows() != 3)
        throw std::runtime_error("UV transfer: the original must be a triangle mesh!");
    for (const UVSet &s : uvs)
        if (s.corners.size() != 3 * (size_t) F.cols())
            throw std::runtime_error("UV transfer: UV set \"" + s.name + "\" does not match the triangles!");
    d->F = F;
    d->V = V;
    d->uvs = uvs;
    const uint32_t T = (uint32_t) F.cols();

    d->N.resize(3, T);
    MatrixXf centers(3, T);
    for (uint32_t t = 0; t < T; ++t) {
        const Vector3f a = d->corner(t, 0), b = d->corner(t, 1), c = d->corner(t, 2);
        const Vector3f n = (b - a).cross(c - a);
        const Float len = n.norm();
        d->N.col(t) = len > 0 && std::isfinite(len) ? Vector3f(n / len) : Vector3f(Vector3f::Zero());
        centers.col(t) = (a + b + c) / 3;
        if (len > 0 && std::isfinite(len))
            d->order.push_back(t);    /* degenerate triangles are never a target */
    }
    if (!d->order.empty()) {
        d->nodes.reserve(2 * d->order.size() + 2);
        d->nodes.emplace_back();
        d->build(0, 0, (uint32_t) d->order.size(), centers);
    }
    d->islands();
}

UVTransfer::~UVTransfer() { }

std::vector<CornerUVs> UVTransfer::transfer(const MatrixXu &F, const MatrixXf &O, Stats *stats) const {
    std::vector<uint32_t> sizes, indices, faceIds, cornerIds;
    extracted_polygons(F, sizes, indices, faceIds, &cornerIds);
    std::vector<size_t> offsets(sizes.size() + 1, 0);
    for (size_t k = 0; k < sizes.size(); ++k)
        offsets[k + 1] = offsets[k] + sizes[k];

    std::vector<CornerUVs> result(d->uvs.size());
    for (size_t s = 0; s < result.size(); ++s) {
        result[s].name = d->uvs[s].name;
        result[s].corners.setZero(2, F.rows() * F.cols());
    }
    std::atomic<uint64_t> extended(0), flipped(0);
    if (d->nodes.empty() || d->uvs.empty())
        return result;

    struct Scratch {
        std::vector<uint32_t> stamp, patch;
        uint32_t generation = 0;
    };
    tbb::enumerable_thread_specific<Scratch> scratch;
    const size_t T = (size_t) d->F.cols();

    tbb::parallel_for(tbb::blocked_range<size_t>(0, sizes.size(), 256),
        [&](const tbb::blocked_range<size_t> &range) {
            Scratch &local = scratch.local();
            if (local.stamp.size() != T)
                local.stamp.assign(T, 0);
            std::vector<Vector3f> P;
            for (size_t k = range.begin(); k != range.end(); ++k) {
                const size_t n = sizes[k], o = offsets[k];
                P.resize(n);
                Vector3f centroid = Vector3f::Zero(), normal = Vector3f::Zero();
                for (size_t i = 0; i < n; ++i) {
                    P[i] = O.col(indices[o + i]);
                    centroid += P[i];
                }
                centroid /= (Float) n;
                for (size_t i = 0; i < n; ++i)   /* Newell */
                    normal += P[i].cross(P[(i + 1) % n]);

                /* The source triangle under the centre, facing the same way if possible */
                auto facing = [&](uint32_t t) { return d->N.col(t).dot(normal) > 0; };
                auto any = [](uint32_t) { return true; };
                uint32_t source = 0;
                Closest hit;
                if (!d->nearest(centroid, facing, source, hit)) {
                    d->nearest(centroid, any, source, hit);
                    ++flipped;
                }

                /* Every corner on the source triangle's island, around the face */
                Float radius = 0;
                for (size_t i = 0; i < n; ++i)
                    radius = std::max(radius, (P[i] - centroid).norm());
                radius = 2 * radius + std::sqrt(d->distance2(source, centroid)) + 1e-6f;
                for (size_t s = 0; s < d->uvs.size(); ++s) {
                    d->local_patch(s, source, centroid, radius, local.stamp, local.generation, local.patch);
                    for (size_t i = 0; i < n; ++i) {
                        uint32_t t = source;
                        Closest c;
                        d->nearest_in(local.patch, P[i], normal, t, c);
                        bool ext;
                        result[s].corners.col(cornerIds[o + i]) = d->lookup(s, t, c, P[i], ext);
                        if (ext && s == 0)
                            ++extended;
                    }
                }
            }
        });

    if (stats) {
        stats->corners = offsets.back();
        stats->extended = extended;
        stats->flipped = flipped;
    }
    return result;
}
