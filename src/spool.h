/*
    spool.h: remeshed meshes waiting for the final write of a scene, kept in
    a temporary file (removed at the end, whatever happens) rather than in
    memory
*/

#pragma once

#include "meshio.h"
#include <fstream>
#include <functional>

class Spool {
public:
    explicit Spool(const std::string &path) : mPath(path) { }
    ~Spool() {
        if (mOut.is_open())
            mOut.close();
        if (mCreated)
            std::remove(mPath.c_str());
    }

    typedef std::function<void(MatrixXu &, MatrixXf &, std::vector<CornerUVs> &)> Fetch;

    /* Stores a mesh and its UV sets, returns the function that reads them back */
    Fetch put(const MatrixXu &F, const MatrixXf &V, const std::vector<CornerUVs> &uvs) {
        if (!mOut.is_open()) {
            mOut.open(mPath, std::ios::binary | std::ios::trunc);
            if (!mOut)
                throw std::runtime_error("Unable to create \"" + mPath + "\"!");
            mCreated = true;
        }
        Record r { mSize, (uint32_t) F.rows(), (uint64_t) F.cols(), (uint64_t) V.cols(), {} };
        write(F.data(), sizeof(MatrixXu::Scalar) * (size_t) F.size());
        write(V.data(), sizeof(MatrixXf::Scalar) * (size_t) V.size());
        for (const CornerUVs &set : uvs) {
            r.uvNames.push_back(set.name);
            r.uvCorners.push_back((uint64_t) set.corners.cols());
            write(set.corners.data(), sizeof(MatrixXf::Scalar) * (size_t) set.corners.size());
        }
        return [this, r](MatrixXu &Fo, MatrixXf &Vo, std::vector<CornerUVs> &uvo) { get(r, Fo, Vo, uvo); };
    }

private:
    struct Record {
        uint64_t offset;
        uint32_t rows;
        uint64_t faces, vertices;
        std::vector<std::string> uvNames;
        std::vector<uint64_t> uvCorners;
    };

    void write(const void *data, size_t size) {
        mOut.write((const char *) data, (std::streamsize) size);
        if (!mOut)
            throw std::runtime_error("Error while writing \"" + mPath + "\" (disk full?)!");
        mSize += size;
    }

    void get(const Record &r, MatrixXu &F, MatrixXf &V, std::vector<CornerUVs> &uvs) {
        mOut.flush();
        std::ifstream in(mPath, std::ios::binary);
        in.seekg((std::streamoff) r.offset);
        F.resize(r.rows, (std::ptrdiff_t) r.faces);
        V.resize(3, (std::ptrdiff_t) r.vertices);
        in.read((char *) F.data(), (std::streamsize) (sizeof(MatrixXu::Scalar) * (size_t) F.size()));
        in.read((char *) V.data(), (std::streamsize) (sizeof(MatrixXf::Scalar) * (size_t) V.size()));
        uvs.resize(r.uvNames.size());
        for (size_t i = 0; i < uvs.size(); ++i) {
            uvs[i].name = r.uvNames[i];
            uvs[i].corners.resize(2, (std::ptrdiff_t) r.uvCorners[i]);
            in.read((char *) uvs[i].corners.data(),
                    (std::streamsize) (sizeof(MatrixXf::Scalar) * (size_t) uvs[i].corners.size()));
        }
        if (!in)
            throw std::runtime_error("Unable to read back \"" + mPath + "\"!");
    }

    std::string mPath;
    std::ofstream mOut;
    bool mCreated = false;
    uint64_t mSize = 0;
};
