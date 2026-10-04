/*
    menubar.cpp: the menu bar of the Matt Dark interface (see menubar.h)
*/

#include "menubar.h"
#include <algorithm>

using nanogui::Color;
using Eigen::Vector2i;

namespace {

const Color BarColor(26, 26, 26, 255), PanelColor(37, 37, 37, 252), BorderColor(51, 51, 51, 255);
const Color TextColor(245, 245, 245, 255), MutedColor(136, 136, 136, 255), Orange(251, 146, 60, 255);
const int RowHeight = 24, PanelWidth = 290, Pad = 6, SeparatorHeight = 9;

bool enabled(const MenuItem &m) { return !m.enabled || m.enabled(); }

int height(const std::vector<MenuItem> &items) {
    int h = 2 * Pad;
    for (const MenuItem &m : items)
        h += m.separator ? SeparatorHeight : RowHeight;
    return h;
}

bool inside(const Vector2i &p, const Vector2i &o, const std::vector<MenuItem> &items) {
    return p.x() >= o.x() && p.x() < o.x() + PanelWidth && p.y() >= o.y() && p.y() < o.y() + height(items);
}

int row(const Vector2i &p, const Vector2i &o, const std::vector<MenuItem> &items) {
    int y = o.y() + Pad;
    for (size_t i = 0; i < items.size(); ++i) {
        const int h = items[i].separator ? SeparatorHeight : RowHeight;
        if (p.y() >= y && p.y() < y + h)
            return items[i].separator ? -1 : (int) i;
        y += h;
    }
    return -1;
}

} // namespace

void MenuBar::addMenu(const std::string &title, const std::vector<MenuItem> &items) {
    Menu m;
    m.title = title;
    m.items = items;
    m.x = mMenus.empty() ? 6 : mMenus.back().x + mMenus.back().w;
    m.w = 24 + (int) title.size() * 8;
    mMenus.push_back(m);
}

int MenuBar::menuAt(const Vector2i &p) const {
    if (p.y() < 0 || p.y() >= Height)
        return -1;
    for (size_t i = 0; i < mMenus.size(); ++i)
        if (p.x() >= mMenus[i].x && p.x() < mMenus[i].x + mMenus[i].w)
            return (int) i;
    return -1;
}

void MenuBar::open(int index) {
    mOpen = index;
    mItems = mMenus[(size_t) index].items;
    mOrigin = Vector2i(mMenus[(size_t) index].x, Height);
    mHover = mSubHover = mSubOf = -1;
    mSub.clear();
}

bool MenuBar::close() {
    if (mOpen < 0)
        return false;
    mOpen = -1;
    mSub.clear();
    mSubOf = -1;
    return true;
}

Vector2i MenuBar::subOrigin() const {
    int y = mOrigin.y() + Pad;
    for (int i = 0; i < mSubOf; ++i)
        y += mItems[(size_t) i].separator ? SeparatorHeight : RowHeight;
    return Vector2i(mOrigin.x() + PanelWidth - 4, y - Pad);
}

bool MenuBar::mouseMotion(const Vector2i &p) {
    mHoverTitle = menuAt(p);
    if (mOpen < 0)
        return false;
    if (mHoverTitle >= 0 && mHoverTitle != mOpen)
        open(mHoverTitle);
    if (!mSub.empty() && inside(p, subOrigin(), mSub)) {
        mSubHover = row(p, subOrigin(), mSub);
        return true;
    }
    mSubHover = -1;
    mHover = inside(p, mOrigin, mItems) ? row(p, mOrigin, mItems) : -1;
    if (mHover >= 0) {
        const MenuItem &m = mItems[(size_t) mHover];
        if (m.submenu && enabled(m)) {
            if (mSubOf != mHover) {
                mSub = m.submenu();
                mSubOf = mHover;
            }
        } else {
            mSub.clear();
            mSubOf = -1;
        }
    }
    return true;
}

bool MenuBar::mouseButton(const Vector2i &p, int button, bool down) {
    const int title = menuAt(p);
    if (mOpen < 0) {
        if (title < 0)
            return false;
        if (down && button == GLFW_MOUSE_BUTTON_1)
            open(title);
        return true;
    }
    if (!down)
        return true;
    if (title >= 0) {
        if (title == mOpen)
            close();
        else
            open(title);
        return true;
    }
    const MenuItem *chosen = nullptr;
    if (!mSub.empty() && inside(p, subOrigin(), mSub)) {
        const int r = row(p, subOrigin(), mSub);
        if (r < 0)
            return true;
        chosen = &mSub[(size_t) r];
    } else if (inside(p, mOrigin, mItems)) {
        const int r = row(p, mOrigin, mItems);
        if (r < 0)
            return true;
        chosen = &mItems[(size_t) r];
        if (chosen->submenu)
            return true;   /* opens on hover */
    }
    if (chosen && (!enabled(*chosen) || button != GLFW_MOUSE_BUTTON_1))
        return true;
    const std::function<void()> action = chosen ? chosen->action : nullptr;
    close();
    if (action)
        action();
    return true;
}

void MenuBar::box(NVGcontext *ctx, const Vector2i &o, const std::vector<MenuItem> &items, int hover) const {
    const int h = height(items);
    NVGpaint shadow = nvgBoxGradient(ctx, o.x(), o.y() + 2, PanelWidth, h, 4, 12,
                                     nvgRGBA(0, 0, 0, 120), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(ctx);
    nvgRect(ctx, o.x() - 12, o.y() - 10, PanelWidth + 24, h + 24);
    nvgRoundedRect(ctx, o.x(), o.y(), PanelWidth, h, 4);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadow);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, o.x(), o.y(), PanelWidth, h, 4);
    nvgFillColor(ctx, PanelColor);
    nvgFill(ctx);
    nvgStrokeColor(ctx, BorderColor);
    nvgStrokeWidth(ctx, 1);
    nvgStroke(ctx);

    nvgFontFace(ctx, "sans");
    nvgFontSize(ctx, 15);
    int y = o.y() + Pad;
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem &m = items[i];
        if (m.separator) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, o.x() + 8, y + 4.5f);
            nvgLineTo(ctx, o.x() + PanelWidth - 8, y + 4.5f);
            nvgStrokeColor(ctx, BorderColor);
            nvgStroke(ctx);
            y += SeparatorHeight;
            continue;
        }
        const bool on = enabled(m);
        if ((int) i == hover && on) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, o.x() + 4, y, PanelWidth - 8, RowHeight, 3);
            nvgFillColor(ctx, Color(251, 146, 60, 70));
            nvgFill(ctx);
        }
        /* the shortcut (or a note) on the right, the label clipped before it */
        float right = 0;
        if (!m.submenu && !m.shortcut.empty()) {
            float bounds[4];
            nvgTextBounds(ctx, 0, 0, m.shortcut.c_str(), nullptr, bounds);
            right = std::min(bounds[2] - bounds[0], PanelWidth * 0.45f) + 12;
        }
        nvgSave(ctx);
        nvgIntersectScissor(ctx, o.x() + 8, y, PanelWidth - 30 - right, RowHeight);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, on ? TextColor : MutedColor);
        nvgText(ctx, o.x() + 14, y + RowHeight * 0.5f, m.label.c_str(), nullptr);
        nvgRestore(ctx);
        nvgSave(ctx);
        nvgIntersectScissor(ctx, o.x() + PanelWidth - 12 - right, y, right, RowHeight);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        if (m.submenu) {
            const float ax = o.x() + PanelWidth - 18, ay = y + RowHeight * 0.5f;
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, ax, ay - 4);
            nvgLineTo(ctx, ax + 5, ay);
            nvgLineTo(ctx, ax, ay + 4);
            nvgClosePath(ctx);
            nvgFillColor(ctx, on ? Orange : MutedColor);
            nvgFill(ctx);
        } else if (!m.shortcut.empty()) {
            nvgFillColor(ctx, MutedColor);
            nvgText(ctx, o.x() + PanelWidth - 12, y + RowHeight * 0.5f, m.shortcut.c_str(), nullptr);
        }
        nvgRestore(ctx);
        y += RowHeight;
    }
}

void MenuBar::draw(NVGcontext *ctx, int width) {
    nvgSave(ctx);
    nvgResetScissor(ctx);
    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, width, Height);
    nvgFillColor(ctx, BarColor);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, 0, Height - 0.5f);
    nvgLineTo(ctx, width, Height - 0.5f);
    nvgStrokeColor(ctx, BorderColor);
    nvgStrokeWidth(ctx, 1);
    nvgStroke(ctx);
    nvgFontFace(ctx, "sans");
    nvgFontSize(ctx, 15);
    nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (size_t i = 0; i < mMenus.size(); ++i) {
        const Menu &m = mMenus[i];
        const bool open = mOpen == (int) i;
        if (open || (int) i == mHoverTitle) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, m.x + 2, 3, m.w - 4, Height - 6, 3);
            nvgFillColor(ctx, open ? Color(251, 146, 60, 90) : Color(255, 255, 255, 18));
            nvgFill(ctx);
        }
        nvgFillColor(ctx, open ? Orange : TextColor);
        nvgText(ctx, m.x + m.w * 0.5f, Height * 0.5f, m.title.c_str(), nullptr);
    }
    if (mOpen >= 0) {
        box(ctx, mOrigin, mItems, mHover);
        if (!mSub.empty())
            box(ctx, subOrigin(), mSub, mSubHover);
    }
    nvgRestore(ctx);
}
