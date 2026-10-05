/*
    outliner.cpp: the Outliner (see outliner.h)
*/

#include "outliner.h"
#include "project.h"
#include <algorithm>

using nanogui::Color;
using Eigen::Vector2i;

namespace {

const int RowHeight = 22, HeaderHeight = 24, Indent = 14, Scrollbar = 8;
const int StateWidth = 64, TargetWidth = 56, FacesWidth = 74;
const Color Text(245, 245, 245, 255), Muted(136, 136, 136, 255), Dim(90, 90, 90, 255), Orange(251, 146, 60, 255);
const Color Ok(74, 222, 128, 255), Warn(250, 204, 21, 255), Error(248, 113, 113, 255);
const Color Border(51, 51, 51, 255), Surface(37, 37, 37, 255), Code(15, 15, 15, 255);

Color state_color(ObjectState s) {
    switch (s) {
        case ObjectState::Done: return Ok;
        case ObjectState::Stale: return Warn;
        case ObjectState::Failed: return Error;
        case ObjectState::Skipped: return Orange;
        default: return Muted;
    }
}

std::string thousands(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = (int) s.size() - 3; i > 0; i -= 3)
        s.insert((size_t) i, ",");
    return s;
}

/* a filter without wildcards matches any path that contains it */
/* a small triangle (the font has no arrow glyphs): pointing down or right */
void arrow(NVGcontext *ctx, float x, float y, bool down, const Color &c) {
    nvgBeginPath(ctx);
    if (down) {
        nvgMoveTo(ctx, x, y - 2.5f);
        nvgLineTo(ctx, x + 8, y - 2.5f);
        nvgLineTo(ctx, x + 4, y + 2.5f);
    } else {
        nvgMoveTo(ctx, x + 1.5f, y - 4);
        nvgLineTo(ctx, x + 6.5f, y);
        nvgLineTo(ctx, x + 1.5f, y + 4);
    }
    nvgClosePath(ctx);
    nvgFillColor(ctx, c);
    nvgFill(ctx);
}

bool matches(const std::string &filter, const std::string &path) {
    if (filter.empty())
        return true;
    const std::string f = str_tolower(filter), p = str_tolower(path);
    if (f.find_first_of("*?") == std::string::npos)
        return p.find(f) != std::string::npos;
    return abc::glob_match(f, p) || abc::glob_match("*" + f, p) || abc::glob_match("*/" + f, p);
}

} // namespace

OutlinerView::OutlinerView(nanogui::Widget *parent) : Widget(parent) { }

void OutlinerView::setProject(Project *project, std::mutex *lock) {
    mProject = project;
    mLock = lock;
    mSelected.clear();
    mScroll = 0;
    mAnchor = -1;
    build();
}

void OutlinerView::setFilter(const std::string &filter) {
    mFilter = filter;
    mScroll = 0;
    layoutRows();
}

void OutlinerView::setSort(const std::string &key, bool descending) {
    mSort = key == "name" ? 1 : key == "faces" ? 2 : key == "state" ? 3 : 0;
    mSortDesc = descending;
    layoutRows();
}

std::string OutlinerView::sortKey() const {
    return mSort == 1 ? "name" : mSort == 2 ? "faces" : mSort == 3 ? "state" : "scene";
}

void OutlinerView::setSelection(const std::set<int> &s) {
    mSelected = s;
}

void OutlinerView::reveal(int object) {
    int node = -1;
    for (size_t k = 1; k < mNodes.size(); ++k)
        if (mNodes[k].object == object) {
            node = (int) k;
            break;
        }
    if (node < 0)
        return;
    bool expanded = false;
    for (int p = mNodes[(size_t) node].parent; p > 0; p = mNodes[(size_t) p].parent)
        if (!mNodes[(size_t) p].expanded) {
            mNodes[(size_t) p].expanded = true;
            expanded = true;
        }
    if (expanded)
        layoutRows();
    for (size_t r = 0; r < mRows.size(); ++r) {
        if (mRows[r].first != node)
            continue;
        /* out of view: centred */
        const int top = (int) r * RowHeight, view = height() - HeaderHeight;
        if (top < mScroll || top + RowHeight > mScroll + view)
            mScroll = top - (view - RowHeight) / 2;
        clampScroll();
        mAnchor = (int) r;
        break;
    }
}

void OutlinerView::refresh() {
    layoutRows();
}

/* The tree of the mesh paths: a node per path element */
void OutlinerView::build() {
    mNodes.assign(1, Node());
    if (!mProject) {
        mRows.clear();
        return;
    }
    std::map<std::string, int> byPath;
    for (size_t i = 0; i < mProject->objects.size(); ++i) {
        const std::string &path = mProject->objects[i].mesh.path;
        int parent = 0;
        std::string prefix;
        size_t pos = path[0] == '/' ? 1 : 0;
        while (pos <= path.size()) {
            const size_t next = path.find('/', pos);
            const std::string name = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            prefix += "/" + name;
            const bool leaf = next == std::string::npos;
            auto it = byPath.find(prefix);
            int node;
            if (it != byPath.end() && !leaf) {
                node = it->second;
            } else {
                node = (int) mNodes.size();
                mNodes.push_back(Node());
                mNodes.back().name = name;
                mNodes.back().parent = parent;
                mNodes[(size_t) parent].children.push_back(node);
                if (!leaf)
                    byPath[prefix] = node;
            }
            if (leaf) {
                mNodes[(size_t) node].object = (int) i;
                break;
            }
            parent = node;
            pos = next + 1;
        }
    }
    layoutRows();
}

void OutlinerView::collect(int node, std::vector<int> &objects) const {
    const Node &n = mNodes[(size_t) node];
    if (n.object >= 0)
        objects.push_back(n.object);
    for (int c : n.children)
        collect(c, objects);
}

void OutlinerView::layoutRows() {
    mRows.clear();
    if (!mProject)
        return;
    /* which nodes the filter keeps: a mesh that matches and its groups */
    std::vector<char> keep(mNodes.size(), 0);
    std::vector<uint64_t> faces(mNodes.size(), 0);
    std::vector<int> worst(mNodes.size(), 0);
    for (size_t k = 1; k < mNodes.size(); ++k) {
        const Node &n = mNodes[k];
        if (n.object < 0)
            continue;
        const ProjectObject &o = mProject->objects[(size_t) n.object];
        const bool ok = matches(mFilter, o.mesh.path);
        for (int p = (int) k; p > 0; p = mNodes[(size_t) p].parent) {
            if (ok)
                keep[(size_t) p] = 1;
            faces[(size_t) p] += o.mesh.faces;
            worst[(size_t) p] = std::max(worst[(size_t) p], (int) o.state);
        }
    }
    auto less = [&](int a, int b) {
        const Node &na = mNodes[(size_t) a], &nb = mNodes[(size_t) b];
        switch (mSort) {
            case 1: return str_tolower(na.name) < str_tolower(nb.name);
            case 2: return faces[(size_t) a] < faces[(size_t) b];
            case 3: return worst[(size_t) a] < worst[(size_t) b];
            default: return a < b;
        }
    };
    std::function<void(int, int)> visit = [&](int node, int depth) {
        std::vector<int> kids;
        for (int c : mNodes[(size_t) node].children)
            if (keep[(size_t) c])
                kids.push_back(c);
        std::stable_sort(kids.begin(), kids.end(), [&](int a, int b) {
            return mSortDesc && mSort != 0 ? less(b, a) : less(a, b);
        });
        for (int c : kids) {
            mRows.emplace_back(c, depth);
            if (mNodes[(size_t) c].expanded || !mFilter.empty())
                visit(c, depth + 1);
        }
    };
    visit(0, 0);
    clampScroll();
}

void OutlinerView::clampScroll() {
    const int content = HeaderHeight + (int) mRows.size() * RowHeight;
    mScroll = std::max(0, std::min(mScroll, std::max(0, content - height())));
}

int OutlinerView::rowAt(const Vector2i &p) const {
    const int y = p.y() - mPos.y() - HeaderHeight + mScroll;
    if (p.y() - mPos.y() < HeaderHeight || y < 0)
        return -1;
    const int r = y / RowHeight;
    return r < (int) mRows.size() ? r : -1;
}

void OutlinerView::draw(NVGcontext *ctx) {
    std::unique_lock<std::mutex> guard;
    if (mLock)
        guard = std::unique_lock<std::mutex>(*mLock);
    const int x0 = mPos.x(), y0 = mPos.y(), w = width(), h = height();
    nvgSave(ctx);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x0, y0, w, h, 3);
    nvgFillColor(ctx, Code);
    nvgFill(ctx);
    nvgStrokeColor(ctx, Border);
    nvgStrokeWidth(ctx, 1);
    nvgStroke(ctx);
    nvgIntersectScissor(ctx, x0, y0, w, h);
    nvgFontFace(ctx, "sans");

    const int content = HeaderHeight + (int) mRows.size() * RowHeight;
    const bool bar = content > h;
    const int inner = w - (bar ? Scrollbar + 2 : 0);
    const int stateX = x0 + inner - StateWidth, targetX = stateX - TargetWidth, facesX = targetX - FacesWidth;

    /* rows */
    if (!mProject) {
        nvgFontSize(ctx, 15);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, Muted);
        nvgText(ctx, x0 + w * 0.5f, y0 + h * 0.4f, "Open a scene (.abc, .obj, .usd)", nullptr);
        nvgText(ctx, x0 + w * 0.5f, y0 + h * 0.4f + 22, "to list its meshes here", nullptr);
    } else {
        const int first = std::max(0, mScroll / RowHeight), last = std::min((int) mRows.size(),
                                                                           (mScroll + h) / RowHeight + 1);
        nvgFontSize(ctx, 14);
        for (int r = first; r < last; ++r) {
            const Node &n = mNodes[(size_t) mRows[(size_t) r].first];
            const int depth = mRows[(size_t) r].second;
            const float y = (float) (y0 + HeaderHeight + r * RowHeight - mScroll);
            std::vector<int> objs;
            collect(mRows[(size_t) r].first, objs);
            bool selected = !objs.empty(), checked = !objs.empty(), some = false, unfit = n.object >= 0;
            uint64_t faces = 0;
            for (int o : objs) {
                const ProjectObject &po = mProject->objects[(size_t) o];
                selected = selected && mSelected.count(o);
                checked = checked && po.checked;
                some = some || po.checked;
                faces += po.mesh.faces;
                if (n.object >= 0)
                    unfit = mProject->unfit(po);
            }
            if (selected || r == mHover) {
                nvgBeginPath(ctx);
                nvgRect(ctx, x0 + 1, y, inner - 2, RowHeight);
                nvgFillColor(ctx, selected ? Color(251, 146, 60, 60) : Color(255, 255, 255, 12));
                nvgFill(ctx);
            }
            float x = (float) (x0 + 6 + depth * Indent);
            /* expand arrow */
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            if (n.object < 0)
                arrow(ctx, x, y + RowHeight * 0.5f, n.expanded || !mFilter.empty(), Muted);
            x += 12;
            /* check box */
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y + 5, 12, 12, 2);
            nvgFillColor(ctx, checked ? Orange : Surface);
            nvgFill(ctx);
            nvgStrokeColor(ctx, unfit ? Dim : Color(110, 110, 110, 255));
            nvgStroke(ctx);
            if (!checked && some) {
                nvgBeginPath(ctx);
                nvgRect(ctx, x + 3, y + 10, 6, 2);
                nvgFillColor(ctx, Orange);
                nvgFill(ctx);
            }
            x += 18;
            /* name */
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x, y, facesX - x - 4, RowHeight);
            nvgFillColor(ctx, unfit ? Dim : (n.object < 0 ? Color(200, 200, 200, 255) : Text));
            nvgText(ctx, x, y + RowHeight * 0.5f, n.name.c_str(), nullptr);
            if (n.object >= 0) {
                /* an instanced mesh: how many times it appears (remeshed once for all) */
                const SceneMesh &m = mProject->objects[(size_t) n.object].mesh;
                if (m.instanced && !m.nested && m.instances > 1) {
                    float b[4];
                    nvgTextBounds(ctx, x, y, n.name.c_str(), nullptr, b);
                    nvgFillColor(ctx, Muted);
                    nvgText(ctx, b[2] + 6, y + RowHeight * 0.5f, ("x" + std::to_string(m.instances)).c_str(), nullptr);
                }
            }
            nvgRestore(ctx);
            /* faces */
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFillColor(ctx, Muted);
            nvgText(ctx, targetX - 8, y + RowHeight * 0.5f, thousands(faces).c_str(), nullptr);
            if (n.object >= 0) {
                const ProjectObject &o = mProject->objects[(size_t) n.object];
                /* target */
                const FaceTarget t = mProject->target_of(o);
                nvgFillColor(ctx, o.target.valid() ? Orange : Muted);
                nvgText(ctx, stateX - 8, y + RowHeight * 0.5f, t.valid() ? t.text.c_str() : "-", nullptr);
                /* state */
                nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                const Color c = unfit ? Dim : state_color(o.state);
                nvgBeginPath(ctx);
                nvgCircle(ctx, stateX + 5, y + RowHeight * 0.5f, 3.5f);
                nvgFillColor(ctx, c);
                nvgFill(ctx);
                nvgFillColor(ctx, c);
                nvgText(ctx, stateX + 13, y + RowHeight * 0.5f,
                        unfit ? (o.mesh.nested ? "nested" : o.mesh.animated ? "anim." : o.mesh.purpose.c_str())
                              : state_name(o.state), nullptr);
            }
        }
    }

    /* header */
    nvgBeginPath(ctx);
    nvgRect(ctx, x0, y0, w, HeaderHeight);
    nvgFillColor(ctx, Surface);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, x0, y0 + HeaderHeight - 0.5f);
    nvgLineTo(ctx, x0 + w, y0 + HeaderHeight - 0.5f);
    nvgStrokeColor(ctx, Border);
    nvgStroke(ctx);
    nvgFontSize(ctx, 13);
    auto title = [&](const char *t, float x, int align, int key) {
        nvgTextAlign(ctx, align | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, mSort == key ? Orange : Muted);
        float bounds[4];
        nvgTextBounds(ctx, x, y0 + HeaderHeight * 0.5f, t, nullptr, bounds);
        nvgText(ctx, x, y0 + HeaderHeight * 0.5f, t, nullptr);
        if (mSort == key) {
            const float ax = bounds[2] + 4, ay = y0 + HeaderHeight * 0.5f;
            nvgBeginPath(ctx);
            if (mSortDesc) {
                nvgMoveTo(ctx, ax, ay - 2);
                nvgLineTo(ctx, ax + 7, ay - 2);
                nvgLineTo(ctx, ax + 3.5f, ay + 2.5f);
            } else {
                nvgMoveTo(ctx, ax, ay + 2);
                nvgLineTo(ctx, ax + 7, ay + 2);
                nvgLineTo(ctx, ax + 3.5f, ay - 2.5f);
            }
            nvgClosePath(ctx);
            nvgFillColor(ctx, Orange);
            nvgFill(ctx);
        }
    };
    title("Name", (float) (x0 + 36), NVG_ALIGN_LEFT, 1);
    title("Faces", (float) (targetX - 8), NVG_ALIGN_RIGHT, 2);
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(ctx, Muted);
    nvgText(ctx, stateX - 8, y0 + HeaderHeight * 0.5f, "Target", nullptr);
    title("State", (float) (stateX + 4), NVG_ALIGN_LEFT, 3);

    /* scroll bar */
    if (bar) {
        const float track = (float) (h - HeaderHeight - 4);
        const float size = std::max(20.f, track * h / content);
        const float at = (track - size) * mScroll / std::max(1, content - h);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x0 + w - Scrollbar - 2, y0 + HeaderHeight + 2 + at, Scrollbar, size, 3);
        nvgFillColor(ctx, Color(110, 110, 110, 200));
        nvgFill(ctx);
    }
    nvgRestore(ctx);
}

bool OutlinerView::scrollEvent(const Vector2i &, const Eigen::Vector2f &rel) {
    mScroll -= (int) (rel.y() * RowHeight * 3);
    clampScroll();
    return true;
}

bool OutlinerView::mouseMotionEvent(const Vector2i &p, const Vector2i &, int, int) {
    mHover = rowAt(p);
    return true;
}

bool OutlinerView::mouseButtonEvent(const Vector2i &p, int button, bool down, int modifiers) {
    if (!down || button != GLFW_MOUSE_BUTTON_1 || !mProject)
        return true;
    std::unique_lock<std::mutex> guard;
    if (mLock)
        guard = std::unique_lock<std::mutex>(*mLock);
    /* header: sort */
    if (p.y() - mPos.y() < HeaderHeight) {
        const int w = width() - ((HeaderHeight + (int) mRows.size() * RowHeight > height()) ? Scrollbar + 2 : 0);
        const int x = p.x() - mPos.x(), stateX = w - StateWidth, targetX = stateX - TargetWidth;
        const int key = x >= stateX ? 3 : x >= targetX - FacesWidth && x < targetX ? 2 : x < targetX - FacesWidth ? 1 : -1;
        if (key > 0) {
            if (mSort == key)
                mSortDesc = !mSortDesc;
            else {
                mSort = key;
                mSortDesc = key == 2;
            }
            layoutRows();
        }
        return true;
    }
    const int r = rowAt(p);
    if (r < 0)
        return true;
    Node &n = mNodes[(size_t) mRows[(size_t) r].first];
    const int depth = mRows[(size_t) r].second;
    const int x = p.x() - mPos.x() - 6 - depth * Indent;
    std::vector<int> objs;
    collect(mRows[(size_t) r].first, objs);
    if (x >= 0 && x < 12 && n.object < 0) {
        n.expanded = !n.expanded;
        layoutRows();
        return true;
    }
    if (x >= 12 && x < 28) {
        bool all = true;
        for (int o : objs)
            all = all && (mProject->objects[(size_t) o].checked || mProject->unfit(mProject->objects[(size_t) o]));
        for (int o : objs)
            if (!mProject->unfit(mProject->objects[(size_t) o]))
                mProject->objects[(size_t) o].checked = !all;
        guard = std::unique_lock<std::mutex>();
        if (changeCallback)
            changeCallback();
        return true;
    }
    const double now = glfwGetTime();
    const bool twice = r == mLastClickRow && now - mLastClick < 0.35;
    mLastClick = now;
    mLastClickRow = r;
    if (modifiers & GLFW_MOD_SHIFT && mAnchor >= 0) {
        if (!(modifiers & GLFW_MOD_CONTROL))
            mSelected.clear();
        for (int k = std::min(mAnchor, r); k <= std::max(mAnchor, r) && k < (int) mRows.size(); ++k)
            collect(mRows[(size_t) k].first, objs);
        mSelected.insert(objs.begin(), objs.end());
    } else if (modifiers & GLFW_MOD_CONTROL) {
        bool all = true;
        for (int o : objs)
            all = all && mSelected.count(o);
        for (int o : objs) {
            if (all)
                mSelected.erase(o);
            else
                mSelected.insert(o);
        }
        mAnchor = r;
    } else {
        mSelected = std::set<int>(objs.begin(), objs.end());
        mAnchor = r;
    }
    guard = std::unique_lock<std::mutex>();
    if (changeCallback)
        changeCallback();
    if (twice && n.object >= 0 && openCallback)
        openCallback(n.object);
    return true;
}
