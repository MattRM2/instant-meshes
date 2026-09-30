/*
    test_border.cpp -- --keep-border: border extraction and snapping
*/

#include "test_common.h"
#include "border.h"
#include "meshio.h"

/* Square [0,2]x[0,2] as a 2x2 grid of quads split into triangles */
static void square(MatrixXu &F, MatrixXf &V) {
    V.resize(3, 9);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            V.col(y * 3 + x) = Vector3f((Float) x, (Float) y, 0);
    F.resize(3, 8);
    int f = 0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const uint32_t a = y * 3 + x, b = a + 1, c = a + 4, d = a + 3;
            F.col(f++) = Vector3u(a, b, c);
            F.col(f++) = Vector3u(a, c, d);
        }
}

/* Area of the polygons of an extracted mesh (projected on z = 0) */
static Float area(const MatrixXu &F, const MatrixXf &O) {
    std::vector<uint32_t> sizes, indices, faceIds;
    extracted_polygons(F, sizes, indices, faceIds);
    Float total = 0;
    size_t offset = 0;
    for (uint32_t n : sizes) {
        for (uint32_t i = 0; i < n; ++i) {
            const Vector3f p = O.col(indices[offset + i]), q = O.col(indices[offset + (i + 1) % n]);
            total += (p.x() * q.y() - q.x() * p.y()) / 2;
        }
        offset += n;
    }
    return total;
}

static bool on_square_border(const Vector3f &p) {
    const bool inRange = p.x() >= 0 && p.x() <= 2 && p.y() >= 0 && p.y() <= 2;
    return inRange && (p.x() == 0 || p.x() == 2 || p.y() == 0 || p.y() == 2);
}

void test_border() {
    std::cout << "border (--keep-border)" << std::endl;
    MatrixXu F;
    MatrixXf V;
    square(F, V);

    const BorderCurves border = extract_border(F, V);
    CHECK(border.chains.size() == 1);
    if (border.chains.size() != 1)
        return;
    const BorderCurves::Chain &c = border.chains[0];
    int corners = 0;
    for (bool b : c.corner)
        corners += b;
    CHECK(c.closed && c.points.size() == 8 && std::abs(c.length - 8) < 1e-5f && corners == 4);

    /* A diamond near the side midpoints: each of its edges cuts a corner */
    MatrixXf O(3, 4);
    O.col(0) = Vector3f(1, 0.02f, 0);
    O.col(1) = Vector3f(2.01f, 1, 0);
    O.col(2) = Vector3f(1, 1.98f, 0);
    O.col(3) = Vector3f(0.03f, 1, 0);

    /* Quad output: one polygon with the 4 corners added, exactly the square */
    {
        MatrixXu Fq(4, 1);
        Fq.col(0) = Vector4u(0, 1, 2, 3);
        MatrixXf Oq = O, Nf(3, 1);
        Nf.col(0) = Vector3f(0, 0, 1);
        const BorderSnapStats s = snap_to_border(border, 0.1f, Fq, Oq, Nf);
        CHECK(s.borderVertices == 4 && s.snapped == 4 && s.tooFar == 0 && s.inserted == 4 && s.faces == 1);
        CHECK(Fq.cols() == 8 && Nf.cols() == 8 && Oq.cols() == 8);
        std::vector<uint32_t> sizes, indices, faceIds;
        CHECK(extracted_polygons(Fq, sizes, indices, faceIds) == 1 && sizes.size() == 1 && sizes[0] == 8);
        bool all = true;
        for (uint32_t i = 0; i < Oq.cols(); ++i)
            all = all && on_square_border(Oq.col(i));
        CHECK(all);
        CHECK(std::abs(area(Fq, Oq) - 4) < 1e-5f);
    }

    /* Triangle output: the polygons are triangulated, same area */
    {
        MatrixXu Ft(3, 2);
        Ft.col(0) = Vector3u(0, 1, 2);
        Ft.col(1) = Vector3u(0, 2, 3);
        MatrixXf Ot = O, Nf(3, 2);
        Nf.setZero();
        const BorderSnapStats s = snap_to_border(border, 0.1f, Ft, Ot, Nf);
        CHECK(s.snapped == 4 && s.inserted == 4 && s.faces == 2);
        CHECK(Ft.rows() == 3 && Ft.cols() == 6 && Nf.cols() == 6);
        CHECK(std::abs(area(Ft, Ot) - 4) < 1e-5f);
    }

    /* Out of reach: nothing moves */
    {
        MatrixXu Fq(4, 1);
        Fq.col(0) = Vector4u(0, 1, 2, 3);
        MatrixXf Oq = O, Nf;
        const BorderSnapStats s = snap_to_border(border, 0.015f, Fq, Oq, Nf);
        CHECK(s.tooFar == 3 && s.snapped == 1 && s.inserted == 0 && Fq.cols() == 1);
    }

    /* A closed mesh has no border */
    {
        MatrixXu Fc(3, 4);
        Fc.col(0) = Vector3u(0, 1, 2);
        Fc.col(1) = Vector3u(0, 3, 1);
        Fc.col(2) = Vector3u(1, 3, 2);
        Fc.col(3) = Vector3u(0, 2, 3);
        MatrixXf Vc(3, 4);
        Vc << 0, 1, 0, 0,
              0, 0, 1, 0,
              0, 0, 0, 1;
        CHECK(extract_border(Fc, Vc).empty());
    }
}
