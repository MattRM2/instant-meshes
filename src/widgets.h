/*
    widgets.h: Additional widgets that are not part of NanoGUI

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include <nanogui/nanogui.h>

class ProgressButton : public nanogui::Button {
public:
    ProgressButton(Widget *parent, const std::string &caption = "Untitled", int icon = 0);

    float progress() const { return mProgress; }
    void setProgress(float value) { mProgress = value; }

    void draw(NVGcontext *ctx);
private:
    float mProgress;
};

/* A drop-down list that opens below its button, as wide as it. nanogui's
   combo boxes open to the right of their window, off screen for a panel at
   the right edge (the Outliner) */
class DropDown : public nanogui::ComboBox {
public:
    DropDown(nanogui::Widget *parent, const std::vector<std::string> &items);
    void performLayout(NVGcontext *ctx) override;
};
