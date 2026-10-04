/*
    usdcwrite.h: USD layers written as Crate (.usdc) and packages (.usdz).

    The writers of usdscene.h produce text; a .usdc output is that text read
    back (usda.cpp) and encoded as Crate 0.8.0, the version Pixar's USD
    wrote by default for years (read by every USD reader since): structural
    sections compressed (LZ4, integer coding), values stored uncompressed.
    A .usdz package is an uncompressed zip whose files start on 64-byte
    boundaries, the root layer first.
*/

#pragma once

#include "usd.h"

namespace usd {

/// Encodes a layer (as parse_usda() returns it) in Crate format
std::vector<uint8_t> encode_crate(const std::map<std::string, Value> &meta, const Prim &root);

/// A .usdz package of the given files (name, content), the root layer first
std::vector<uint8_t> encode_usdz(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files);

/// LZ4 block of 'data' (greedy matching; read back by usdc.h lz4_block)
std::vector<uint8_t> lz4_compress(const uint8_t *data, size_t size);

/// CRC-32 (zip, PNG)
uint32_t crc32(const uint8_t *data, size_t size);

/// LZ4 block holding 'data' as literals only (a valid block, no matches)
std::vector<uint8_t> lz4_literals(const uint8_t *data, size_t size);

/// Usd_IntegerCompression of 32-bit integers, before the LZ4 framing
std::vector<uint8_t> encode_ints(const std::vector<int32_t> &values);

/// Writes the layer 'usdaText' to 'filename' in the format of the extension
/// of 'output' (.usda, .usdc, .usdz; 'filename' may be a temporary file)
void write_layer_file(const std::string &filename, const std::string &usdaText, const std::string &output);

} // namespace usd
