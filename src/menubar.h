/*
    menubar.h: a menu bar for the Matt Dark interface (nanogui has none).

    Not a widget: the screen draws it last and gives it the mouse first, so
    that it stays above the windows (nanogui brings a clicked window to the
    front). A thin bar at the top; a menu opens below its title, with the
    shortcuts, disabled entries, separators and submenus (filled when they
    open, e.g. the recent files). A click outside an open menu closes it.
*/

#pragma once

#include <nanogui/nanogui.h>
#include <functional>

struct MenuItem {
    std::string label, shortcut;
    std::function<void()> action;
    std::function<bool()> enabled;                     ///< null: always enabled
    std::function<std::vector<MenuItem>()> submenu;    ///< non-null: a submenu, filled on opening
    bool separator = false;

    static MenuItem item(const std::string &label, const std::string &shortcut, std::function<void()> action,
                         std::function<bool()> enabled = nullptr) {
        MenuItem m;
        m.label = label;
        m.shortcut = shortcut;
        m.action = action;
        m.enabled = enabled;
        return m;
    }
    static MenuItem sub(const std::string &label, std::function<std::vector<MenuItem>()> submenu,
                        std::function<bool()> enabled = nullptr) {
        MenuItem m;
        m.label = label;
        m.submenu = submenu;
        m.enabled = enabled;
        return m;
    }
    static MenuItem line() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

class MenuBar {
public:
    static const int Height = 26;

    void addMenu(const std::string &title, const std::vector<MenuItem> &items);

    /// Draws the bar (full width) and the open menu
    void draw(NVGcontext *ctx, int width);
    /// Mouse events, before the screen's; true when used
    bool mouseButton(const Eigen::Vector2i &p, int button, bool down);
    bool mouseMotion(const Eigen::Vector2i &p);

    bool isOpen() const { return mOpen >= 0; }
    /// Closes the open menu (Escape); true if one was open
    bool close();

private:
    struct Menu {
        std::string title;
        std::vector<MenuItem> items;
        int x = 0, w = 0;
    };
    int menuAt(const Eigen::Vector2i &p) const;
    void open(int index);
    Eigen::Vector2i subOrigin() const;
    void box(NVGcontext *ctx, const Eigen::Vector2i &o, const std::vector<MenuItem> &items, int hover) const;

    std::vector<Menu> mMenus;
    int mOpen = -1, mHoverTitle = -1, mHover = -1, mSubHover = -1, mSubOf = -1;
    std::vector<MenuItem> mItems, mSub;
    Eigen::Vector2i mOrigin = Eigen::Vector2i::Zero();
};
