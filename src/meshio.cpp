/*
    meshio.cpp: Mesh file input/output routines

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "meshio.h"
#include "abc.h"
#include "normal.h"
#include <unordered_map>
#include <fstream>
#if !defined(_WIN32)
#include <libgen.h>
#endif

extern "C" {
    #include "rply.h"
}

void load_mesh_or_pointcloud(const std::string &filename, MatrixXu &F, MatrixXf &V, MatrixXf &N,
              const ProgressCallback &progress, uint64_t *polygons) {
    if (polygons)
        *polygons = 0;
    std::string extension;
    if (filename.size() > 4)
        extension = str_tolower(filename.substr(filename.size()-4));

    if (extension == ".ply") {
        load_ply(filename, F, V, N, false, progress);
        if (polygons)
            *polygons = F.cols();   /* the PLY reader only accepts triangles */
    } else if (extension == ".obj")
        load_obj(filename, F, V, progress, polygons);
    else if (extension == ".abc")
        abc::load_abc(filename, F, V, "", progress, polygons);
    else if (extension == ".aln")
        load_pointcloud(filename, V, N, progress);
    else
        throw std::runtime_error("load_mesh_or_pointcloud: Unknown file extension \"" + extension + "\" (.ply/.obj/.abc/.aln are supported)");
}

void write_mesh(const std::string &filename, const MatrixXu &F,
                const MatrixXf &V, const MatrixXf &N, const MatrixXf &Nf,
                const MatrixXf &UV, const MatrixXf &C,
                const ProgressCallback &progress) {
    std::string extension;
    if (filename.size() > 4)
        extension = str_tolower(filename.substr(filename.size()-4));

    if (extension == ".ply")
        write_ply(filename, F, V, N, Nf, UV, C, progress);
    else if (extension == ".obj")
        write_obj(filename, F, V, N, Nf, UV, C, progress);
    else if (extension == ".abc")
        abc::write_abc(filename, F, V, progress);
    else
        throw std::runtime_error("write_mesh: Unknown file extension \"" + extension + "\" (.ply/.obj/.abc are supported)");
}

void load_ply(const std::string &filename, MatrixXu &F, MatrixXf &V,
              MatrixXf &N, bool pointcloud, const ProgressCallback &progress) {
    auto message_cb = [](p_ply ply, const char *msg) { cerr << "rply: " << msg << endl; };

    Timer<> timer;
    cout << "Loading \"" << filename << "\" .. ";
    cout.flush();

    p_ply ply = ply_open(filename.c_str(), message_cb, 0, nullptr);
    if (!ply)
        throw std::runtime_error("Unable to open PLY file \"" + filename + "\"!");

    if (!ply_read_header(ply)) {
        ply_close(ply);
        throw std::runtime_error("Unable to open PLY header of \"" + filename + "\"!");
    }

    p_ply_element element = nullptr;
    uint32_t vertexCount = 0, faceCount = 0;

    /* Inspect the structure of the PLY file, load number of faces if avaliable */
    while ((element = ply_get_next_element(ply, element)) != nullptr) {
        const char *name;
        long nInstances;

        ply_get_element_info(element, &name, &nInstances);
        if (!strcmp(name, "vertex"))
            vertexCount = (uint32_t) nInstances;
        else if (!strcmp(name, "face"))
            faceCount = (uint32_t) nInstances;
    }

    if (vertexCount == 0 && faceCount == 0)
        throw std::runtime_error("PLY file \"" + filename + "\" is invalid! No face/vertex/elements found!");
    else if (!pointcloud && faceCount == 0)
        throw std::runtime_error("PLY file \"" + filename + "\" is invalid! No faces found!");

    F.resize(3, faceCount);
    V.resize(3, vertexCount);

    struct VertexCallbackData {
        MatrixXf &V;
        const ProgressCallback &progress;
        VertexCallbackData(MatrixXf &V, const ProgressCallback &progress)
            : V(V), progress(progress) { }
    };

    struct FaceCallbackData {
        MatrixXu &F;
        const ProgressCallback &progress;
        FaceCallbackData(MatrixXu &F, const ProgressCallback &progress)
            : F(F), progress(progress) { }
    };

    struct VertexNormalCallbackData {
        MatrixXf &N;
        const ProgressCallback &progress;
        VertexNormalCallbackData(MatrixXf &_N, const ProgressCallback &progress)
            : N(_N), progress(progress) { }
    };

    auto rply_vertex_cb = [](p_ply_argument argument) -> int {
        VertexCallbackData *data; long index, coord;
        ply_get_argument_user_data(argument, (void **) &data, &coord);
        ply_get_argument_element(argument, nullptr, &index);
        data->V(coord, index) = (Float) ply_get_argument_value(argument);
        if (data->progress && coord == 0 && index % 500000 == 0)
            data->progress("Loading vertex data", index / (Float) data->V.cols());
        return 1;
    };

    auto rply_vertex_normal_cb = [](p_ply_argument argument) -> int {
        VertexNormalCallbackData *data; long index, coord;
        ply_get_argument_user_data(argument, (void **) &data, &coord);
        ply_get_argument_element(argument, nullptr, &index);
        data->N(coord, index) = (Float) ply_get_argument_value(argument);
        if (data->progress && coord == 0 && index % 500000 == 0)
            data->progress("Loading vertex normal data", index / (Float)data->N.cols());
        return 1;
    };

    auto rply_index_cb = [](p_ply_argument argument) -> int {
        FaceCallbackData *data;
        long length, value_index, index;
        ply_get_argument_property(argument, nullptr, &length, &value_index);

        if (length != 3)
            throw std::runtime_error("Only triangle faces are supported!");

        ply_get_argument_user_data(argument, (void **) &data, nullptr);
        ply_get_argument_element(argument, nullptr, &index);

        if (value_index >= 0)
            data->F(value_index, index) = (uint32_t) ply_get_argument_value(argument);

        if (data->progress && value_index == 0 && index % 500000 == 0)
            data->progress("Loading face data", index / (Float) data->F.cols());

        return 1;
    };

    VertexCallbackData vcbData(V, progress);
    FaceCallbackData fcbData(F, progress);
    VertexNormalCallbackData vncbData(N, progress);

    if (!ply_set_read_cb(ply, "vertex", "x", rply_vertex_cb, &vcbData, 0) ||
        !ply_set_read_cb(ply, "vertex", "y", rply_vertex_cb, &vcbData, 1) ||
        !ply_set_read_cb(ply, "vertex", "z", rply_vertex_cb, &vcbData, 2)) {
        ply_close(ply);
        throw std::runtime_error("PLY file \"" + filename + "\" does not contain vertex position data!");
    }

    if (pointcloud && faceCount == 0) {
        N.resize(3, vertexCount);
        if (!ply_set_read_cb(ply, "vertex", "nx", rply_vertex_normal_cb, &vncbData, 0) ||
            !ply_set_read_cb(ply, "vertex", "ny", rply_vertex_normal_cb, &vncbData, 1) ||
            !ply_set_read_cb(ply, "vertex", "nz", rply_vertex_normal_cb, &vncbData, 2)) {
            ply_close(ply);
            throw std::runtime_error("PLY file \"" + filename + "\" does not contain vertex normal or face data!");
        }
    } else {
        if (!ply_set_read_cb(ply, "face", "vertex_indices", rply_index_cb, &fcbData, 0)) {
            ply_close(ply);
            throw std::runtime_error("PLY file \"" + filename + "\" does not contain vertex indices!");
        }
    }

    if (!ply_read(ply)) {
        ply_close(ply);
        throw std::runtime_error("Error while loading PLY data from \"" + filename + "\"!");
    }

    ply_close(ply);
    cout << "done. (V=" << vertexCount;
    if (faceCount > 0)
        cout << ", F=" << faceCount;
    cout << ", took " << timeString(timer.value()) << ")" << endl;
}

void write_ply(const std::string &filename, const MatrixXu &F,
               const MatrixXf &V, const MatrixXf &N, const MatrixXf &Nf, const MatrixXf &UV,
               const MatrixXf &C, const ProgressCallback &progress) {
    auto message_cb = [](p_ply ply, const char *msg) { cerr << "rply: " << msg << endl; };

    Timer<> timer;
    cout << "Writing \"" << filename << "\" (V=" << V.cols()
         << ", F=" << F.cols() << ") .. ";
    cout.flush();

    if (N.size() > 0 && Nf.size() > 0)
        throw std::runtime_error("Please specify either face or vertex normals but not both!");

    p_ply ply = ply_create(filename.c_str(), PLY_DEFAULT, message_cb, 0, nullptr);
    if (!ply)
        throw std::runtime_error("Unable to write PLY file!");

    ply_add_comment(ply, "Generated by Instant Meshes");
    ply_add_element(ply, "vertex", V.cols());
    ply_add_scalar_property(ply, "x", PLY_FLOAT);
    ply_add_scalar_property(ply, "y", PLY_FLOAT);
    ply_add_scalar_property(ply, "z", PLY_FLOAT);

    if (N.size() > 0) {
        ply_add_scalar_property(ply, "nx", PLY_FLOAT);
        ply_add_scalar_property(ply, "ny", PLY_FLOAT);
        ply_add_scalar_property(ply, "nz", PLY_FLOAT);
        if (N.cols() != V.cols() || N.rows() != 3)
            throw std::runtime_error("Vertex normal matrix has incorrect size");
    }

    if (UV.size() > 0) {
        ply_add_scalar_property(ply, "u", PLY_FLOAT);
        ply_add_scalar_property(ply, "v", PLY_FLOAT);
        if (UV.cols() != V.cols() || UV.rows() != 2)
            throw std::runtime_error("Texture coordinate matrix has incorrect size");
    }

    if (C.size() > 0) {
        ply_add_scalar_property(ply, "red", PLY_FLOAT);
        ply_add_scalar_property(ply, "green", PLY_FLOAT);
        ply_add_scalar_property(ply, "blue", PLY_FLOAT);
        if (C.cols() != V.cols() || (C.rows() != 3 && C.rows() != 4))
            throw std::runtime_error("Color matrix has incorrect size");
    }

    /* Check for irregular faces */
    std::map<uint32_t, std::pair<uint32_t, std::map<uint32_t, uint32_t>>> irregular;
    size_t nIrregular = 0;
    if (F.rows() == 4) {
        for (uint32_t f=0; f<F.cols(); ++f) {
            if (F(2, f) == F(3, f)) {
                nIrregular++;
                auto &value = irregular[F(2, f)];
                value.first = f;
                value.second[F(0, f)] = F(1, f);
            }
        }
    }

    ply_add_element(ply, "face", F.cols() - nIrregular + irregular.size());
    ply_add_list_property(ply, "vertex_indices", PLY_UINT8, PLY_INT);
    if (Nf.size() > 0) {
        ply_add_scalar_property(ply, "nx", PLY_FLOAT);
        ply_add_scalar_property(ply, "ny", PLY_FLOAT);
        ply_add_scalar_property(ply, "nz", PLY_FLOAT);
        if (Nf.cols() != F.cols() || Nf.rows() != 3)
            throw std::runtime_error("Face normal matrix has incorrect size");
    }
    ply_write_header(ply);

    for (uint32_t j=0; j<V.cols(); ++j) {
        for (uint32_t i=0; i<V.rows(); ++i)
            ply_write(ply, V(i, j));
        if (N.size() > 0) {
            for (uint32_t i=0; i<N.rows(); ++i)
                ply_write(ply, N(i, j));
        }
        if (UV.size() > 0) {
            for (uint32_t i=0; i<UV.rows(); ++i)
                ply_write(ply, UV(i, j));
        }
        if (C.size() > 0) {
            for (uint32_t i=0; i<std::min(3u, (uint32_t) C.rows()); ++i)
                ply_write(ply, C(i, j));
        }
        if (progress && j % 500000 == 0)
            progress("Writing vertex data", j / (Float) V.cols());
    }

    for (uint32_t f=0; f<F.cols(); ++f) {
        if (F.rows() == 4 && F(2, f) == F(3, f))
            continue;
        ply_write(ply, F.rows());
        for (uint32_t i=0; i<F.rows(); ++i)
            ply_write(ply, F(i, f));
        if (Nf.size() > 0) {
            for (uint32_t i=0; i<Nf.rows(); ++i)
                ply_write(ply, Nf(i, f));
        }
        if (progress && f % 500000 == 0)
            progress("Writing face data", f / (Float) F.cols());
    }

    for (auto item : irregular) {
        auto face = item.second;
        uint32_t v = face.second.begin()->first, first = v, i = 0;
        ply_write(ply, face.second.size());
        while (true) {
            ply_write(ply, v);
            v = face.second[v];
            ++i;
            if (v == first || i == face.second.size())
                break;
        }
        while (i != face.second.size()) {
            ply_write(ply, v);
            ++i;
        }
        if (Nf.size() > 0) {
            for (uint32_t i=0; i<Nf.rows(); ++i)
                ply_write(ply, Nf(i, face.first));
        }
    }

    ply_close(ply);
    cout << "done. (";
    if (irregular.size() > 0)
        cout << irregular.size() << " irregular faces, ";
    cout << "took " << timeString(timer.value()) << ")" << endl;
}

void triangulate_polygon(const std::vector<Vector3f> &p,
                         std::vector<uint32_t> &tris) {
    const uint32_t n = (uint32_t) p.size();
    if (n < 3)
        throw std::runtime_error("triangulate_polygon: polygon with fewer than 3 corners!");

    /* Fan over the corners listed in 'idx': (0,1,2), (i+1,0,i).
       For a quad this is exactly the historical OBJ loader split. */
    auto fan = [&](const std::vector<uint32_t> &idx) {
        tris.push_back(idx[0]); tris.push_back(idx[1]); tris.push_back(idx[2]);
        for (size_t i = 2; i + 1 < idx.size(); ++i) {
            tris.push_back(idx[i + 1]); tris.push_back(idx[0]); tris.push_back(idx[i]);
        }
    };

    std::vector<uint32_t> idx(n);
    for (uint32_t i = 0; i < n; ++i)
        idx[i] = i;

    if (n <= 4) {
        fan(idx);
        return;
    }

    /* Newell normal -> project onto the dominant axis plane, keeping the
       polygon counter-clockwise in 2D */
    Vector3f normal = Vector3f::Zero();
    for (uint32_t i = 0; i < n; ++i) {
        const Vector3f &a = p[i], &b = p[(i + 1) % n];
        normal += Vector3f((a.y() - b.y()) * (a.z() + b.z()),
                           (a.z() - b.z()) * (a.x() + b.x()),
                           (a.x() - b.x()) * (a.y() + b.y()));
    }
    int axis = 0;
    normal.cwiseAbs().maxCoeff(&axis);
    if (!std::isfinite(normal[axis]) || normal[axis] == 0) {
        fan(idx);
        return;
    }
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    const Float sign = normal[axis] > 0 ? 1 : -1;
    std::vector<Vector2f> q(n);
    for (uint32_t i = 0; i < n; ++i)
        q[i] = Vector2f(p[i][u], p[i][v] * sign);

    auto cross = [](const Vector2f &a, const Vector2f &b) {
        return a.x() * b.y() - a.y() * b.x();
    };
    auto inside = [&](const Vector2f &pt, const Vector2f &a, const Vector2f &b, const Vector2f &c) {
        return cross(b - a, pt - a) >= 0 && cross(c - b, pt - b) >= 0 &&
               cross(a - c, pt - c) >= 0;
    };

    /* Ear clipping. Pass 0 only accepts ears with a strictly positive area;
       pass 1 also accepts flat ones (collinear corners). If neither finds
       an ear the polygon is self-intersecting: fan whatever remains. */
    size_t start = 0;
    while (idx.size() > 3) {
        const size_t m = idx.size();
        bool clipped = false;
        for (int pass = 0; pass < 2 && !clipped; ++pass) {
            for (size_t j = 0; j < m && !clipped; ++j) {
                const size_t k = (start + j) % m;
                const uint32_t a = idx[(k + m - 1) % m], b = idx[k], c = idx[(k + 1) % m];
                const Float area = cross(q[b] - q[a], q[c] - q[b]);
                if (pass == 0 ? !(area > 0) : !(area >= 0))
                    continue;
                bool ear = true;
                for (uint32_t r : idx) {
                    if (r == a || r == b || r == c ||
                        q[r] == q[a] || q[r] == q[b] || q[r] == q[c])
                        continue;
                    if (inside(q[r], q[a], q[b], q[c])) {
                        ear = false;
                        break;
                    }
                }
                if (!ear)
                    continue;
                tris.push_back(a); tris.push_back(b); tris.push_back(c);
                idx.erase(idx.begin() + k);
                start = k % idx.size();
                clipped = true;
            }
        }
        if (!clipped)
            break;
    }
    fan(idx);
}

void build_mesh(const std::vector<Vector3f> &positions,
                const std::vector<uint32_t> &sizes,
                const std::vector<uint32_t> &indices,
                MatrixXu &F, MatrixXf &V, const std::string &source) {
    /* New index of every position, assigned on first use */
    std::vector<uint32_t> remap(positions.size(), (uint32_t) -1);
    std::vector<uint32_t> used, triangles;
    std::vector<Vector3f> polygon;
    std::vector<uint32_t> tris;

    size_t offset = 0;
    for (uint32_t size : sizes) {
        if (size < 3)
            throw std::runtime_error("Invalid face with fewer than 3 vertices in \"" + source + "\"!");
        if (size > indices.size() - offset)
            throw std::runtime_error("Face data truncated in \"" + source + "\"!");
        const uint32_t *face = indices.data() + offset;
        offset += size;

        polygon.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            if (face[i] >= positions.size())
                throw std::runtime_error("Vertex index " + std::to_string(face[i]) +
                                         " out of range in \"" + source + "\"!");
            polygon[i] = positions[face[i]];
        }
        tris.clear();
        triangulate_polygon(polygon, tris);

        for (uint32_t corner : tris) {
            uint32_t &id = remap[face[corner]];
            if (id == (uint32_t) -1) {
                id = (uint32_t) used.size();
                used.push_back(face[corner]);
            }
            triangles.push_back(id);
        }
    }

    F.resize(3, triangles.size() / 3);
    if (!triangles.empty())
        memcpy(F.data(), triangles.data(), sizeof(uint32_t) * triangles.size());

    V.resize(3, used.size());
    for (uint32_t i = 0; i < used.size(); ++i)
        V.col(i) = positions[used[i]];
}

void load_obj(const std::string &filename, MatrixXu &F, MatrixXf &V,
              const ProgressCallback &progress, uint64_t *polygons) {
    std::vector<Vector3f> positions;

    /// Position index of a face corner ("p", "p/uv", "p//n" or "p/uv/n"),
    /// converted from 1-based (or negative = relative to the positions read
    /// so far) to 0-based
    auto corner_index = [&](const std::string &string) -> uint32_t {
        std::vector<std::string> tokens = str_tokenize(string, '/', true);
        if (tokens.size() < 1 || tokens.size() > 3)
            throw std::runtime_error("Invalid vertex data: \"" + string + "\"");
        char *end = nullptr;
        const long long p = strtoll(tokens[0].c_str(), &end, 10);
        if (tokens[0].empty() || *end != '\0')
            throw std::runtime_error("Could not parse vertex index \"" + tokens[0] + "\"");
        if (p == 0 || (p < 0 && (unsigned long long) (-p) > positions.size()) || p > 0xffffffffLL)
            throw std::runtime_error("Vertex index " + tokens[0] + " out of range in OBJ file \"" + filename + "\"!");
        return (uint32_t) (p < 0 ? (long long) positions.size() + p : p - 1);
    };

    std::ifstream is(filename);
    if (is.fail())
        throw std::runtime_error("Unable to open OBJ file \"" + filename + "\"!");
    cout << "Loading \"" << filename << "\" .. ";
    cout.flush();
    Timer<> timer;

    std::vector<uint32_t> faceSizes, corners;   /* all face corners, in file order */

    std::string line_str;
    while (std::getline(is, line_str)) {
        std::istringstream line(line_str);

        std::string prefix;
        line >> prefix;

        if (prefix == "v") {
            Vector3f p;
            line >> p.x() >> p.y() >> p.z();
            positions.push_back(p);
        } else if (prefix == "f") {
            std::string token;
            uint32_t size = 0;
            while (line >> token) {
                corners.push_back(corner_index(token));
                ++size;
            }
            if (size < 3)
                throw std::runtime_error("Invalid face with fewer than 3 vertices in OBJ file \"" + filename + "\"!");
            faceSizes.push_back(size);
        }
    }

    for (uint32_t index : corners)   /* report 1-based, as in the file */
        if (index >= positions.size())
            throw std::runtime_error("Vertex index " + std::to_string((uint64_t) index + 1) +
                                     " out of range in OBJ file \"" + filename + "\"!");

    build_mesh(positions, faceSizes, corners, F, V, filename);
    if (polygons)
        *polygons = faceSizes.size();

    cout << "done. (V=" << V.cols() << ", F=" << F.cols() << ", took "
         << timeString(timer.value()) << ")" << endl;
}

void load_pointcloud(const std::string &filename, MatrixXf &V, MatrixXf &N,
                     const ProgressCallback &progress) {
    std::ifstream is(filename);
    if (is.fail())
        throw std::runtime_error("Unable to open ALN file \"" + filename + "\"!");
    cout.flush();
    Timer<> timer;
    std::istringstream line;

    auto fetch_line = [&]() {
        std::string line_str;
        do {
            std::getline(is, line_str);
            if (is.eof())
                throw std::runtime_error("Parser error while processing ALN file!");
        } while (line_str.empty() || line_str[0] == '#');
        line.clear();
        line.str(std::move(line_str));
    };

    auto fetch_string = [&](std::string &value) {
        while (!(line >> value)) {
            if (line.eof())
                fetch_line();
            else
                throw std::runtime_error("Parser error while processing ALN file!");
        }
    };
    auto fetch_uint = [&](uint32_t &value) {
        while (!(line >> value)) {
            if (line.eof())
                fetch_line();
            else
                throw std::runtime_error("Parser error while processing ALN file!");
        }
    };

    auto fetch_float = [&](Float &value) {
        while (!(line >> value)) {
            if (line.eof())
                fetch_line();
            else
                throw std::runtime_error("Parser error while processing ALN file!");
        }
    };

    uint32_t nFiles;
    fetch_uint(nFiles);

#if defined(_WIN32)
    char path_drive[_MAX_DRIVE];
    char path_dir[_MAX_DIR];
    char path_fname[_MAX_FNAME];
    char path_ext[_MAX_EXT];
    _splitpath(filename.c_str(), path_drive, path_dir, path_fname, path_ext);
#else
    char *path_dir = dirname((char *) filename.c_str());
#endif

    for (uint32_t i=0; i<nFiles; ++i) {
        std::string filename_sub;
        fetch_string(filename_sub);
        MatrixXu F_sub;
        MatrixXf V_sub, N_sub;
        load_ply(std::string(path_dir) + "/" + filename_sub, F_sub, V_sub, N_sub, true);
        Eigen::Matrix<Float, 4, 4> M;
        for (uint32_t k=0; k<16; ++k)
            fetch_float(M.data()[k]);
        M.transposeInPlace();
        for (uint32_t k=0; k<V_sub.cols(); ++k) {
            Vector4f p;
            p << V_sub.col(k), 1.0f;
            p = (M*p).eval();
            p /= p.w();
            V_sub.col(k) = p.head<3>();
        }
        if (N_sub.cols() == 0)
            generate_smooth_normals(F_sub, V_sub, N_sub, true);
        uint32_t base = (uint32_t) V.cols();
        V.conservativeResize(3, base + V_sub.cols());
        V.block(0, base, 3, V_sub.cols()) = V_sub;
        N.conservativeResize(3, base + N_sub.cols());
        N.block(0, base, 3, N_sub.cols()) = N_sub;
        if (progress)
            progress("Loading point cloud", i / (Float) (nFiles-1));
    }

    cout << "Point cloud loading finished. (V=" << V.cols() << ", took "
         << timeString(timer.value()) << ")" << endl;
}

size_t extracted_polygons(const MatrixXu &F, std::vector<uint32_t> &sizes,
                          std::vector<uint32_t> &indices, std::vector<uint32_t> &faceIds) {
    sizes.clear();
    indices.clear();
    faceIds.clear();

    /* Irregular faces: quads with F(2) == F(3) are directed edges (F(0) ->
       F(1)) of the polygon whose id is F(2) */
    std::map<uint32_t, std::pair<uint32_t, std::map<uint32_t, uint32_t>>> irregular;

    for (uint32_t f = 0; f < F.cols(); ++f) {
        if (F.rows() == 4 && F(2, f) == F(3, f)) {
            auto &value = irregular[F(2, f)];
            value.first = f;
            value.second[F(0, f)] = F(1, f);
            continue;
        }
        for (uint32_t j = 0; j < F.rows(); ++j)
            indices.push_back(F(j, f));
        sizes.push_back((uint32_t) F.rows());
        faceIds.push_back(f);
    }

    /* Walk each edge loop (same traversal as the historical OBJ writer,
       including its behaviour on open loops) */
    for (auto item : irregular) {
        auto face = item.second;
        uint32_t v = face.second.begin()->first, first = v, i = 0, size = 0;
        while (true) {
            indices.push_back(v);
            ++size;
            v = face.second[v];
            if (v == first || ++i == face.second.size())
                break;
        }
        sizes.push_back(size);
        faceIds.push_back(face.first);
    }
    return irregular.size();
}

void write_obj(const std::string &filename, const MatrixXu &F,
                const MatrixXf &V, const MatrixXf &N, const MatrixXf &Nf,
                const MatrixXf &UV, const MatrixXf &C,
                const ProgressCallback &progress) {
    Timer<> timer;
    cout << "Writing \"" << filename << "\" (V=" << V.cols()
         << ", F=" << F.cols() << ") .. ";
    cout.flush();
    std::ofstream os(filename);
    if (os.fail())
        throw std::runtime_error("Unable to open OBJ file \"" + filename + "\"!");
    if (N.size() > 0 && Nf.size() > 0)
        throw std::runtime_error("Please specify either face or vertex normals but not both!");

    for (uint32_t i=0; i<V.cols(); ++i)
        os << "v " << V(0, i) << " " << V(1, i) << " " << V(2, i) << endl;

    for (uint32_t i=0; i<N.cols(); ++i)
        os << "vn " << N(0, i) << " " << N(1, i) << " " << N(2, i) << endl;

    for (uint32_t i=0; i<Nf.cols(); ++i)
        os << "vn " << Nf(0, i) << " " << Nf(1, i) << " " << Nf(2, i) << endl;

    for (uint32_t i=0; i<UV.cols(); ++i)
        os << "vt " << UV(0, i) << " " << UV(1, i) << endl;

    std::vector<uint32_t> sizes, indices, faceIds;
    const size_t nIrregular = extracted_polygons(F, sizes, indices, faceIds);

    size_t offset = 0;
    for (size_t k = 0; k < sizes.size(); ++k) {
        os << "f ";
        for (uint32_t j = 0; j < sizes[k]; ++j) {
            uint32_t idx = indices[offset + j] + 1;
            os << idx;
            if (Nf.size() > 0)
                idx = faceIds[k] + 1;
            os << "//" << idx << " ";
        }
        offset += sizes[k];
        os << endl;
    }

    cout << "done. (";
    if (nIrregular > 0)
        cout << nIrregular << " irregular faces, ";
    cout << "took " << timeString(timer.value()) << ")" << endl;
}
