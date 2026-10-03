/*
    usdstage.cpp: a USD file composed into a stage (see usdstage.h)

    Every prim of the stage has a prim index: a tree of nodes, each one the
    specs of one site (a prim path in one layer stack). The root node is the
    local layer stack; arcs (inherit, variant, reference, payload, specialize)
    add child nodes, ordered by kind, then deeper-authored first. The tree in
    pre-order is the strength order. The index of a child prim maps every
    node to its child site, then adds the arcs authored on the child itself.
*/

#include "usdstage.h"
#include <algorithm>
#include <functional>
#include <set>

namespace usd {

namespace {

enum Rank { RankRoot = 0, RankInherit = 1, RankVariant = 2, RankReference = 3, RankPayload = 4, RankSpecialize = 5 };

/* A layer stack: a root layer and its sublayers, strongest first */
struct Stack {
    std::vector<const Layer *> layers;
    const Layer *root = nullptr;
};

struct Spec {
    const Layer *layer;
    const Prim *prim;
};

struct Node {
    const Stack *stack = nullptr;
    std::vector<Spec> specs;          /* strongest first */
    std::string src, dst;             /* site path <-> stage path */
    std::string mapSrc = "/", mapDst = "/";  /* the arc root: what maps the targets into the stage */
    int rank = RankRoot;
    int depth = 0;                    /* namespace depth of the prim that authored the arc */
    std::vector<Node> arcs;           /* in strength order */
};

std::string slashes(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

bool is_absolute(const std::string &p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
}

/* "a/./b/../c" -> "a/c" */
std::string normalize(const std::string &path) {
    const std::string p = slashes(path);
    std::vector<std::string> parts;
    size_t pos = 0;
    const bool lead = !p.empty() && p[0] == '/';
    while (pos <= p.size()) {
        const size_t next = p.find('/', pos);
        const std::string part = p.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else
                parts.push_back(part);
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    std::string out = lead ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k)
        out += (k ? "/" : "") + parts[k];
    return out;
}

std::string parent_dir(const std::string &file) {
    const std::string p = slashes(file);
    const size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

const Value *find_meta(const Prim &p, std::initializer_list<const char *> keys) {
    for (const char *k : keys)
        if (const Value *v = p.metadata(k))
            return v;
    return nullptr;
}

class Composer {
public:
    explicit Composer(const std::string &filename) {
        mRootFile = filename;
    }

    std::shared_ptr<const Layer> run() {
        const Stack *root = stack_of(mRootFile);
        if (!root)
            throw std::runtime_error("Unable to open USD file \"" + mRootFile + "\"!");
        std::shared_ptr<const Layer> rootLayer = mLayers.at(key_of(mRootFile));
        if (root->layers.size() == 1 && !has_arcs(rootLayer->root))
            return rootLayer;

        std::shared_ptr<Layer> stage(new Layer(rootLayer->filename(), rootLayer->format()));
        stage->meta = rootLayer->meta;
        Node tree;
        tree.stack = root;
        tree.src = tree.dst = "/";
        for (const Layer *l : root->layers)
            tree.specs.push_back(Spec { l, &l->root });
        build_children(stage->root, tree, "/", 0);
        for (auto &kv : mLayers)
            stage->sources.push_back(kv.second);
        for (const std::string &w : mWarnings)
            cout << "Warning: " << w << endl;
        return stage;
    }

private:
    /* ---- layers and layer stacks ---- */

    static std::string key_of(const std::string &file) { return str_tolower(normalize(file)); }

    const Layer *open(const std::string &file) {
        const std::string key = key_of(file);
        auto it = mLayers.find(key);
        if (it != mLayers.end())
            return it->second.get();
        if (mLayers.size() > 10000)
            throw std::runtime_error("USD stage \"" + mRootFile + "\": too many layers");
        std::shared_ptr<const Layer> layer;
        try {
            layer.reset(new Layer(file));
        } catch (const std::exception &e) {
            if (mLayers.empty())
                throw;
            warn(e.what());
            return nullptr;
        }
        mLayers[key] = layer;
        return layer.get();
    }

    /* An asset path of a layer, as a file path (empty if it cannot be opened) */
    std::string resolve(const std::string &asset, const Layer *from) {
        if (asset.empty())
            return std::string();
        if (asset.find('[') != std::string::npos) {
            warn("\"" + asset + "\" (in \"" + from->filename() + "\"): files inside packages are not read");
            return std::string();
        }
        if (is_absolute(asset))
            return normalize(asset);
        if (from->format() == "usdz") {
            warn("\"" + asset + "\" (in the package \"" + from->filename() + "\"): files inside packages are not read");
            return std::string();
        }
        return normalize(parent_dir(from->filename()) + asset);
    }

    const Stack *stack_of(const std::string &file) {
        const std::string key = key_of(file);
        auto it = mStacks.find(key);
        if (it != mStacks.end())
            return it->second.get();
        const Layer *root = open(file);
        if (!root)
            return nullptr;
        std::unique_ptr<Stack> stack(new Stack());
        stack->root = root;
        std::set<const Layer *> seen;
        std::function<void(const Layer *, int)> add = [&](const Layer *l, int depth) {
            if (!seen.insert(l).second || depth > 64)
                return;
            stack->layers.push_back(l);
            auto sub = l->meta.find("subLayers");
            if (sub == l->meta.end())
                return;
            for (const std::string &s : sub->second.strings) {
                const std::string path = resolve(s, l);
                if (path.empty())
                    continue;
                if (const Layer *child = open(path))
                    add(child, depth + 1);
            }
        };
        add(root, 0);
        const Stack *result = stack.get();
        mStacks[key] = std::move(stack);
        return result;
    }

    void warn(const std::string &w) {
        if (std::find(mWarnings.begin(), mWarnings.end(), w) == mWarnings.end() && mWarnings.size() < 100)
            mWarnings.push_back(w);
    }

    /* Whether a layer needs composing: arcs or variant selections anywhere */
    static bool has_arcs(const Prim &p) {
        if (find_meta(p, { "references", "payload", "payloads", "inherits", "inheritPaths", "specializes" }))
            return true;
        if (!p.variants.empty() && find_meta(p, { "variants", "variantSelection" }))
            return true;
        for (const auto &c : p.children)
            if (has_arcs(*c))
                return true;
        for (const auto &set : p.variants)
            for (const auto &v : set.second)
                if (has_arcs(*v.second))
                    return true;
        return false;
    }

    /* ---- prim index ---- */

    static void flatten(const Node &n, std::vector<std::pair<Spec, const Node *>> &out) {
        for (const Spec &s : n.specs)
            out.emplace_back(s, &n);
        for (const Node &a : n.arcs)
            flatten(a, out);
    }

    /* The items of a list op over the specs of a node, strongest first:
       an explicit list stops the weaker ones; prepends of the stronger
       specs come first, their appends last. Items with their spec. */
    static std::vector<std::pair<std::string, const Spec *>> list_items(const Node &n,
                                                                       std::initializer_list<const char *> keys) {
        typedef std::pair<std::string, const Spec *> Item;
        std::vector<Item> pre, base, app;
        std::set<std::string> deleted;
        for (const Spec &s : n.specs) {
            const Value *v = find_meta(*s.prim, keys);
            if (!v)
                continue;
            auto add = [&](std::vector<Item> &to, const std::vector<std::string> &items) {
                for (const std::string &i : items)
                    if (!deleted.count(i))
                        to.emplace_back(i, &s);
            };
            if (v->kind == Value::ListOp) {
                std::vector<Item> p, a;
                add(p, v->prepended);
                add(a, v->appended);
                pre.insert(pre.end(), p.begin(), p.end());
                app.insert(app.begin(), a.begin(), a.end());
                if (v->isExplicit) {
                    add(base, v->explicitItems);
                    break;
                }
                deleted.insert(v->deleted.begin(), v->deleted.end());
            } else if (v->kind == Value::Strings) {
                add(base, v->strings);
                break;
            }
        }
        std::vector<Item> out;
        std::set<std::string> seen;
        for (const std::vector<Item> *list : { &pre, &base, &app })
            for (const Item &i : *list)
                if (seen.insert(i.first).second)
                    out.push_back(i);
        return out;
    }

    /* The specs of a site: the prim at 'path' in every layer of a stack */
    static std::vector<Spec> site(const Stack *stack, const std::string &path) {
        std::vector<Spec> specs;
        for (const Layer *l : stack->layers)
            if (const Prim *p = l->prim(path))
                specs.push_back(Spec { l, p });
        return specs;
    }

    std::string selection(const std::string &set) const {
        std::vector<std::pair<Spec, const Node *>> all;
        if (mTree)
            flatten(*mTree, all);
        for (const Node *n : mPending)
            for (const Spec &s : n->specs)
                all.emplace_back(s, n);
        for (const auto &e : all) {
            const Value *v = find_meta(*e.first.prim, { "variants", "variantSelection" });
            if (!v || v->kind != Value::Dictionary)
                continue;
            auto it = v->dict.find(set);
            if (it != v->dict.end() && !it->second.str().empty())
                return it->second.str();
        }
        return std::string();
    }

    /* Adds to 'n' the arcs authored on its specs (prim at stage path 'dst',
       namespace depth 'depth'), each one expanded in turn */
    void expand(Node &n, int depth, int nesting) {
        if (n.specs.empty())
            return;
        if (nesting > 32 || ++mNodes > 50000000) {
            warn("composition arcs nested too deeply at \"" + n.dst + "\" (cycle?)");
            return;
        }
        mPending.push_back(&n);
        std::vector<Node> arcs;
        auto arc = [&](const Stack *stack, const std::string &src, int rank) {
            Node a;
            a.stack = stack;
            a.src = a.mapSrc = src;
            a.dst = a.mapDst = n.dst;
            a.rank = rank;
            a.depth = depth;
            a.specs = site(stack, src);
            arcs.push_back(std::move(a));
        };
        /* "asset<path>" -> "path"; a bare path (inherits in .usdc) as is.
           Paths are in the namespace of the layer stack that holds them */
        auto path_of = [](const std::string &item) {
            const size_t open = item.find('<');
            if (open == std::string::npos)
                return item.empty() || item[0] != '/' ? std::string() : item;
            const size_t close = item.rfind('>');
            return close == std::string::npos || close < open ? std::string() : item.substr(open + 1, close - open - 1);
        };

        for (const auto &item : list_items(n, { "inherits", "inheritPaths" }))
            if (!path_of(item.first).empty())
                arc(n.stack, path_of(item.first), RankInherit);

        /* variants: every set of the specs, its selection from the whole index */
        std::set<std::string> sets;
        for (const Spec &s : n.specs)
            for (const auto &vs : s.prim->variants) {
                if (!sets.insert(vs.first).second)
                    continue;
                const std::string sel = selection(vs.first);
                if (sel.empty())
                    continue;
                Node v;
                v.stack = n.stack;
                v.src = n.src;
                v.dst = n.dst;
                v.mapSrc = n.mapSrc;
                v.mapDst = n.mapDst;
                v.rank = RankVariant;
                v.depth = depth;
                for (const Spec &t : n.specs) {
                    auto it = t.prim->variants.find(vs.first);
                    if (it == t.prim->variants.end())
                        continue;
                    auto jt = it->second.find(sel);
                    if (jt != it->second.end())
                        v.specs.push_back(Spec { t.layer, jt->second.get() });
                }
                arcs.push_back(std::move(v));
            }

        for (int rank : { (int) RankReference, (int) RankPayload }) {
            const auto items = rank == RankReference ? list_items(n, { "references" })
                                                     : list_items(n, { "payload", "payloads" });
            for (const auto &item : items) {
                const std::string &text = item.first;
                const size_t open = text.find('<');
                const std::string asset = text.substr(0, open);
                std::string prim = open == std::string::npos ? std::string() : path_of(text);
                const Stack *stack = n.stack;
                if (!asset.empty()) {
                    const std::string file = resolve(asset, item.second->layer);
                    stack = file.empty() ? nullptr : stack_of(file);
                    if (!stack) {
                        warn("\"" + asset + "\", referenced by \"" + n.dst + "\", cannot be opened");
                        continue;
                    }
                }
                if (prim.empty()) {
                    auto dp = stack->root->meta.find("defaultPrim");
                    if (dp == stack->root->meta.end() || dp->second.str().empty()) {
                        warn("\"" + stack->root->filename() + "\", referenced by \"" + n.dst + "\", has no defaultPrim");
                        continue;
                    }
                    prim = "/" + dp->second.str();
                }
                arc(stack, prim, rank);
                if (arcs.back().specs.empty())
                    warn("\"" + prim + "\" not found in \"" + stack->root->filename() + "\" (referenced by \"" + n.dst + "\")");
            }
        }

        for (const auto &item : list_items(n, { "specializes" }))
            if (!path_of(item.first).empty())
                arc(n.stack, path_of(item.first), RankSpecialize);

        for (Node &a : arcs)
            expand(a, depth, nesting + 1);
        mPending.pop_back();

        /* by kind, then the arcs authored deeper (on this prim) first */
        for (Node &a : arcs) {
            auto at = std::find_if(n.arcs.begin(), n.arcs.end(), [&](const Node &b) {
                return b.rank > a.rank || (b.rank == a.rank && b.depth < a.depth);
            });
            n.arcs.insert(at, std::move(a));
        }
    }

    /* The node of a child site; false when nothing is left below it */
    static bool child_node(const Node &n, const std::string &name, Node &c) {
        c.stack = n.stack;
        c.rank = n.rank;
        c.depth = n.depth;
        c.src = child_path(n.src, name);
        c.dst = child_path(n.dst, name);
        c.mapSrc = n.mapSrc;
        c.mapDst = n.mapDst;
        for (const Spec &s : n.specs)
            if (const Prim *p = s.prim->child(name))
                c.specs.push_back(Spec { s.layer, p });
        for (const Node &a : n.arcs) {
            Node ca;
            if (child_node(a, name, ca))
                c.arcs.push_back(std::move(ca));
        }
        return !c.specs.empty() || !c.arcs.empty();
    }

    /* Expands the arcs authored on the specs of a new prim index */
    void expand_tree(Node &n, int depth) {
        const size_t count = n.arcs.size();
        for (size_t k = 0; k < count; ++k)
            expand_tree(n.arcs[k], depth);
        expand(n, depth, 0);
    }

    /* A target path of a site, in the stage namespace (outside the arc root:
       unchanged) */
    static std::string map_path(const std::string &t, const Node &n) {
        const std::string &from = n.mapSrc, &to = n.mapDst;
        if (from == to || from == "/")
            return t;
        if (t == from)
            return to;
        if (t.size() > from.size() && t.compare(0, from.size(), from) == 0 &&
            (t[from.size()] == '/' || t[from.size()] == '.'))
            return to + t.substr(from.size());
        return t;
    }

    /* The composed prim of an index */
    void build_prim(Prim &out, const Node &tree) {
        std::vector<std::pair<Spec, const Node *>> flat;
        flatten(tree, flat);
        out.specifier = Specifier::Over;
        bool specified = false;
        std::map<std::string, std::vector<std::pair<const Property *, std::pair<const Layer *, const Node *>>>> props;
        std::vector<std::string> propOrder;
        for (const auto &e : flat) {
            const Prim &p = *e.first.prim;
            if (!specified && p.specifier != Specifier::Over) {
                out.specifier = p.specifier;
                specified = true;
            }
            if (out.type.empty())
                out.type = p.type;
            for (const auto &kv : p.meta)
                out.meta.insert(kv);
            for (const Property &q : p.properties) {
                auto &list = props[q.name];
                if (list.empty())
                    propOrder.push_back(q.name);
                list.emplace_back(&q, std::make_pair(e.first.layer, e.second));
            }
        }
        for (const std::string &name : propOrder) {
            const auto &list = props[name];
            const Property &strongest = *list.front().first;
            Property q;
            q.name = name;
            q.relationship = strongest.relationship;
            q.custom = strongest.custom;
            q.uniform = strongest.uniform;
            const std::pair<const Property *, std::pair<const Layer *, const Node *>> *valued = nullptr, *targeted = nullptr;
            for (const auto &e : list) {
                const Property &p = *e.first;
                if (q.type.empty())
                    q.type = p.type;
                for (const auto &kv : p.meta)
                    if (kv.first != "__samples__")
                        q.meta.insert(kv);
                if (!valued && (p.crateDefault != 0 || p.value.kind != Value::Empty || p.hasTimeSamples))
                    valued = &e;
                if (!targeted && !p.targets.empty())
                    targeted = &e;
            }
            const auto &from = valued ? *valued : list.front();
            q.origin = from.first;
            q.owner = from.second.first;
            q.hasTimeSamples = from.first->hasTimeSamples;
            if (targeted)
                for (const std::string &t : targeted->first->targets)
                    q.targets.push_back(map_path(t, *targeted->second.second));
            out.properties.push_back(std::move(q));
        }
    }

    void build_children(Prim &out, const Node &tree, const std::string &path, int depth) {
        if (depth > 1000)
            throw std::runtime_error("USD stage \"" + mRootFile + "\": prims nested too deeply");
        /* child names: weaker sites first, the stronger ones add theirs */
        std::vector<std::pair<Spec, const Node *>> flat;
        flatten(tree, flat);
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (auto it = flat.rbegin(); it != flat.rend(); ++it)
            for (const auto &c : it->first.prim->children)
                if (seen.insert(c->name).second)
                    names.push_back(c->name);
        for (const std::string &name : names) {
            Node child;
            if (!child_node(tree, name, child))
                continue;
            mTree = &child;
            expand_tree(child, depth + 1);
            mTree = nullptr;
            std::unique_ptr<Prim> p(new Prim());
            p->name = name;
            p->path = child_path(path, name);
            build_prim(*p, child);
            /* an inactive prim has no children on the stage */
            const Value *active = p->metadata("active");
            if (!(active && active->kind == Value::Numbers && active->num() == 0))
                build_children(*p, child, p->path, depth + 1);
            out.children.push_back(std::move(p));
        }
    }

    std::string mRootFile;
    std::map<std::string, std::shared_ptr<const Layer>> mLayers;
    std::map<std::string, std::unique_ptr<Stack>> mStacks;
    std::vector<std::string> mWarnings;
    const Node *mTree = nullptr;
    std::vector<const Node *> mPending;
    size_t mNodes = 0;
};

} // namespace

std::shared_ptr<const Layer> open_stage(const std::string &filename) {
    return Composer(filename).run();
}

} // namespace usd
