/*
    usd.cpp: usd::Layer, whatever the file format (see usd.h)
*/

#include "usd.h"
#include "usdc.h"
#include <fstream>
#include <cstring>
#include <set>

namespace usd {

struct Layer::Impl {
    std::unique_ptr<CrateFile> crate;
    std::map<std::string, Prim *> prims;   /* .usdc: path -> prim (variants included) */
};

namespace {

std::string read_text(const std::string &filename, uint64_t start, uint64_t size) {
    std::ifstream is(filename, std::ios::binary);
    if (!is)
        throw std::runtime_error("Unable to open USD file \"" + filename + "\"!");
    if (size == 0) {
        is.seekg(0, std::ios::end);
        size = (uint64_t) is.tellg() - start;
    }
    std::string text((size_t) size, '\0');
    is.seekg((std::streamoff) start);
    if (size > 0)
        is.read(&text[0], (std::streamsize) size);
    if (!is)
        throw std::runtime_error("Unable to read USD file \"" + filename + "\"!");
    return text;
}

/* .usdz: an uncompressed zip whose first file is the root layer */
void usdz_root(const std::string &filename, std::string &name, uint64_t &start, uint64_t &size) {
    std::ifstream is(filename, std::ios::binary);
    if (!is)
        throw std::runtime_error("Unable to open USD file \"" + filename + "\"!");
    uint8_t h[30];
    is.read((char *) h, 30);
    if (!is || memcmp(h, "PK\x03\x04", 4) != 0)
        throw std::runtime_error("USD file \"" + filename + "\": not a .usdz package!");
    auto u16 = [&](int at) { return (uint32_t) h[at] | ((uint32_t) h[at + 1] << 8); };
    auto u32 = [&](int at) { return u16(at) | (u16(at + 2) << 16); };
    if (u16(8) != 0)
        throw std::runtime_error("USD file \"" + filename + "\": compressed .usdz entries are not allowed!");
    const uint32_t nameLength = u16(26), extraLength = u16(28);
    name.resize(nameLength);
    is.read(&name[0], nameLength);
    if (!is)
        throw std::runtime_error("USD file \"" + filename + "\": truncated .usdz package!");
    start = 30 + (uint64_t) nameLength + extraLength;
    size = u32(18);
    if (size == 0xffffffffu)
        throw std::runtime_error("USD file \"" + filename + "\": zip64 .usdz packages are not supported!");
}

std::string extension(const std::string &name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : str_tolower(name.substr(dot + 1));
}

/* .usdc specs -> prims */
class CrateBuilder {
public:
    CrateBuilder(CrateFile &cf, Layer::Impl &d, Prim &root, std::map<std::string, Value> &meta)
        : cf(cf), d(d), root(root), meta(meta) { }

    void build() {
        const std::vector<CrateFile::Field> &fields = cf.fields();
        const std::vector<uint32_t> &sets = cf.field_sets();
        std::map<Prim *, std::vector<std::string>> order;
        d.prims["/"] = &root;
        root.path = "/";
        for (const CrateFile::Spec &spec : cf.specs()) {
            const std::string &path = cf.path(spec.path);
            std::vector<const CrateFile::Field *> fs;
            for (size_t k = spec.fieldSet; k < sets.size() && sets[k] != 0xffffffffu; ++k) {
                if (sets[k] >= fields.size())
                    cf.fail("invalid field index");
                fs.push_back(&fields[sets[k]]);
            }
            switch (spec.type) {
                case CrateFile::SpecPseudoRoot:
                    for (const CrateFile::Field *f : fs) {
                        const std::string &name = cf.token(f->token);
                        Value v = cf.value(f->rep);
                        if (name == "primChildren")
                            order[&root] = v.strings;
                        else
                            meta[name] = v;
                    }
                    break;
                case CrateFile::SpecPrim:
                case CrateFile::SpecVariant: {
                    Prim *p = prim(path);
                    for (const CrateFile::Field *f : fs) {
                        const std::string &name = cf.token(f->token);
                        Value v = cf.value(f->rep);
                        if (name == "specifier") {
                            const int s = (int) v.num();
                            p->specifier = s == 1 ? Specifier::Over : (s == 2 ? Specifier::Class : Specifier::Def);
                        } else if (name == "typeName") {
                            p->type = v.str();
                        } else if (name == "primChildren") {
                            order[p] = v.strings;
                        } else if (name != "properties" && name != "variantChildren") {
                            p->meta[name] = v;
                        }
                    }
                    break;
                }
                case CrateFile::SpecAttribute:
                case CrateFile::SpecRelationship: {
                    if (path.find('[') != std::string::npos)
                        break;   /* relational attributes: not used */
                    const size_t slash = path.find_last_of("/}");
                    const size_t dot = path.find('.', slash == std::string::npos ? 0 : slash);
                    if (dot == std::string::npos)
                        cf.fail("invalid property path \"" + path + "\"");
                    Prim *p = prim(path.substr(0, dot));
                    p->properties.emplace_back();
                    Property &prop = p->properties.back();
                    prop.name = path.substr(dot + 1);
                    prop.relationship = spec.type == CrateFile::SpecRelationship;
                    for (const CrateFile::Field *f : fs) {
                        const std::string &name = cf.token(f->token);
                        if (name == "default") {
                            prop.crateDefault = f->rep;
                        } else if (name == "timeSamples") {
                            prop.hasTimeSamples = true;
                            prop.crateSamples = f->rep;
                        } else if (name == "typeName") {
                            prop.type = cf.value(f->rep).str();
                        } else if (name == "variability") {
                            prop.uniform = cf.value(f->rep).num() == 1;
                        } else if (name == "custom") {
                            prop.custom = cf.value(f->rep).num() != 0;
                        } else if (name == "targetPaths" || name == "connectionPaths") {
                            prop.targets = cf.value(f->rep).list_items();
                        } else {
                            prop.meta[name] = cf.value(f->rep);
                        }
                    }
                    break;
                }
                default:
                    break;   /* variant sets, connections, targets: nothing to keep */
            }
        }
        /* children in their authored order */
        for (auto &kv : order) {
            Prim *p = kv.first;
            std::vector<std::unique_ptr<Prim>> sorted;
            for (const std::string &name : kv.second)
                for (auto &c : p->children)
                    if (c && c->name == name)
                        sorted.push_back(std::move(c));
            for (auto &c : p->children)
                if (c)
                    sorted.push_back(std::move(c));
            p->children.swap(sorted);
        }
    }

private:
    /* The prim at a path, created with its parents if needed */
    Prim *prim(const std::string &path) {
        auto it = d.prims.find(path);
        if (it != d.prims.end())
            return it->second;
        if (path.empty() || path[0] != '/')
            cf.fail("invalid prim path \"" + path + "\"");
        if (d.prims.size() > 100000000)
            cf.fail("too many prims");
        Prim *created;
        if (path.back() == '}') {
            /* a variant: /A{set=selection} */
            const size_t open = path.rfind('{');
            const size_t eq = path.find('=', open);
            if (open == std::string::npos || eq == std::string::npos || open == 0)
                cf.fail("invalid variant path \"" + path + "\"");
            Prim *parent = prim(path.substr(0, open));
            std::unique_ptr<Prim> &slot =
                parent->variants[path.substr(open + 1, eq - open - 1)][path.substr(eq + 1, path.size() - eq - 2)];
            if (!slot) {
                slot.reset(new Prim());
                slot->name = path.substr(eq + 1, path.size() - eq - 2);
                slot->path = path;
            }
            created = slot.get();
        } else {
            const size_t pos = path.find_last_of("/}");
            const std::string parentPath = path[pos] == '/' ? (pos == 0 ? std::string("/") : path.substr(0, pos))
                                                            : path.substr(0, pos + 1);
            Prim *parent = prim(parentPath);
            parent->children.emplace_back(new Prim());
            created = parent->children.back().get();
            created->name = path.substr(pos + 1);
            created->path = path;
        }
        d.prims[path] = created;
        return created;
    }

    CrateFile &cf;
    Layer::Impl &d;
    Prim &root;
    std::map<std::string, Value> &meta;
};

} // namespace

Layer::Layer(const std::string &filename) : mFilename(filename), d(new Impl()) {
    char magic[8] = { 0 };
    {
        std::ifstream is(filename, std::ios::binary);
        if (!is)
            throw std::runtime_error("Unable to open USD file \"" + filename + "\"!");
        is.read(magic, 8);
    }
    uint64_t start = 0, size = 0;
    std::string inner;
    if (memcmp(magic, "PK\x03\x04", 4) == 0) {
        mFormat = "usdz";
        usdz_root(filename, inner, start, size);
        const std::string ext = extension(inner);
        if (ext != "usdc" && ext != "usda" && ext != "usd")
            throw std::runtime_error("USD file \"" + filename + "\": the package does not start with a layer!");
        std::ifstream is(filename, std::ios::binary);
        is.seekg((std::streamoff) start);
        is.read(magic, 8);
    } else if (memcmp(magic, "PXR-USDC", 8) == 0) {
        mFormat = "usdc";
    } else if (memcmp(magic, "#usda", 5) == 0) {
        mFormat = "usda";
    } else {
        throw std::runtime_error("USD file \"" + filename + "\": not a USD layer (.usda, .usdc or .usdz)!");
    }

    root.path = "/";
    if (memcmp(magic, "PXR-USDC", 8) == 0) {
        d->crate.reset(new CrateFile(filename, start, size));
        d->crate->parse();
        CrateBuilder(*d->crate, *d, root, meta).build();
    } else if (memcmp(magic, "#usda", 5) == 0) {
        parse_usda(read_text(filename, start, size), filename, meta, root);
    } else {
        throw std::runtime_error("USD file \"" + filename + "\": unknown layer format in the package!");
    }
}

Layer::Layer(const std::string &filename, const std::string &format)
    : mFilename(filename), mFormat(format), d(new Impl()) {
    root.path = "/";
}

Layer::~Layer() { }

const Prim *Layer::prim(const std::string &path) const {
    if (path == "/")
        return &root;
    const Prim *p = &root;
    size_t pos = 1;
    while (p && pos <= path.size()) {
        const size_t next = path.find('/', pos);
        const std::string name = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        p = p->child(name);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return p;
}

Value Layer::value(const Property &property) const {
    if (property.origin)
        return property.owner->value(*property.origin);
    if (d->crate && property.crateDefault != 0) {
        Value v = d->crate->value(property.crateDefault);
        v.type = property.type;
        return v;
    }
    return property.value;
}

size_t Layer::count(const Property &property) const {
    if (property.origin)
        return property.owner->count(*property.origin);
    if (d->crate && property.crateDefault != 0)
        return (size_t) d->crate->count(property.crateDefault);
    return property.value.size();
}

Value Layer::samples(const Property &property) const {
    if (property.origin)
        return property.owner->samples(*property.origin);
    if (!property.hasTimeSamples)
        return Value();
    if (d->crate) {
        Value v = d->crate->value(property.crateSamples);
        v.type = property.type;
        return v;
    }
    auto it = property.meta.find("__samples__");
    return it == property.meta.end() ? Value() : it->second;
}

} // namespace usd
