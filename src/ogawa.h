/*
    ogawa.h: Hardened reader/writer for Ogawa, the binary container format
    underneath Alembic (.abc) files. Self-contained: no Alembic library.

    File layout (all integers little endian):

        header   "Ogawa" | frozen flag (0xff once the writer finished)
                 | version (uint16, big endian, = 1) | root group position
        group    uint64 count, then 'count' uint64 child entries
        data     uint64 size,  then 'size' bytes

    A child entry with the top bit set refers to a data block, otherwise to a
    group; position 0 means "empty". Blocks may be shared (referenced by
    several entries), so the structure is a DAG rather than a tree.

    Every position, count and size read from a file is validated against the
    file size before it is used, and every traversal is memoized and bounded
    in depth: a corrupted or hostile file produces an std::runtime_error,
    never a crash, a hang or an out-of-bounds read.
*/

#pragma once

#include "common.h"
#include <fstream>

namespace ogawa {

const uint64_t DATA_BIT    = 0x8000000000000000ULL;
const uint64_t EMPTY_GROUP = 0;
const uint64_t EMPTY_DATA  = DATA_BIT;
const uint64_t HEADER_SIZE = 16;

/// Maximum nesting of groups accepted during a traversal
const uint32_t MAX_DEPTH = 1024;

inline bool is_data(uint64_t entry) { return (entry & DATA_BIT) != 0; }
inline uint64_t entry_pos(uint64_t entry) { return entry & ~DATA_BIT; }

class Reader {
public:
    /// Opens and validates the header; throws if this is not a complete,
    /// finalized Ogawa (version 1) file
    explicit Reader(const std::string &filename);

    const std::string &filename() const { return mFilename; }
    uint64_t size() const { return mSize; }

    /// Entry of the root group
    uint64_t root() const { return mRoot; }

    /// Child entries of a group entry (none for an empty group). Every
    /// child position is checked to lie inside the file.
    std::vector<uint64_t> group(uint64_t entry);

    /// Payload size of a data entry (0 for empty data), checked to fit
    uint64_t data_size(uint64_t entry);

    /// Reads 'size' payload bytes starting at 'offset' of a data entry
    void read_data(uint64_t entry, uint64_t offset, uint64_t size, void *out);

    /// Reads a whole data block
    std::vector<uint8_t> data(uint64_t entry);

    [[noreturn]] void fail(const std::string &msg) const;

private:
    void read(uint64_t pos, uint64_t size, void *out);
    void check_block(uint64_t pos, const char *what) const;

    std::string mFilename;
    std::ifstream mStream;
    uint64_t mSize = 0;
    uint64_t mRoot = 0;
};

class Writer {
public:
    /// Writes into a temporary file next to 'filename'; commit() moves it
    /// into place. If commit() is never reached (error, exception), the
    /// temporary file is deleted and 'filename' is left untouched.
    explicit Writer(const std::string &filename);
    ~Writer();

    /// Appends a data block, returns its entry (EMPTY_DATA when size == 0)
    uint64_t add_data(const void *data, uint64_t size);

    /// Streams a data block of 'in' into this file, returns its new entry
    uint64_t add_data(Reader &in, uint64_t entry);

    /// Appends a group whose children were written before, returns its entry
    /// (EMPTY_GROUP when there are no children)
    uint64_t add_group(const std::vector<uint64_t> &entries);

    /// Finalizes the header and atomically replaces 'filename'
    void commit(uint64_t root);

private:
    void write(const void *data, uint64_t size);

    std::string mFilename, mTempName;
    std::ofstream mStream;
    uint64_t mPos = 0;
    bool mCommitted = false;
};

struct Stats {
    uint64_t groups = 0;       ///< distinct non-empty groups
    uint64_t datas = 0;        ///< distinct non-empty data blocks
    uint64_t dataBytes = 0;    ///< total payload of distinct data blocks
    uint64_t references = 0;   ///< child entries followed (shared ones included)
    uint32_t maxDepth = 0;
};

/// Walks and validates the whole DAG reachable from the root
Stats scan(Reader &in);

/// Deep-copies the DAG rooted at 'entry' from 'in' into 'out' and returns
/// the new entry. Shared blocks stay shared, cycles are rejected.
uint64_t copy(Reader &in, Writer &out, uint64_t entry);

} // namespace ogawa
