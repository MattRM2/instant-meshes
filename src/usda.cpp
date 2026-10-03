/*
    usda.cpp: .usda (text) layers and the shared parts of the USD data
    model (see usd.h)
*/

#include "usd.h"
#include <cstring>
#include <cstdlib>
#include <set>

namespace usd {

/* ------------------------------------------------------------------------- */
/*  Data model                                                               */
/* ------------------------------------------------------------------------- */

std::vector<std::string> Value::list_items() const {
    if (kind == Strings)
        return strings;
    if (kind != ListOp)
        return std::vector<std::string>();
    if (isExplicit)
        return explicitItems;
    std::vector<std::string> items = prepended;
    items.insert(items.end(), appended.begin(), appended.end());
    return items;
}

const Property *Prim::property(const std::string &n) const {
    for (const Property &p : properties)
        if (p.name == n)
            return &p;
    return nullptr;
}

Property *Prim::property(const std::string &n) {
    for (Property &p : properties)
        if (p.name == n)
            return &p;
    return nullptr;
}

const Prim *Prim::child(const std::string &n) const {
    for (const auto &c : children)
        if (c->name == n)
            return c.get();
    return nullptr;
}

const Value *Prim::metadata(const std::string &key) const {
    auto it = meta.find(key);
    return it == meta.end() ? nullptr : &it->second;
}

std::string child_path(const std::string &parent, const std::string &name) {
    return (parent == "/" ? "" : parent) + "/" + name;
}

namespace {

/* Types whose values are numbers, with their component count */
int numeric_tuple(const std::string &base) {
    static const std::map<std::string, int> types = {
        { "bool", 1 }, { "uchar", 1 }, { "int", 1 }, { "uint", 1 }, { "int64", 1 }, { "uint64", 1 },
        { "half", 1 }, { "float", 1 }, { "double", 1 }, { "timecode", 1 },
        { "int2", 2 }, { "int3", 3 }, { "int4", 4 },
        { "half2", 2 }, { "half3", 3 }, { "half4", 4 },
        { "float2", 2 }, { "float3", 3 }, { "float4", 4 },
        { "double2", 2 }, { "double3", 3 }, { "double4", 4 },
        { "point3h", 3 }, { "point3f", 3 }, { "point3d", 3 },
        { "vector3h", 3 }, { "vector3f", 3 }, { "vector3d", 3 },
        { "normal3h", 3 }, { "normal3f", 3 }, { "normal3d", 3 },
        { "color3h", 3 }, { "color3f", 3 }, { "color3d", 3 },
        { "color4h", 4 }, { "color4f", 4 }, { "color4d", 4 },
        { "texCoord2h", 2 }, { "texCoord2f", 2 }, { "texCoord2d", 2 },
        { "texCoord3h", 3 }, { "texCoord3f", 3 }, { "texCoord3d", 3 },
        { "quath", 4 }, { "quatf", 4 }, { "quatd", 4 },
        { "matrix2d", 4 }, { "matrix3d", 9 }, { "matrix4d", 16 }, { "frame4d", 16 },
    };
    auto it = types.find(base);
    return it == types.end() ? 0 : it->second;
}

bool string_type(const std::string &base) {
    return base == "string" || base == "token" || base == "asset" || base == "path";
}

/* ------------------------------------------------------------------------- */
/*  Parser                                                                   */
/* ------------------------------------------------------------------------- */

class Parser {
public:
    Parser(const std::string &text, const std::string &source) : s(text), src(source) { }

    void layer(std::map<std::string, Value> &meta, Prim &root) {
        if (s.compare(0, 5, "#usda") != 0)
            fail("not a .usda layer (no #usda header)");
        while (i < s.size() && s[i] != '\n')
            ++i;
        ws();
        if (peek('('))
            metadata(meta, nullptr);
        root.path = "/";
        while (true) {
            ws();
            if (i >= s.size())
                break;
            prim(root);
        }
    }

private:
    const std::string &s;
    std::string src;
    size_t i = 0;
    int depth = 0;    /* nesting of values and prims: bounded (stack) */

    struct Nest {
        Parser &p;
        explicit Nest(Parser &p) : p(p) {
            if (++p.depth > 200)
                p.fail("values or prims nested too deeply");
        }
        ~Nest() { --p.depth; }
    };

    [[noreturn]] void fail(const std::string &msg) const {
        size_t line = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k)
            line += s[k] == '\n';
        throw std::runtime_error("USD file \"" + src + "\", line " + std::to_string(line) + ": " + msg + "!");
    }

    /* Spaces, newlines and comments */
    void ws() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else if (c == '#') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
            } else {
                break;
            }
        }
    }

    bool peek(char c) {
        ws();
        return i < s.size() && s[i] == c;
    }

    void expect(char c) {
        if (!peek(c))
            fail(std::string("expected '") + c + "'");
        ++i;
    }

    bool accept(char c) {
        if (!peek(c))
            return false;
        ++i;
        return true;
    }

    static bool ident_start(char c) { return std::isalpha((unsigned char) c) || c == '_'; }
    static bool ident_char(char c) {
        return std::isalnum((unsigned char) c) || c == '_' || c == ':' || c == '.';
    }

    /* An identifier (namespaced with ':', field suffix with '.'), "" if none */
    std::string ident() {
        ws();
        if (i >= s.size() || !ident_start(s[i]))
            return std::string();
        const size_t b = i;
        while (i < s.size() && ident_char(s[i]))
            ++i;
        return s.substr(b, i - b);
    }

    /* The next word, without consuming it */
    std::string peek_ident() {
        const size_t save = i;
        std::string w = ident();
        i = save;
        return w;
    }

    std::string quoted() {
        ws();
        if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
            fail("expected a string");
        const char q = s[i];
        const bool triple = s.compare(i, 3, std::string(3, q)) == 0;
        i += triple ? 3 : 1;
        std::string out;
        while (true) {
            if (i >= s.size())
                fail("unterminated string");
            if (triple ? s.compare(i, 3, std::string(3, q)) == 0 : s[i] == q) {
                i += triple ? 3 : 1;
                break;
            }
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '0': c = '\0'; break;
                    default: c = e; break;
                }
            } else if (c == '\n' && !triple) {
                fail("newline in a string");
            }
            out += c;
        }
        return out;
    }

    /* @asset@ or @@@asset@@@ */
    std::string asset() {
        ws();
        if (s.compare(i, 3, "@@@") == 0) {
            const size_t e = s.find("@@@", i + 3);
            if (e == std::string::npos)
                fail("unterminated asset path");
            std::string a = s.substr(i + 3, e - i - 3);
            i = e + 3;
            return a;
        }
        const size_t e = s.find('@', i + 1);
        if (e == std::string::npos)
            fail("unterminated asset path");
        std::string a = s.substr(i + 1, e - i - 1);
        i = e + 1;
        return a;
    }

    /* <path> */
    std::string path() {
        expect('<');
        const size_t e = s.find('>', i);
        if (e == std::string::npos)
            fail("unterminated path");
        std::string p = s.substr(i, e - i);
        i = e + 1;
        return p;
    }

    bool number_start() {
        ws();
        if (i >= s.size())
            return false;
        const char c = s[i];
        return std::isdigit((unsigned char) c) || c == '-' || c == '+' || c == '.' ||
               s.compare(i, 3, "inf") == 0 || s.compare(i, 3, "nan") == 0;
    }

    double number() {
        ws();
        const char *b = s.c_str() + i;
        char *e = nullptr;
        double v = strtod(b, &e);
        if (e == b) {
            /* true / false as numbers */
            if (s.compare(i, 4, "true") == 0) {
                i += 4;
                return 1;
            }
            if (s.compare(i, 5, "false") == 0) {
                i += 5;
                return 0;
            }
            fail("expected a number");
        }
        i += (size_t) (e - b);
        return v;
    }

    /* Numbers of a value, flattened: 1, (1, 2), [(1, 2), (3, 4)], ((1,0),(0,1)) */
    void numbers(std::vector<double> &out) {
        Nest nest(*this);
        ws();
        if (i < s.size() && (s[i] == '[' || s[i] == '(')) {
            const char close = s[i] == '[' ? ']' : ')';
            ++i;
            while (true) {
                ws();
                if (i < s.size() && s[i] == close) {
                    ++i;
                    return;
                }
                numbers(out);
                ws();
                if (i < s.size() && s[i] == ',')
                    ++i;
                else if (!(i < s.size() && s[i] == close))
                    fail(std::string("expected ',' or '") + close + "'");
            }
        }
        out.push_back(number());
    }

    /* A balanced (...) / [...] / {...} block, skipped */
    void skip_block() {
        ws();
        const char open = s[i];
        const char close = open == '(' ? ')' : (open == '[' ? ']' : '}');
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"' || c == '\'') {
                quoted();
                continue;
            }
            if (c == '@') {
                asset();
                continue;
            }
            if (c == '#') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
                continue;
            }
            ++i;
            if (c == open)
                ++depth;
            else if (c == close && --depth == 0)
                return;
        }
        fail("unterminated block");
    }

    /* One item of an untyped value: string, asset (+path, + offset), path, number, word */
    void item(Value &v) {
        ws();
        if (i >= s.size())
            fail("expected a value");
        const char c = s[i];
        if (c == '"' || c == '\'') {
            v.kind = Value::Strings;
            v.strings.push_back(quoted());
        } else if (c == '@') {
            std::string a = asset();
            if (peek('<'))
                a += "<" + path() + ">";
            if (peek('('))
                skip_block();   /* layer offset of a reference / sublayer */
            v.kind = Value::Strings;
            v.strings.push_back(a);
        } else if (c == '<') {
            v.kind = Value::Strings;
            v.strings.push_back("<" + path() + ">");
        } else if (number_start()) {
            v.kind = Value::Numbers;
            v.numbers.push_back(number());
        } else if (ident_start(c)) {
            const std::string w = ident();
            if (w == "true" || w == "false") {
                v.kind = Value::Numbers;
                v.numbers.push_back(w == "true" ? 1 : 0);
            } else {
                v.kind = Value::Strings;
                v.strings.push_back(w);
            }
        } else {
            fail(std::string("unexpected '") + c + "'");
        }
    }

    /* An untyped value (metadata): None, item, [items], (numbers), {dictionary} */
    Value any() {
        Value v;
        ws();
        if (s.compare(i, 4, "None") == 0 && (i + 4 >= s.size() || !ident_char(s[i + 4]))) {
            i += 4;
            v.kind = Value::Blocked;
            return v;
        }
        if (peek('{')) {
            dictionary(v);
            return v;
        }
        if (peek('(')) {
            v.kind = Value::Numbers;
            numbers(v.numbers);
            v.tuple = (int) v.numbers.size();
            return v;
        }
        if (peek('[')) {
            ++i;
            v.array = true;
            while (!accept(']')) {
                if (peek('(')) {
                    /* a list of tuples */
                    v.kind = Value::Numbers;
                    const size_t before = v.numbers.size();
                    numbers(v.numbers);
                    v.tuple = (int) (v.numbers.size() - before);
                } else {
                    Value e;
                    item(e);
                    if (e.kind == Value::Numbers) {
                        v.kind = Value::Numbers;
                        v.numbers.push_back(e.numbers[0]);
                    } else {
                        v.kind = Value::Strings;
                        v.strings.push_back(e.strings[0]);
                    }
                }
                accept(',');
            }
            if (v.kind == Value::Empty)
                v.kind = Value::Strings;   /* [] */
            return v;
        }
        item(v);
        return v;
    }

    /* { type name = value ... } */
    void dictionary(Value &v) {
        Nest nest(*this);
        expect('{');
        v.kind = Value::Dictionary;
        while (!accept('}')) {
            std::string type = ident();
            if (type.empty())
                fail("expected a dictionary entry");
            if (accept('[')) {
                expect(']');
                type += "[]";
            }
            ws();
            std::string key = (i < s.size() && (s[i] == '"' || s[i] == '\'')) ? quoted() : ident();
            expect('=');
            v.dict[key] = type == "dictionary" ? any() : typed(type);
            accept(';');
        }
    }

    /* A value of a known type */
    Value typed(const std::string &type) {
        Value v;
        v.type = type;
        ws();
        if (s.compare(i, 4, "None") == 0 && (i + 4 >= s.size() || !ident_char(s[i + 4]))) {
            i += 4;
            v.kind = Value::Blocked;
            return v;
        }
        std::string base = type;
        if (base.size() > 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
            base.resize(base.size() - 2);
            v.array = true;
        }
        const int tuple = numeric_tuple(base);
        if (tuple > 0) {
            v.kind = Value::Numbers;
            v.tuple = tuple;
            numbers(v.numbers);
            return v;
        }
        if (string_type(base)) {
            Value a = any();
            a.type = type;
            a.array = v.array;
            return a;
        }
        if (base == "dictionary") {
            dictionary(v);
            return v;
        }
        /* opaque / unknown types: keep going */
        if (peek('[') || peek('(') || peek('{'))
            skip_block();
        else
            any();
        v.kind = Value::Unsupported;
        return v;
    }

    /* ( key = value ... ) of a layer, prim or property; 'doc' strings are kept */
    void metadata(std::map<std::string, Value> &meta, Prim *prim) {
        expect('(');
        while (!accept(')')) {
            ws();
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                Value doc;
                doc.kind = Value::Strings;
                doc.strings.push_back(quoted());
                meta["doc"] = doc;
                accept(';');
                continue;
            }
            std::string op = peek_ident();
            if (op == "prepend" || op == "append" || op == "add" || op == "delete" || op == "reorder") {
                ident();
            } else {
                op.clear();
            }
            const std::string key = ident();
            if (key.empty())
                fail("expected a metadata key");
            expect('=');
            Value v;
            if (key == "variants" && prim) {
                dictionary(v);
            } else {
                v = any();
            }
            if (!op.empty() || key == "references" || key == "payload" || key == "inherits" ||
                key == "specializes" || key == "apiSchemas" || key == "variantSets") {
                /* list op */
                Value &lop = meta[key];
                lop.kind = Value::ListOp;
                const std::vector<std::string> items = v.kind == Value::Blocked ? std::vector<std::string>() : v.strings;
                if (op.empty()) {
                    lop.isExplicit = true;
                    lop.explicitItems = items;
                } else if (op == "prepend") {
                    lop.prepended = items;
                } else if (op == "append" || op == "add") {
                    lop.appended = items;
                } else if (op == "delete") {
                    lop.deleted = items;
                }
            } else {
                meta[key] = v;
            }
            accept(';');
        }
    }

    void prim(Prim &parent) {
        const std::string spec = ident();
        auto child = std::unique_ptr<Prim>(new Prim());
        if (spec == "def")
            child->specifier = Specifier::Def;
        else if (spec == "over")
            child->specifier = Specifier::Over;
        else if (spec == "class")
            child->specifier = Specifier::Class;
        else
            fail("expected def, over or class, found \"" + spec + "\"");
        ws();
        if (i < s.size() && s[i] != '"' && s[i] != '\'')
            child->type = ident();
        child->name = quoted();
        child->path = child_path(parent.path, child->name);
        if (peek('('))
            metadata(child->meta, child.get());
        body(*child);
        parent.children.push_back(std::move(child));
    }

    /* { properties, children, variant sets } */
    void body(Prim &p) {
        Nest nest(*this);
        expect('{');
        while (!accept('}')) {
            const std::string w = peek_ident();
            if (w == "def" || w == "over" || w == "class") {
                prim(p);
            } else if (w == "variantSet") {
                ident();
                const std::string set = quoted();
                expect('=');
                expect('{');
                while (!accept('}')) {
                    const std::string name = quoted();
                    auto variant = std::unique_ptr<Prim>(new Prim());
                    variant->name = name;
                    variant->path = p.path + "{" + set + "=" + name + "}";
                    if (peek('('))
                        metadata(variant->meta, variant.get());
                    body(*variant);
                    p.variants[set][name] = std::move(variant);
                }
            } else if (w == "reorder") {
                ident();
                ident();
                expect('=');
                any();
            } else if (w.empty()) {
                fail("unexpected character in a prim");
            } else {
                property(p);
            }
            accept(';');
        }
    }

    void property(Prim &p) {
        std::string w = ident();
        if (w == "prepend" || w == "append" || w == "add" || w == "delete")
            w = ident();
        bool custom = false, uniform = false;
        if (w == "custom") {
            custom = true;
            w = ident();
        }
        if (w == "uniform" || w == "varying" || w == "config") {
            uniform = w == "uniform";
            w = ident();
        }
        if (w == "rel") {
            const std::string name = ident();
            Property &prop = slot(p, name);
            prop.relationship = true;
            prop.custom = custom;
            if (accept('=')) {
                Value v = any();
                prop.targets.clear();
                for (const std::string &t : v.strings)
                    prop.targets.push_back(t.size() > 1 && t[0] == '<' ? t.substr(1, t.size() - 2) : t);
            }
            if (peek('('))
                metadata(prop.meta, nullptr);
            return;
        }

        std::string type = w;
        if (accept('[')) {
            expect(']');
            type += "[]";
        }
        std::string name = ident();
        if (name.empty())
            fail("expected a property name");
        std::string field;
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos) {
            const std::string suffix = name.substr(dot + 1);
            if (suffix == "timeSamples" || suffix == "connect" || suffix == "spline" || suffix == "default") {
                field = suffix;
                name.resize(dot);
            }
        }
        Property &prop = slot(p, name);
        prop.type = type;
        prop.custom = prop.custom || custom;
        prop.uniform = prop.uniform || uniform;
        if (accept('=')) {
            if (field == "timeSamples") {
                expect('{');
                prop.hasTimeSamples = true;
                Value ts;
                ts.kind = Value::TimeSamples;
                ts.type = type;
                while (!accept('}')) {
                    const double t = number();
                    expect(':');
                    ts.samples.emplace_back(t, typed(type));
                    accept(',');
                }
                prop.value.type = type;
                prop.meta["__samples__"] = ts;
            } else if (field == "connect") {
                Value v = any();
                for (const std::string &t : v.strings)
                    prop.targets.push_back(t.size() > 1 && t[0] == '<' ? t.substr(1, t.size() - 2) : t);
            } else if (field == "spline") {
                skip_block();
            } else {
                prop.value = typed(type);
            }
        }
        if (peek('('))
            metadata(prop.meta, nullptr);
    }

    /* The property of that name, created if needed (".timeSamples" and
       ".connect" lines add to an existing one) */
    static Property &slot(Prim &p, const std::string &name) {
        if (Property *existing = p.property(name))
            return *existing;
        p.properties.emplace_back();
        p.properties.back().name = name;
        return p.properties.back();
    }
};

} // namespace

void parse_usda(const std::string &text, const std::string &source,
                std::map<std::string, Value> &meta, Prim &root) {
    Parser(text, source).layer(meta, root);
}

} // namespace usd
