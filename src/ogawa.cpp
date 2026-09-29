/*
    ogawa.cpp: Hardened reader/writer for the Ogawa container format
    (see ogawa.h for the file layout and the safety guarantees)
*/

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "ogawa.h"
#include <unordered_map>
#include <unordered_set>
#include <cstdio>

namespace ogawa {

static bool host_is_little_endian() {
    const uint16_t probe = 1;
    uint8_t first;
    memcpy(&first, &probe, 1);
    return first == 1;
}

/* ------------------------------------------------------------------------- */
/*  Reader                                                                   */
/* ------------------------------------------------------------------------- */

Reader::Reader(const std::string &filename) : mFilename(filename) {
    if (!host_is_little_endian())
        fail("big-endian hosts are not supported");

    mStream.open(filename, std::ios::binary | std::ios::ate);
    if (mStream.fail())
        throw std::runtime_error("Unable to open Alembic file \"" + filename + "\"!");
    const std::streamoff end = mStream.tellg();
    if (end < 0)
        fail("unable to determine the file size");
    mSize = (uint64_t) end;

    if (mSize < HEADER_SIZE)
        fail("file too small to be an Alembic archive");

    uint8_t header[HEADER_SIZE];
    read(0, HEADER_SIZE, header);

    if (memcmp(header, "Ogawa", 5) != 0) {
        if (memcmp(header, "\x89HDF", 4) == 0)
            fail("legacy HDF5-based Alembic files are not supported, "
                 "re-export the file with the Ogawa backend");
        fail("not an Alembic (Ogawa) file");
    }
    if (header[5] != 0xff)
        fail("the archive was never finalized (the program that wrote it "
             "was probably interrupted)");
    const uint16_t version = (uint16_t) ((header[6] << 8) | header[7]);
    if (version != 1)
        fail("unsupported Ogawa version " + std::to_string(version));

    memcpy(&mRoot, header + 8, 8);
    if (is_data(mRoot) || mRoot == EMPTY_GROUP)
        fail("invalid root group");
    check_block(mRoot, "root group");
}

void Reader::fail(const std::string &msg) const {
    throw std::runtime_error("Alembic file \"" + mFilename + "\": " + msg + "!");
}

void Reader::read(uint64_t pos, uint64_t size, void *out) {
    if (pos > mSize || size > mSize - pos)
        fail("read past the end of the file");
    if (size == 0)
        return;
    mStream.clear();
    mStream.seekg((std::streamoff) pos, std::ios::beg);
    mStream.read((char *) out, (std::streamsize) size);
    if (!mStream || (uint64_t) mStream.gcount() != size)
        fail("I/O error while reading");
}

/* A block starts with a uint64 (count or size) that must lie after the
   header and inside the file */
void Reader::check_block(uint64_t pos, const char *what) const {
    if (pos < HEADER_SIZE || pos > mSize || mSize - pos < 8)
        fail(std::string(what) + " at an invalid position");
}

std::vector<uint64_t> Reader::group(uint64_t entry) {
    std::vector<uint64_t> children;
    if (is_data(entry))
        fail("expected a group, found a data block");
    if (entry == EMPTY_GROUP)
        return children;
    check_block(entry, "group");

    uint64_t count = 0;
    read(entry, 8, &count);
    if (count > (mSize - entry - 8) / 8)
        fail("group child count exceeds the file size");

    children.resize((size_t) count);
    if (count > 0)
        read(entry + 8, count * 8, children.data());

    for (uint64_t child : children) {
        const uint64_t pos = entry_pos(child);
        if (pos != 0)
            check_block(pos, is_data(child) ? "data block" : "group");
    }
    return children;
}

uint64_t Reader::data_size(uint64_t entry) {
    if (!is_data(entry))
        fail("expected a data block, found a group");
    const uint64_t pos = entry_pos(entry);
    if (pos == 0)
        return 0;
    check_block(pos, "data block");

    uint64_t size = 0;
    read(pos, 8, &size);
    if (size > mSize - pos - 8)
        fail("data block size exceeds the file size");
    return size;
}

void Reader::read_data(uint64_t entry, uint64_t offset, uint64_t size, void *out) {
    const uint64_t total = data_size(entry);
    if (offset > total || size > total - offset)
        fail("read past the end of a data block");
    read(entry_pos(entry) + 8 + offset, size, out);
}

std::vector<uint8_t> Reader::data(uint64_t entry) {
    std::vector<uint8_t> result((size_t) data_size(entry));
    if (!result.empty())
        read(entry_pos(entry) + 8, result.size(), result.data());
    return result;
}

/* ------------------------------------------------------------------------- */
/*  Writer                                                                   */
/* ------------------------------------------------------------------------- */

Writer::Writer(const std::string &filename)
    : mFilename(filename), mTempName(filename + ".tmp") {
    if (!host_is_little_endian())
        throw std::runtime_error("Ogawa writer: big-endian hosts are not supported!");

    mStream.open(mTempName, std::ios::binary | std::ios::trunc);
    if (mStream.fail())
        throw std::runtime_error("Unable to create Alembic file \"" + mTempName + "\"!");

    /* Not frozen (byte 5 = 0) and no root until commit() */
    const uint8_t header[HEADER_SIZE] = { 'O', 'g', 'a', 'w', 'a', 0, 0, 1,
                                          0, 0, 0, 0, 0, 0, 0, 0 };
    write(header, HEADER_SIZE);
}

Writer::~Writer() {
    if (!mCommitted) {
        mStream.close();
        std::remove(mTempName.c_str());
    }
}

void Writer::write(const void *data, uint64_t size) {
    if (mCommitted)
        throw std::runtime_error("Ogawa writer: archive already committed!");
    mStream.write((const char *) data, (std::streamsize) size);
    if (!mStream)
        throw std::runtime_error("Error while writing Alembic file \"" + mTempName +
                                 "\" (disk full?)!");
    mPos += size;
}

uint64_t Writer::add_data(const void *data, uint64_t size) {
    if (size == 0)
        return EMPTY_DATA;
    const uint64_t pos = mPos;
    write(&size, 8);
    write(data, size);
    return pos | DATA_BIT;
}

uint64_t Writer::add_data(Reader &in, uint64_t entry) {
    const uint64_t size = in.data_size(entry);
    if (size == 0)
        return EMPTY_DATA;
    const uint64_t pos = mPos;
    write(&size, 8);
    std::vector<uint8_t> buffer((size_t) std::min<uint64_t>(size, 1 << 20));
    for (uint64_t offset = 0; offset < size; ) {
        const uint64_t chunk = std::min<uint64_t>(buffer.size(), size - offset);
        in.read_data(entry, offset, chunk, buffer.data());
        write(buffer.data(), chunk);
        offset += chunk;
    }
    return pos | DATA_BIT;
}

uint64_t Writer::add_group(const std::vector<uint64_t> &entries) {
    if (entries.empty())
        return EMPTY_GROUP;
    for (uint64_t child : entries)
        if (entry_pos(child) >= mPos)
            throw std::runtime_error("Ogawa writer: group child written after its parent!");
    const uint64_t pos = mPos, count = entries.size();
    write(&count, 8);
    write(entries.data(), count * 8);
    return pos;
}

void Writer::commit(uint64_t root) {
    if (is_data(root) || root == EMPTY_GROUP || root >= mPos)
        throw std::runtime_error("Ogawa writer: invalid root group!");

    /* Root position first, then the frozen flag: a crash in between leaves
       a file that readers reject as unfinished */
    const uint8_t frozen = 0xff;
    mStream.seekp(8);
    write(&root, 8);
    mStream.seekp(5);
    write(&frozen, 1);
    mStream.flush();
    mStream.close();
    if (mStream.fail())
        throw std::runtime_error("Error while finalizing Alembic file \"" + mTempName + "\"!");

#if defined(_WIN32)
    const bool moved = MoveFileExA(mTempName.c_str(), mFilename.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = std::rename(mTempName.c_str(), mFilename.c_str()) == 0;
#endif
    if (!moved) {
        std::remove(mTempName.c_str());
        throw std::runtime_error("Unable to replace \"" + mFilename +
                                 "\" (is it open in another program?)!");
    }
    mCommitted = true;
}

/* ------------------------------------------------------------------------- */
/*  Traversals                                                               */
/* ------------------------------------------------------------------------- */

namespace {

/* Depth-first walk over the DAG: each distinct block is visited once
   (shared blocks cannot blow up the traversal), a group reachable from
   itself is reported as a cycle, and nesting is bounded by MAX_DEPTH. */
class Walker {
public:
    explicit Walker(Reader &in) : mIn(in) { }
    virtual ~Walker() { }

protected:
    /* Returns the value computed for 'entry' (memoized) */
    uint64_t visit(uint64_t entry, uint32_t depth) {
        if (entry_pos(entry) == 0)
            return entry;   /* empty group / empty data */

        auto it = mDone.find(entry);
        if (it != mDone.end())
            return it->second;

        uint64_t result;
        if (is_data(entry)) {
            result = on_data(entry);
        } else {
            if (depth >= MAX_DEPTH)
                mIn.fail("groups nested deeper than " + std::to_string(MAX_DEPTH) + " levels");
            if (!mActive.insert(entry).second)
                mIn.fail("cyclic group structure");
            std::vector<uint64_t> children = mIn.group(entry);
            std::vector<uint64_t> mapped(children.size());
            for (size_t i = 0; i < children.size(); ++i)
                mapped[i] = visit(children[i], depth + 1);
            mActive.erase(entry);
            result = on_group(entry, mapped, depth);
        }
        mDone[entry] = result;
        return result;
    }

    virtual uint64_t on_data(uint64_t entry) = 0;
    virtual uint64_t on_group(uint64_t entry, const std::vector<uint64_t> &children,
                              uint32_t depth) = 0;

    Reader &mIn;

private:
    std::unordered_map<uint64_t, uint64_t> mDone;
    std::unordered_set<uint64_t> mActive;
};

class Scanner : public Walker {
public:
    explicit Scanner(Reader &in) : Walker(in) { }
    Stats run() { visit(mIn.root(), 0); return mStats; }

protected:
    uint64_t on_data(uint64_t entry) override {
        mStats.datas++;
        mStats.dataBytes += mIn.data_size(entry);
        return entry;
    }
    uint64_t on_group(uint64_t entry, const std::vector<uint64_t> &children,
                      uint32_t depth) override {
        mStats.groups++;
        mStats.references += children.size();
        mStats.maxDepth = std::max(mStats.maxDepth, depth);
        return entry;
    }

private:
    Stats mStats;
};

class Copier : public Walker {
public:
    Copier(Reader &in, Writer &out) : Walker(in), mOut(out) { }
    uint64_t run(uint64_t entry) { return visit(entry, 0); }

protected:
    uint64_t on_data(uint64_t entry) override {
        return mOut.add_data(mIn, entry);
    }
    uint64_t on_group(uint64_t, const std::vector<uint64_t> &children, uint32_t) override {
        return mOut.add_group(children);
    }

private:
    Writer &mOut;
};

} // namespace

Stats scan(Reader &in) {
    return Scanner(in).run();
}

uint64_t copy(Reader &in, Writer &out, uint64_t entry) {
    return Copier(in, out).run(entry);
}

} // namespace ogawa
