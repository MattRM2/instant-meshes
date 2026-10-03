/*
    usd.h: Universal Scene Description layers, read without any USD
    library: .usda (text), .usdc (Crate binary, compressed sections
    included) and .usdz (zip package), all into the same data model.

    A layer is what one file holds (no composition here): its metadata and
    a tree of prim specs, each with its metadata, properties (attributes and
    relationships), children and variants. Values are kept generic: numbers
    flattened into doubles, tokens / strings / asset paths / paths as
    strings. In a .usdc file the attribute values are decoded on demand
    (Layer::value()), so that listing a scene does not read its geometry.
*/

#pragma once

#include "common.h"
#include <map>
#include <memory>

namespace usd {

/// A value of any USD type, generic
struct Value {
    enum Kind {
        Empty,         ///< no value
        Blocked,       ///< "None": the value is blocked (removes a weaker opinion)
        Numbers,       ///< bool, integers, floats, vectors, matrices, quaternions (and their arrays)
        Strings,       ///< token, string, asset path, path (and their arrays)
        Dictionary,
        ListOp,        ///< list editing (references, payloads, inherits, API schemas...)
        TimeSamples,
        Unsupported    ///< a type this reader keeps but does not decode
    };
    Kind kind = Empty;
    std::string type;               ///< USD type name ("float3[]", "token", "matrix4d"...), when known
    bool array = false;
    int tuple = 1;                  ///< components per element (3 for float3, 16 for matrix4d)
    std::vector<double> numbers;    ///< flattened, matrices row by row
    std::vector<std::string> strings;
    std::map<std::string, Value> dict;
    /* ListOp: explicit items (isExplicit) or prepended / appended / deleted items */
    bool isExplicit = false;
    std::vector<std::string> explicitItems, prepended, appended, deleted;
    std::vector<std::pair<double, Value>> samples;   ///< TimeSamples: (time, value), sorted

    bool empty() const { return kind == Empty; }
    size_t size() const { return kind == Numbers ? numbers.size() / (size_t) tuple : strings.size(); }
    /// First string, "" if none
    std::string str() const { return strings.empty() ? std::string() : strings[0]; }
    /// First number, 'fallback' if none
    double num(double fallback = 0) const { return numbers.empty() ? fallback : numbers[0]; }
    /// The items a list op ends up with, for a single layer (explicit, or prepended + appended)
    std::vector<std::string> list_items() const;
};

enum class Specifier { Def, Over, Class };

struct Property {
    std::string name;
    bool relationship = false;
    bool custom = false;
    bool uniform = false;
    std::string type;                         ///< attribute type ("point3f[]"); "" for a relationship
    Value value;                              ///< default value (see Layer::value())
    bool hasTimeSamples = false;
    std::vector<std::string> targets;         ///< relationship targets, or attribute connections
    std::map<std::string, Value> meta;        ///< interpolation, elementSize...

    /* .usdc: where the default value and the time samples are (decoded by Layer::value()) */
    uint64_t crateDefault = 0, crateSamples = 0;

    /* A property of a composed stage (open_stage()): the property its value
       comes from, in the layer that holds it (Layer::value() reads it there) */
    const Property *origin = nullptr;
    const class Layer *owner = nullptr;
};

struct Prim {
    Specifier specifier = Specifier::Def;
    std::string type;                         ///< "Mesh", "Xform"... ("" for a typeless prim)
    std::string name;
    std::string path;                         ///< "/World/geo/Mesh" ("/" for the pseudo-root)
    std::map<std::string, Value> meta;        ///< kind, active, instanceable, references...
    std::vector<Property> properties;
    std::vector<std::unique_ptr<Prim>> children;
    /// variant set -> variant name -> its contents (properties, children)
    std::map<std::string, std::map<std::string, std::unique_ptr<Prim>>> variants;

    const Property *property(const std::string &name) const;
    Property *property(const std::string &name);
    const Prim *child(const std::string &name) const;
    const Value *metadata(const std::string &key) const;
};

class Layer {
public:
    /// Reads a .usda, .usdc or .usdz file (the format is detected from the
    /// content: a .usd file may be text or binary); throws on any error
    explicit Layer(const std::string &filename);
    /// An empty layer named 'filename' (a composed stage, see open_stage())
    Layer(const std::string &filename, const std::string &format);
    ~Layer();

    const std::string &filename() const { return mFilename; }
    /// "usda", "usdc" or "usdz"
    const std::string &format() const { return mFormat; }

    std::map<std::string, Value> meta;        ///< upAxis, metersPerUnit, defaultPrim, subLayers...
    Prim root;                                ///< the pseudo-root, path "/"
    /// A composed stage: the layers its properties come from (kept open)
    std::vector<std::shared_ptr<const Layer>> sources;

    /// The prim at a path (nullptr if absent)
    const Prim *prim(const std::string &path) const;

    /// Default value of an attribute (decoded on demand for .usdc)
    Value value(const Property &property) const;
    /// Time samples of an attribute (Value::TimeSamples, Empty if none)
    Value samples(const Property &property) const;
    /// Element count of the default value (without decoding it in .usdc)
    size_t count(const Property &property) const;

    struct Impl;

private:
    std::string mFilename, mFormat;
    std::unique_ptr<Impl> d;
};

/// Reads a .usda layer from text ('source' names it in error messages)
void parse_usda(const std::string &text, const std::string &source,
                std::map<std::string, Value> &meta, Prim &root);

/// Path utilities: "/A/B" + "C" -> "/A/B/C"; parent of "/A/B" is "/A"
std::string child_path(const std::string &parent, const std::string &name);

} // namespace usd
