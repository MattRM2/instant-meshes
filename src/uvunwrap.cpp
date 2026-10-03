/*
    uvunwrap.cpp -- --uv unwrap with xatlas (see uvunwrap.h)

    xatlas grows its charts triangle by triangle: a polygon can end up with
    its triangles in two charts. Polygons are kept whole here: one pass
    unwraps the triangles; a split polygon then takes the chart that holds
    most of its area, its other corners extended by the affine map of its
    known ones; if any polygon was split, a second pass re-packs the charts
    (xatlas UV mesh packing) so that the extended corners overlap nothing.
*/

#include "uvunwrap.h"
#include "xatlas.h"
#include <limits>
#include <map>
#include <tuple>

namespace {

int quiet(const char *, ...) { return 0; }

struct AtlasGuard {
    xatlas::Atlas *atlas;
    AtlasGuard() : atlas(xatlas::Create()) { xatlas::SetPrint(quiet, false); }
    ~AtlasGuard() { xatlas::Destroy(atlas); }
};

xatlas::PackOptions pack_options() {
    /* One atlas, its resolution estimated around 1024 (a fixed resolution
       could spill into several atlases, overlapping once normalized) */
    xatlas::PackOptions pack;
    pack.padding = 2;
    /* its 90 degree turns swap u and v: they would mirror charts */
    pack.rotateCharts = false;
    return pack;
}

/* Fits the 2D affine map plane coordinates -> UV of the 'known' corners
   (least squares) and applies it to the others */
void extend_corners(const std::vector<Vector3f> &P, std::vector<Vector2f> &uv, const std::vector<bool> &known) {
    const size_t n = P.size();
    Vector3f normal = Vector3f::Zero(), centre = Vector3f::Zero();
    for (size_t i = 0; i < n; ++i) {
        normal += P[i].cross(P[(i + 1) % n]);
        centre += P[i];
    }
    centre /= (Float) n;
    if (normal.squaredNorm() == 0)
        return;
    normal.normalize();
    const Vector3f t = (std::abs(normal.x()) < 0.9f ? Vector3f::UnitX() : Vector3f::UnitY()).cross(normal).normalized();
    const Vector3f b = normal.cross(t);
    /* uv = A^T (x, y, 1) */
    Eigen::Matrix3d AtA = Eigen::Matrix3d::Zero();
    Eigen::Matrix<double, 3, 2> Atb = Eigen::Matrix<double, 3, 2>::Zero();
    for (size_t i = 0; i < n; ++i) {
        if (!known[i])
            continue;
        const Eigen::Vector3d x((P[i] - centre).dot(t), (P[i] - centre).dot(b), 1.0);
        AtA += x * x.transpose();
        Atb += x * Eigen::Vector2d(uv[i].x(), uv[i].y()).transpose();
    }
    Eigen::LDLT<Eigen::Matrix3d> solver(AtA);
    if (solver.info() != Eigen::Success)
        return;
    const Eigen::Matrix<double, 3, 2> A = solver.solve(Atb);
    if (!A.allFinite())
        return;
    for (size_t i = 0; i < n; ++i) {
        if (known[i])
            continue;
        const Eigen::Vector3d x((P[i] - centre).dot(t), (P[i] - centre).dot(b), 1.0);
        const Eigen::Vector2d u = A.transpose() * x;
        uv[i] = Vector2f((float) u.x(), (float) u.y());
    }
}

} // namespace

CornerUVs unwrap_uvs(const MatrixXu &F, const MatrixXf &O, const std::string &name, UnwrapStats *stats) {
    static_assert(sizeof(MatrixXf::Scalar) == sizeof(float), "xatlas takes float positions");
    std::vector<uint32_t> sizes, indices, faceIds, cornerIds;
    extracted_polygons(F, sizes, indices, faceIds, &cornerIds);

    CornerUVs result;
    result.name = name;
    result.corners.setZero(2, F.rows() * F.cols());
    UnwrapStats local;
    if (sizes.empty()) {
        if (stats)
            *stats = local;
        return result;
    }

    /* Triangle fans: the polygon corner of every triangle corner */
    std::vector<size_t> offsets(sizes.size() + 1, 0), firstTri(sizes.size() + 1, 0);
    std::vector<uint32_t> triCorner;
    for (size_t k = 0; k < sizes.size(); ++k) {
        offsets[k + 1] = offsets[k] + sizes[k];
        firstTri[k + 1] = firstTri[k] + (sizes[k] - 2);
        for (uint32_t i = 1; i + 1 < sizes[k]; ++i) {
            triCorner.push_back((uint32_t) offsets[k]);
            triCorner.push_back((uint32_t) (offsets[k] + i));
            triCorner.push_back((uint32_t) (offsets[k] + i + 1));
        }
    }
    std::vector<uint32_t> triVertex(triCorner.size());
    for (size_t i = 0; i < triCorner.size(); ++i)
        triVertex[i] = indices[triCorner[i]];

    /* Pass 1: charts and their parameterization, on the triangles */
    std::vector<Vector2f> cornerUV(indices.size(), Vector2f::Zero());
    std::vector<int32_t> polygonChart(sizes.size(), -1);
    uint32_t width = 0, height = 0;
    size_t split = 0;
    {
        AtlasGuard guard;
        xatlas::MeshDecl decl;
        decl.vertexPositionData = O.data();
        decl.vertexPositionStride = sizeof(float) * 3;
        decl.vertexCount = (uint32_t) O.cols();
        decl.indexData = triVertex.data();
        decl.indexCount = (uint32_t) triVertex.size();
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        const xatlas::AddMeshError error = xatlas::AddMesh(guard.atlas, decl);
        if (error != xatlas::AddMeshError::Success)
            throw std::runtime_error(std::string("UV unwrap: ") + xatlas::StringForEnum(error));
        /* maxCost 1 (default 2): measured 1.6x faster on 30-60k faces, with
           as many charts and an atlas as full */
        xatlas::ChartOptions charts;
        charts.maxCost = 1.0f;
        xatlas::ComputeCharts(guard.atlas, charts);
        xatlas::PackCharts(guard.atlas, pack_options());
        const xatlas::Atlas &atlas = *guard.atlas;
        if (atlas.meshCount != 1 || atlas.meshes[0].indexCount != triVertex.size() || atlas.width == 0)
            throw std::runtime_error("UV unwrap: xatlas returned an unexpected mesh!");
        const xatlas::Mesh &mesh = atlas.meshes[0];
        for (uint32_t i = 0; i < mesh.indexCount; ++i)
            if (mesh.indexArray[i] >= mesh.vertexCount || mesh.vertexArray[mesh.indexArray[i]].xref != triVertex[i])
                throw std::runtime_error("UV unwrap: xatlas returned an unexpected mesh!");
        width = atlas.width;
        height = atlas.height;
        local.charts = atlas.chartCount;

        std::vector<Vector2f> uv;
        std::vector<bool> known;
        std::vector<Vector3f> P;
        for (size_t k = 0; k < sizes.size(); ++k) {
            /* The chart holding the largest part of the polygon's area */
            std::map<int32_t, Float> area;
            for (size_t t = firstTri[k]; t < firstTri[k + 1]; ++t) {
                const Vector3f p0 = O.col(triVertex[3 * t]), p1 = O.col(triVertex[3 * t + 1]),
                               p2 = O.col(triVertex[3 * t + 2]);
                area[mesh.vertexArray[mesh.indexArray[3 * t]].chartIndex] += (p1 - p0).cross(p2 - p0).norm();
            }
            int32_t chart = area.begin()->first;
            for (const auto &kv : area)
                if (kv.second > area[chart])
                    chart = kv.first;

            const size_t n = sizes[k];
            uv.assign(n, Vector2f::Zero());
            known.assign(n, false);
            for (size_t t = firstTri[k]; t < firstTri[k + 1]; ++t) {
                for (int j = 0; j < 3; ++j) {
                    const xatlas::Vertex &v = mesh.vertexArray[mesh.indexArray[3 * t + j]];
                    const size_t c = triCorner[3 * t + j] - offsets[k];
                    if (v.chartIndex == chart && !known[c]) {
                        uv[c] = Vector2f(v.uv[0], v.uv[1]);
                        known[c] = true;
                    }
                }
            }
            if (area.size() > 1) {
                P.resize(n);
                for (size_t i = 0; i < n; ++i)
                    P[i] = O.col(indices[offsets[k] + i]);
                extend_corners(P, uv, known);
                ++split;
            }
            for (size_t i = 0; i < n; ++i)
                cornerUV[offsets[k] + i] = uv[i];
            polygonChart[k] = chart;
        }
    }

    /* Mirrored charts (xatlas accepts them, normal maps do not): the faces
       are consistently oriented, so a chart whose signed UV area is
       negative is flipped horizontally (before the packing of pass 2) */
    {
        struct ChartInfo {
            double area = 0;
            float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
        };
        std::map<int32_t, ChartInfo> charts;
        for (size_t k = 0; k < sizes.size(); ++k) {
            ChartInfo &ci = charts[polygonChart[k]];
            for (uint32_t i = 0; i < sizes[k]; ++i) {
                const Vector2f p = cornerUV[offsets[k] + i], q = cornerUV[offsets[k] + (i + 1) % sizes[k]];
                ci.area += (double) p.x() * q.y() - (double) q.x() * p.y();
                ci.lo = std::min(ci.lo, p.x());
                ci.hi = std::max(ci.hi, p.x());
            }
        }
        for (size_t k = 0; k < sizes.size(); ++k) {
            const ChartInfo &ci = charts[polygonChart[k]];
            if (polygonChart[k] < 0 || ci.area >= 0)
                continue;
            for (uint32_t i = 0; i < sizes[k]; ++i)
                cornerUV[offsets[k] + i].x() = ci.lo + ci.hi - cornerUV[offsets[k] + i].x();
        }
        for (const auto &kv : charts)
            local.mirrored += kv.first >= 0 && kv.second.area < 0;
    }

    /* Pass 2 (if a polygon was split or a chart mirrored): re-pack the charts with the
       polygons whole (xatlas finds the charts of a UV mesh from its
       connectivity, then packs them). UV vertices: one per (vertex, uv) */
    if (split > 0 || local.mirrored > 0) {
        std::map<std::tuple<uint32_t, float, float>, uint32_t> ids;
        std::vector<float> uvValues;
        std::vector<uint32_t> cornerVertex(indices.size());
        for (size_t c = 0; c < indices.size(); ++c) {
            auto key = std::make_tuple(indices[c], cornerUV[c].x(), cornerUV[c].y());
            auto it = ids.find(key);
            if (it == ids.end()) {
                it = ids.insert(std::make_pair(key, (uint32_t) (uvValues.size() / 2))).first;
                uvValues.push_back(cornerUV[c].x());
                uvValues.push_back(cornerUV[c].y());
            }
            cornerVertex[c] = it->second;
        }
        std::vector<uint32_t> uvIndex(triCorner.size());
        for (size_t i = 0; i < triCorner.size(); ++i)
            uvIndex[i] = cornerVertex[triCorner[i]];

        AtlasGuard guard;
        xatlas::UvMeshDecl decl;
        decl.vertexUvData = uvValues.data();
        decl.vertexStride = sizeof(float) * 2;
        decl.vertexCount = (uint32_t) (uvValues.size() / 2);
        decl.indexData = uvIndex.data();
        decl.indexCount = (uint32_t) uvIndex.size();
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        const xatlas::AddMeshError error = xatlas::AddUvMesh(guard.atlas, decl);
        if (error != xatlas::AddMeshError::Success)
            throw std::runtime_error(std::string("UV unwrap: ") + xatlas::StringForEnum(error));
        xatlas::Generate(guard.atlas, xatlas::ChartOptions(), pack_options());
        const xatlas::Atlas &atlas = *guard.atlas;
        if (atlas.meshCount != 1 || atlas.meshes[0].vertexCount != decl.vertexCount || atlas.width == 0)
            throw std::runtime_error("UV unwrap: xatlas returned an unexpected UV mesh!");
        const xatlas::Mesh &mesh = atlas.meshes[0];
        for (size_t c = 0; c < indices.size(); ++c) {
            const xatlas::Vertex &v = mesh.vertexArray[cornerVertex[c]];
            cornerUV[c] = Vector2f(v.uv[0], v.uv[1]);
        }
        for (size_t k = 0; k < sizes.size(); ++k)
            polygonChart[k] = mesh.vertexArray[cornerVertex[offsets[k]]].chartIndex;
        width = atlas.width;
        height = atlas.height;
        local.charts = atlas.chartCount;
    }

    /* Square normalization: [0, 1] on the larger side, charts keep their shape */
    const float scale = 1.0f / (float) std::max(width, height);
    for (size_t c = 0; c < indices.size(); ++c)
        result.corners.col(cornerIds[c]) = cornerUV[c] * scale;
    local.resolution = std::max(width, height);
    local.split = (uint32_t) split;

    /* Share of the [0, 1] square covered by the faces (UV area) */
    double total = 0;
    for (size_t k = 0; k < sizes.size(); ++k) {
        double a = 0;
        for (uint32_t i = 0; i < sizes[k]; ++i) {
            const Vector2f p = cornerUV[offsets[k] + i] * scale;
            const Vector2f q = cornerUV[offsets[k] + (i + 1) % sizes[k]] * scale;
            a += (double) p.x() * q.y() - (double) q.x() * p.y();
        }
        total += std::abs(a) / 2;
    }
    local.utilization = (float) total;
    if (stats)
        *stats = local;
    return result;
}
