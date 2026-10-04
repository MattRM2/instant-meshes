/*
    usdedit.cpp: in-place edits of USD layers (see usdedit.h)
*/

#include "usdedit.h"
#include "usdc.h"
#include "usdcwrite.h"
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

namespace usd {

/* ------------------------------------------------------------------------- */
/*  .usda                                                                    */
/* ------------------------------------------------------------------------- */

namespace {

/* Just enough of the .usda syntax to find the root prims and their
   metadata: comments, strings, asset paths, prim paths and brackets are
   skipped whole */
class UsdaText {
public:
    explicit UsdaText(const std::string &s) : s(s) { }

    size_t skip(size_t i) const {
        while (i < s.size()) {
            if (std::isspace((unsigned char) s[i])) {
                ++i;
            } else if (s[i] == '#') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
            } else {
                break;
            }
        }
        return i;
    }

    /* End of the token at 'i' */
    size_t end(size_t i) const {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const std::string triple(3, c);
            const bool three = s.compare(i, 3, triple) == 0;
            for (size_t j = i + (three ? 3 : 1); j < s.size(); ++j) {
                if (s[j] == '\\') {
                    ++j;
                    continue;
                }
                if (three ? s.compare(j, 3, triple) == 0 : s[j] == c)
                    return j + (three ? 3 : 1);
            }
            fail("unterminated string");
        }
        if (c == '@') {
            const bool three = s.compare(i, 3, "@@@") == 0;
            for (size_t j = i + (three ? 3 : 1); j < s.size(); ++j) {
                if (three && s[j] == '\\') {
                    ++j;
                    continue;
                }
                if (three ? s.compare(j, 3, "@@@") == 0 : s[j] == '@')
                    return j + (three ? 3 : 1);
            }
            fail("unterminated asset path");
        }
        if (c == '<') {
            const size_t j = s.find('>', i);
            if (j == std::string::npos)
                fail("unterminated path");
            return j + 1;
        }
        if (c == '(' || c == '[' || c == '{')
            return match(i);
        if (word(c)) {
            size_t j = i;
            while (j < s.size() && word(s[j]))
                ++j;
            return j;
        }
        return i + 1;
    }

    /* After the bracket that closes the one at 'i' */
    size_t match(size_t i) const {
        const char close = s[i] == '(' ? ')' : s[i] == '[' ? ']' : '}';
        for (size_t j = skip(i + 1);; j = skip(j)) {
            if (j >= s.size())
                fail(std::string("missing '") + close + "'");
            if (s[j] == close)
                return j + 1;
            j = end(j);
        }
    }

    struct RootPrim {
        std::string name;
        size_t nameEnd = 0;
        size_t metaStart = std::string::npos, metaEnd = 0;   ///< '(' and after ')'
    };

    std::vector<RootPrim> root_prims() const {
        std::vector<RootPrim> prims;
        size_t i = skip(0);
        if (i < s.size() && s[i] == '(')
            i = match(i);   /* layer metadata */
        while ((i = skip(i)) < s.size()) {
            const size_t e = end(i);
            const std::string w = s.substr(i, e - i);
            i = e;
            if (w != "def" && w != "over" && w != "class")
                continue;
            size_t j = skip(i);
            if (j < s.size() && s[j] != '"' && s[j] != '\'')
                j = skip(end(j));   /* the type */
            if (j >= s.size() || (s[j] != '"' && s[j] != '\''))
                fail("expected a prim name");
            RootPrim p;
            p.nameEnd = end(j);
            p.name = s.substr(j + 1, p.nameEnd - j - 2);
            j = skip(p.nameEnd);
            if (j < s.size() && s[j] == '(') {
                p.metaStart = j;
                p.metaEnd = match(j);
                j = skip(p.metaEnd);
            }
            if (j >= s.size() || s[j] != '{')
                fail("expected '{' after the prim \"" + p.name + "\"");
            i = match(j);
            prims.push_back(p);
        }
        return prims;
    }

    /* A "references" statement of a metadata block */
    struct Statement {
        std::string op;               ///< "", "prepend", "append", "add", "delete", "reorder"
        size_t value = 0, valueEnd = 0;
    };

    std::vector<Statement> references(size_t metaStart, size_t metaEnd) const {
        std::vector<Statement> out;
        std::string prev;
        for (size_t j = skip(metaStart + 1); j + 1 < metaEnd; j = skip(j)) {
            size_t e = end(j);
            const std::string w = s.substr(j, e - j);
            if (w == "references") {
                const size_t k = skip(e);
                if (k < metaEnd && s[k] == '=') {
                    Statement st;
                    st.op = prev == "prepend" || prev == "append" || prev == "add" || prev == "delete" ||
                            prev == "reorder" ? prev : std::string();
                    st.value = skip(k + 1);
                    st.valueEnd = end(st.value);
                    if (s[st.value] == '@' || s[st.value] == '<') {   /* @asset@ <path> (layer offset) */
                        size_t t = skip(st.valueEnd);
                        if (s[st.value] == '@' && t < metaEnd && s[t] == '<') {
                            st.valueEnd = end(t);
                            t = skip(st.valueEnd);
                        }
                        if (t < metaEnd - 1 && s[t] == '(')
                            st.valueEnd = end(t);
                    }
                    out.push_back(st);
                    e = st.valueEnd;
                }
            }
            prev = w;
            j = e;
        }
        return out;
    }

    [[noreturn]] void fail(const std::string &msg) const {
        throw std::runtime_error(".usda edit: " + msg + "!");
    }

private:
    static bool word(char c) { return std::isalnum((unsigned char) c) || c == '_' || c == ':'; }
    const std::string &s;
};

std::string item_text(const RootReference &r) {
    return "@" + r.asset + "@<" + r.target + ">";
}

} // namespace

std::string usda_add_references(const std::string &input, const std::vector<RootReference> &refs, bool *changed) {
    std::string s = input;
    bool any = false;
    for (const RootReference &r : refs) {
        const std::string item = item_text(r);
        const UsdaText text(s);
        const std::vector<UsdaText::RootPrim> prims = text.root_prims();
        const UsdaText::RootPrim *prim = nullptr;
        for (const auto &p : prims)
            if (p.name == r.prim)
                prim = &p;
        if (!prim) {
            /* defined in another layer (a sublayer): an over of it here */
            if (!s.empty() && s.back() != '\n')
                s += "\n";
            s += "\nover \"" + r.prim + "\" (\n    prepend references = " + item + "\n)\n{\n}\n";
            any = true;
            continue;
        }
        if (prim->metaStart == std::string::npos) {
            s.insert(prim->nameEnd, " (\n    prepend references = " + item + "\n)");
            any = true;
            continue;
        }
        if (s.substr(prim->metaStart, prim->metaEnd - prim->metaStart).find(item) != std::string::npos)
            continue;   /* already there */
        const UsdaText::Statement *st = nullptr;
        for (const auto &x : text.references(prim->metaStart, prim->metaEnd))
            if (x.op.empty() || x.op == "prepend")
                st = &x;
        if (st) {
            const std::string value = s.substr(st->value, st->valueEnd - st->value);
            std::string updated;
            if (value[0] == '[') {
                const size_t first = text.skip(st->value + 1);
                updated = s[first] == ']' ? "[" + item + "]"
                                          : "[" + item + ", " + s.substr(st->value + 1, st->valueEnd - st->value - 1);
            } else if (value == "None") {
                updated = "[" + item + "]";
            } else {
                updated = "[" + item + ", " + value + "]";
            }
            s.replace(st->value, st->valueEnd - st->value, updated);
        } else {
            /* a new statement, on its own line before the ')' */
            const size_t close = prim->metaEnd - 1;
            size_t lineStart = close;
            while (lineStart > 0 && (s[lineStart - 1] == ' ' || s[lineStart - 1] == '\t'))
                --lineStart;
            if (lineStart > 0 && s[lineStart - 1] == '\n')
                s.insert(lineStart, "    prepend references = " + item + "\n");
            else
                s.insert(close, "\n    prepend references = " + item + "\n");
        }
        any = true;
    }
    if (changed)
        *changed = any;
    return s;
}

/* ------------------------------------------------------------------------- */
/*  .usdc                                                                    */
/* ------------------------------------------------------------------------- */

namespace {

/* Crate value types used here (usdc.cpp has the whole list) */
const int TReferenceListOp = 35, TTokenVector = 41, TSpecifier = 42;
const uint64_t InlinedBit = 1ull << 62, PayloadMask = (1ull << 48) - 1;

uint64_t rep_of(int type, bool inlined, uint64_t payload) {
    return (inlined ? InlinedBit : 0) | ((uint64_t) type << 48) | (payload & PayloadMask);
}

int type_of(uint64_t rep) { return (int) ((rep >> 48) & 0xff); }

/* An SdfReference as Crate stores it, without custom data: asset (string
   index), prim path (path index), layer offset and scale, empty dictionary */
typedef std::array<uint8_t, 32> RefItem;

RefItem ref_item(uint32_t asset, uint32_t path) {
    RefItem item {};
    const double offset = 0.0, scale = 1.0;
    memcpy(item.data(), &asset, 4);
    memcpy(item.data() + 4, &path, 4);
    memcpy(item.data() + 8, &offset, 8);
    memcpy(item.data() + 16, &scale, 8);
    return item;   /* custom data count: 0 */
}

class CrateEdit {
public:
    explicit CrateEdit(CrateFile &c)
        : c(c), out(c.raw(0, c.size())), tokens(c.tokens()), strings(c.string_tokens()), fields(c.fields()),
          fieldSets(c.field_sets()), specs(c.specs()), tree(c.path_tree()), numPaths(c.path_count()) {
        for (uint32_t k = 0; k < (uint32_t) tokens.size(); ++k)
            tokenIds.emplace(tokens[k], k);
        for (uint32_t k = 0; k < (uint32_t) numPaths; ++k)
            pathIds.emplace(c.path(k), k);
    }

    bool add(const RootReference &r) {
        const std::string primPath = "/" + r.prim;
        if (r.target != primPath)
            throw std::logic_error("usdc_add_references: the target must be the root prim itself");
        auto pit = pathIds.find(primPath);
        const uint32_t path = pit != pathIds.end() ? pit->second : add_root_path(r.prim);
        const RefItem item = ref_item(string(r.asset), path);

        size_t specIndex = specs.size();
        for (size_t k = 0; k < specs.size(); ++k)
            if (specs[k].path == path && specs[k].type == CrateFile::SpecPrim)
                specIndex = k;
        if (specIndex == specs.size()) {
            /* defined in another layer: an over of it here, a child of the pseudo-root */
            std::vector<uint32_t> list;
            list.push_back(field("specifier", rep_of(TSpecifier, true, 1)));
            list.push_back(field("references", list_op(0, { { 32, { item } } })));
            specs.push_back(CrateFile::Spec { path, field_set(list), (uint32_t) CrateFile::SpecPrim });
            add_root_child(r.prim);
            return true;
        }

        std::vector<uint32_t> list = fields_of(specs[specIndex].fieldSet);
        size_t at = list.size();
        for (size_t k = 0; k < list.size(); ++k)
            if (tokens[fields[list[k]].token] == "references")
                at = k;
        uint64_t rep;
        if (at == list.size()) {
            rep = list_op(0, { { 32, { item } } });
            list.push_back(0);
        } else {
            /* the existing list op, the reference first among the explicit
               or prepended items */
            const uint64_t old = fields[list[at]].rep;
            if (type_of(old) != TReferenceListOp || (old & InlinedBit))
                c.fail("unexpected references field on " + primPath);
            uint64_t pos = old & PayloadMask;
            const uint8_t header = read<uint8_t>(pos);
            std::vector<std::pair<uint8_t, std::vector<RefItem>>> lists;
            for (uint8_t bit : { 2, 4, 32, 64, 8, 16 }) {
                if (!(header & bit))
                    continue;
                const uint64_t count = read<uint64_t>(pos);
                if (count > out.size() / 32)
                    c.fail("invalid reference list on " + primPath);
                std::vector<RefItem> items;
                for (uint64_t k = 0; k < count; ++k) {
                    RefItem x;
                    if (pos + 32 > out.size())
                        c.fail("invalid reference list on " + primPath);
                    memcpy(x.data(), out.data() + pos, 32);
                    pos += 32;
                    uint64_t customData;
                    memcpy(&customData, x.data() + 24, 8);
                    if (customData != 0)
                        c.fail("a reference of " + primPath + " has custom data, that cannot be kept in an edit");
                    if (x == item)
                        return false;   /* already there */
                    items.push_back(x);
                }
                lists.emplace_back(bit, items);
            }
            const uint8_t into = (header & 1) ? 2 : 32;
            bool placed = false;
            for (auto &l : lists)
                if (l.first == into) {
                    l.second.insert(l.second.begin(), item);
                    placed = true;
                }
            if (!placed)
                lists.emplace_back(into, std::vector<RefItem> { item });
            rep = list_op(header & 1, lists);
        }
        list[at] = field("references", rep);
        specs[specIndex].fieldSet = field_set(list);
        return true;
    }

    /* The file with the new sections and table of contents appended */
    std::vector<uint8_t> finish() {
        std::vector<CrateFile::Section> toc;
        auto section = [&](const char *name, std::function<void()> body) {
            align();
            const uint64_t start = out.size();
            body();
            toc.push_back(CrateFile::Section { name, start, out.size() - start });
        };
        section("TOKENS", [&] {
            std::vector<uint8_t> chars;
            for (const std::string &t : tokens) {
                chars.insert(chars.end(), t.begin(), t.end());
                chars.push_back(0);
            }
            put<uint64_t>(tokens.size());
            put<uint64_t>(chars.size());
            const std::vector<uint8_t> z = compress(chars);
            put<uint64_t>(z.size());
            bytes(z);
        });
        section("STRINGS", [&] {
            put<uint64_t>(strings.size());
            for (uint32_t s : strings)
                put<uint32_t>(s);
        });
        section("FIELDS", [&] {
            put<uint64_t>(fields.size());
            std::vector<int32_t> t;
            std::vector<uint8_t> reps(fields.size() * 8);
            for (size_t k = 0; k < fields.size(); ++k) {
                t.push_back((int32_t) fields[k].token);
                memcpy(reps.data() + 8 * k, &fields[k].rep, 8);
            }
            ints(t);
            const std::vector<uint8_t> z = compress(reps);
            put<uint64_t>(z.size());
            bytes(z);
        });
        section("FIELDSETS", [&] {
            put<uint64_t>(fieldSets.size());
            ints(std::vector<int32_t>(fieldSets.begin(), fieldSets.end()));
        });
        section("PATHS", [&] {
            put<uint64_t>(numPaths);
            put<uint64_t>(tree.indexes.size());
            ints(std::vector<int32_t>(tree.indexes.begin(), tree.indexes.end()));
            ints(tree.elements);
            ints(tree.jumps);
        });
        section("SPECS", [&] {
            put<uint64_t>(specs.size());
            std::vector<int32_t> p, f, t;
            for (const auto &s : specs) {
                p.push_back((int32_t) s.path);
                f.push_back((int32_t) s.fieldSet);
                t.push_back((int32_t) s.type);
            }
            ints(p);
            ints(f);
            ints(t);
        });
        /* sections this edit does not touch stay where they are */
        for (const CrateFile::Section &s : c.sections()) {
            bool known = false;
            for (const char *name : { "TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS" })
                known |= s.name == name;
            if (!known)
                toc.push_back(s);
        }
        align();
        const uint64_t tocOffset = out.size();
        put<uint64_t>(toc.size());
        for (const CrateFile::Section &s : toc) {
            char name[16] = { 0 };
            memcpy(name, s.name.c_str(), std::min<size_t>(s.name.size(), 15));
            out.insert(out.end(), name, name + 16);
            put<uint64_t>(s.start);
            put<uint64_t>(s.size);
        }
        memcpy(out.data() + 16, &tocOffset, 8);
        return std::move(out);
    }

private:
    template <typename T> void put(T v) {
        const size_t at = out.size();
        out.resize(at + sizeof(T));
        memcpy(out.data() + at, &v, sizeof(T));
    }
    template <typename T> T read(uint64_t &pos) {
        if (pos + sizeof(T) > out.size())
            c.fail("value past the end of the file");
        T v;
        memcpy(&v, out.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    void bytes(const std::vector<uint8_t> &b) { out.insert(out.end(), b.begin(), b.end()); }
    void align() {
        while (out.size() % 8)
            out.push_back(0);
    }
    static std::vector<uint8_t> compress(const std::vector<uint8_t> &data) {
        std::vector<uint8_t> z { 0 };   /* TfFastCompression: one chunk */
        const std::vector<uint8_t> block = lz4_literals(data.data(), data.size());
        z.insert(z.end(), block.begin(), block.end());
        return z;
    }
    void ints(const std::vector<int32_t> &values) {
        const std::vector<uint8_t> z = compress(encode_ints(values));
        put<uint64_t>(z.size());
        bytes(z);
    }

    uint32_t token(const std::string &t) {
        auto it = tokenIds.find(t);
        if (it != tokenIds.end())
            return it->second;
        tokens.push_back(t);
        return tokenIds[t] = (uint32_t) (tokens.size() - 1);
    }
    uint32_t string(const std::string &s) {
        const uint32_t t = token(s);
        for (uint32_t k = 0; k < (uint32_t) strings.size(); ++k)
            if (strings[k] == t)
                return k;
        strings.push_back(t);
        return (uint32_t) (strings.size() - 1);
    }
    uint32_t field(const char *name, uint64_t rep) {
        fields.push_back(CrateFile::Field { token(name), rep });
        return (uint32_t) (fields.size() - 1);
    }
    uint32_t field_set(const std::vector<uint32_t> &list) {
        const uint32_t set = (uint32_t) fieldSets.size();
        fieldSets.insert(fieldSets.end(), list.begin(), list.end());
        fieldSets.push_back(0xffffffffu);
        return set;
    }
    std::vector<uint32_t> fields_of(uint32_t set) const {
        std::vector<uint32_t> list;
        for (size_t k = set; k < fieldSets.size() && fieldSets[k] != 0xffffffffu; ++k)
            list.push_back(fieldSets[k]);
        return list;
    }

    /* A list op of references: header bit 1 (explicit), then the lists in
       Crate's order (explicit, added, prepended, appended, deleted, ordered) */
    uint64_t list_op(uint8_t isExplicit, std::vector<std::pair<uint8_t, std::vector<RefItem>>> lists) {
        static const uint8_t order[] = { 2, 4, 32, 64, 8, 16 };
        uint8_t header = isExplicit;
        for (const auto &l : lists)
            header |= l.first;
        align();
        const uint64_t at = out.size();
        put<uint8_t>(header);
        for (uint8_t bit : order)
            for (const auto &l : lists)
                if (l.first == bit) {
                    put<uint64_t>(l.second.size());
                    for (const RefItem &x : l.second)
                        out.insert(out.end(), x.begin(), x.end());
                }
        return rep_of(TReferenceListOp, false, at);
    }

    /* A new root prim path, the last child of "/" in the path tree */
    uint32_t add_root_path(const std::string &name) {
        if (tree.indexes.empty())
            c.fail("empty path tree");
        const uint32_t index = (uint32_t) numPaths++;
        const int32_t n = (int32_t) tree.indexes.size();
        if (tree.jumps[0] == -2) {
            tree.jumps[0] = -1;   /* the root gets its first child */
        } else {
            int32_t cur = 1;
            while (cur < n && (tree.jumps[cur] == 0 || tree.jumps[cur] > 0))
                cur = tree.jumps[cur] == 0 ? cur + 1 : cur + tree.jumps[cur];
            if (cur >= n)
                c.fail("corrupted path tree");
            if (tree.jumps[cur] == -1)
                tree.jumps[cur] = n - cur;   /* children, now a sibling at the end */
            else if (cur == n - 1)
                tree.jumps[cur] = 0;         /* no children: the sibling follows */
            else
                c.fail("corrupted path tree");
        }
        tree.indexes.push_back(index);
        tree.elements.push_back((int32_t) token(name));
        tree.jumps.push_back(-2);
        pathIds["/" + name] = index;
        return index;
    }

    /* The pseudo-root lists the new root prim among its children */
    void add_root_child(const std::string &name) {
        for (CrateFile::Spec &s : specs) {
            if (s.type != CrateFile::SpecPseudoRoot)
                continue;
            std::vector<uint32_t> list = fields_of(s.fieldSet);
            std::vector<uint32_t> names;
            size_t at = list.size();
            for (size_t k = 0; k < list.size(); ++k) {
                if (tokens[fields[list[k]].token] != "primChildren")
                    continue;
                at = k;
                const uint64_t rep = fields[list[k]].rep;
                if (!(rep & InlinedBit) && (rep & PayloadMask) != 0) {
                    uint64_t pos = rep & PayloadMask;
                    const uint64_t count = read<uint64_t>(pos);
                    if (count > out.size() / 4)
                        c.fail("invalid primChildren");
                    for (uint64_t j = 0; j < count; ++j)
                        names.push_back(read<uint32_t>(pos));
                }
            }
            names.push_back(token(name));
            align();
            const uint64_t pos = out.size();
            put<uint64_t>(names.size());
            for (uint32_t t : names)
                put<uint32_t>(t);
            if (at == list.size())
                list.push_back(0);
            list[at] = field("primChildren", rep_of(TTokenVector, false, pos));
            s.fieldSet = field_set(list);
            return;
        }
        c.fail("no pseudo-root");
    }

    CrateFile &c;
    std::vector<uint8_t> out;
    std::vector<std::string> tokens;
    std::map<std::string, uint32_t> tokenIds, pathIds;
    std::vector<uint32_t> strings;
    std::vector<CrateFile::Field> fields;
    std::vector<uint32_t> fieldSets;
    std::vector<CrateFile::Spec> specs;
    CrateFile::PathTree tree;
    size_t numPaths;
};

} // namespace

std::vector<uint8_t> usdc_add_references(const std::string &filename, const std::vector<RootReference> &refs,
                                         bool *changed) {
    CrateFile c(filename);
    c.parse();
    if (c.version() < 0x000400)
        c.fail("Crate version older than 0.4.0: save it again with a recent USD to add references to it");
    CrateEdit edit(c);
    bool any = false;
    for (const RootReference &r : refs)
        any |= edit.add(r);
    if (changed)
        *changed = any;
    return any ? edit.finish() : c.raw(0, c.size());
}

} // namespace usd
