/*
    outliner.h: the Outliner, the meshes of a project as a tree (by path).
    Drawn by hand with NanoVG, only the visible rows: tens of thousands of
    meshes stay fluid. Per mesh: a check box (what "Process" takes), its
    name, faces, target (its own in orange, the default one muted) and
    state; groups sum their meshes' faces and check them all at once.
    Click selects (Ctrl adds, Shift extends), double click opens the mesh,
    the header sorts, the filter keeps the paths that match (* and ?, or a
    part of the name).
*/

#pragma once

#include <nanogui/nanogui.h>
#include <functional>
#include <mutex>
#include <set>

class Project;

class OutlinerView : public nanogui::Widget {
public:
    explicit OutlinerView(nanogui::Widget *parent);

    /// The project shown ('lock' guards it while a worker updates it)
    void setProject(Project *project, std::mutex *lock);
    /// The meshes changed (targets, states): sort again, keep the view
    void refresh();
    void setFilter(const std::string &filter);
    /// "name", "faces", "state"; descending or not
    void setSort(const std::string &key, bool descending);
    std::string sortKey() const;
    bool sortDescending() const { return mSortDesc; }

    /// Selected meshes (object indices)
    const std::set<int> &selection() const { return mSelected; }
    void setSelection(const std::set<int> &s);
    /// The row of a mesh shown: its groups expanded, scrolled to it
    void reveal(int object);

    std::function<void(int object)> openCallback;   ///< double click on a mesh
    std::function<void()> changeCallback;           ///< check boxes or selection changed

    void draw(NVGcontext *ctx) override;
    bool mouseButtonEvent(const Eigen::Vector2i &p, int button, bool down, int modifiers) override;
    bool mouseMotionEvent(const Eigen::Vector2i &p, const Eigen::Vector2i &rel, int button, int modifiers) override;
    bool mouseDragEvent(const Eigen::Vector2i &p, const Eigen::Vector2i &rel, int button, int modifiers) override;
    bool scrollEvent(const Eigen::Vector2i &p, const Eigen::Vector2f &rel) override;

private:
    struct Node {
        std::string name;
        int object = -1;              /* mesh, or -1 for a group */
        int parent = -1;
        std::vector<int> children;
        bool expanded = true;
    };
    void build();
    void layoutRows();
    void collect(int node, std::vector<int> &objects) const;
    int rowAt(const Eigen::Vector2i &p) const;
    void clampScroll();
    /// The scroll bar's thumb (top, size, in pixels from the top of the track); false: no bar
    bool thumb(float &at, float &size) const;
    void dragThumb(int y);

    Project *mProject = nullptr;
    std::mutex *mLock = nullptr;
    std::vector<Node> mNodes;                 /* 0: the root (not shown) */
    std::vector<std::pair<int, int>> mRows;   /* visible (node, depth) */
    std::set<int> mSelected;
    std::string mFilter;
    int mSort = 0;                            /* 0 scene order, 1 name, 2 faces, 3 state */
    bool mSortDesc = false;
    int mScroll = 0, mHover = -1, mAnchor = -1;
    int mThumbGrab = -1;                      /* dragging the scroll bar: where the thumb was grabbed */
    double mLastClick = 0;
    int mLastClickRow = -1;
};
