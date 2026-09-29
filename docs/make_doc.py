"""
make_doc.py -- Builds docs/InstantMeshes_Documentation.pdf with ReportLab,
in the MattRM2 documentation style (dark A4 pages, Roboto, orange accent).

Usage:
    python docs/make_doc.py [cover_screenshot.png]   (default: docs/cover.png)

Fonts: Roboto from C:/Windows/Fonts (Regular, Light, Medium, Bold).
The only picture is the cover screenshot of the application.
"""

import os
import sys

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.colors import HexColor
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, Frame, PageTemplate, Paragraph, Spacer,
                                Table, TableStyle, KeepTogether, PageBreak, NextPageTemplate,
                                Flowable)
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_CENTER

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "InstantMeshes_Documentation.pdf")
VERSION = "2.0.1"
PRODUCT = "Instant Meshes"
REPO = "github.com/MattRM2/instant-meshes"

BG = HexColor("#1E1E1E")
BAND = HexColor("#262626")
BAND2 = HexColor("#2C2C2C")
LINE = HexColor("#333333")
ORANGE = HexColor("#FB923C")
TEXT = HexColor("#E8E8E8")
MUTED = HexColor("#9A9A9A")
GREEN = HexColor("#4ADE80")
CODE_BG = HexColor("#141414")

FONTS = "C:/Windows/Fonts"
pdfmetrics.registerFont(TTFont("Roboto", os.path.join(FONTS, "Roboto-Regular.ttf")))
pdfmetrics.registerFont(TTFont("Roboto-Light", os.path.join(FONTS, "Roboto-Light.ttf")))
pdfmetrics.registerFont(TTFont("Roboto-Medium", os.path.join(FONTS, "Roboto-Medium.ttf")))
pdfmetrics.registerFont(TTFont("Roboto-Bold", os.path.join(FONTS, "Roboto-Bold_2.ttf")))
pdfmetrics.registerFontFamily("Roboto", normal="Roboto", bold="Roboto-Bold",
                              italic="Roboto-Light", boldItalic="Roboto-Bold")

W, H = A4
MARGIN = 20 * mm

body = ParagraphStyle("body", fontName="Roboto", fontSize=10, leading=15.5, textColor=TEXT)
muted = ParagraphStyle("muted", parent=body, textColor=MUTED, fontSize=9, leading=13)
bullet = ParagraphStyle("bullet", parent=body, leftIndent=10, bulletIndent=0, spaceBefore=1)
code = ParagraphStyle("code", fontName="Courier", fontSize=8.6, leading=12, textColor=TEXT)
cell = ParagraphStyle("cell", parent=body, fontSize=9, leading=13)
cellb = ParagraphStyle("cellb", parent=cell, fontName="Roboto-Bold")
head = ParagraphStyle("head", parent=cell, fontName="Roboto-Bold", textColor=HexColor("#1A1A1A"))
key = ParagraphStyle("key", parent=body, fontName="Roboto-Bold", textColor=ORANGE, spaceBefore=6)


# --------------------------------------------------------------------------
# Page decoration
# --------------------------------------------------------------------------

def page_background(c):
    c.setFillColor(BG)
    c.rect(0, 0, W, H, stroke=0, fill=1)


def inner_page(c, doc):
    page_background(c)
    c.setFont("Roboto", 8)
    c.setFillColor(MUTED)
    c.drawString(MARGIN, H - 12 * mm, "%s  |  Documentation" % PRODUCT)
    c.setFillColor(ORANGE)
    c.setFont("Roboto-Bold", 8.5)
    c.drawRightString(W - MARGIN, H - 12 * mm, str(doc.page))
    c.setStrokeColor(ORANGE)
    c.setLineWidth(0.8)
    c.line(0, H - 15 * mm, W, H - 15 * mm)
    c.setStrokeColor(LINE)
    c.line(MARGIN, 14 * mm, W - MARGIN, 14 * mm)
    c.setFont("Roboto", 7.5)
    c.setFillColor(MUTED)
    c.drawString(MARGIN, 10 * mm, "%s  \u2014  Release %s" % (PRODUCT, VERSION))
    c.drawRightString(W - MARGIN, 10 * mm, "Native Alembic  |  Matt Dark UI")


def cover_page(c, doc):
    page_background(c)
    shot = getattr(doc, "cover_image", None)
    top_h = H * 0.44
    if shot and os.path.exists(shot):
        from reportlab.lib.utils import ImageReader
        img = ImageReader(shot)
        iw, ih = img.getSize()
        # fill the top band, cropped (keep the left UI panel visible)
        scale = max(W / iw, top_h / ih)
        dw, dh = iw * scale, ih * scale
        c.saveState()
        p = c.beginPath()
        p.rect(0, H - top_h, W, top_h)
        c.clipPath(p, stroke=0, fill=0)
        c.drawImage(img, 0, H - dh, dw, dh)
        c.restoreState()
    # title plate on the picture
    c.setFillColor(HexColor("#141414"))
    c.setFillAlpha(0.82)
    c.roundRect(12 * mm, H - 30 * mm, 86 * mm, 20 * mm, 3 * mm, stroke=0, fill=1)
    c.setFillAlpha(1)
    c.setFillColor(TEXT)
    c.setFont("Roboto-Light", 20)
    c.drawString(17 * mm, H - 20 * mm, PRODUCT)
    c.setFillColor(ORANGE)
    c.setFont("Roboto", 8)
    c.drawString(17.5 * mm, H - 26.5 * mm, "R e l e a s e   %s" % "   ".join(VERSION))
    c.setFillColor(HexColor("#141414"))
    c.setFillAlpha(0.75)
    c.roundRect(12 * mm, H - top_h + 4 * mm, 70 * mm, 6 * mm, 1.5 * mm, stroke=0, fill=1)
    c.setFillAlpha(1)
    c.setFillColor(MUTED)
    c.setFont("Roboto", 7.5)
    c.drawString(15 * mm, H - top_h + 6 * mm, "GitHub : https://%s" % REPO)

    # orange timeline
    y = H - top_h - 5 * mm
    c.setStrokeColor(ORANGE)
    c.setLineWidth(0.9)
    c.line(MARGIN * 0.4, y, W - MARGIN * 0.4, y)
    c.setFillColor(ORANGE)
    for x in (MARGIN * 0.4, W / 2, W - MARGIN * 0.4):
        c.circle(x, y, 1.2 * mm, stroke=0, fill=1)

    cx = W / 2
    c.setFont("Roboto-Bold", 8.5)
    c.drawCentredString(cx, y - 11 * mm, "ALEMBIC  \u00b7  RETOPOLOGY  \u00b7  VFX PIPELINE")
    c.setFillColor(TEXT)
    c.setFont("Roboto-Bold", 30)
    c.drawCentredString(cx, y - 27 * mm, PRODUCT)
    c.setFillColor(MUTED)
    c.setFont("Roboto-Light", 15)
    c.drawCentredString(cx, y - 37 * mm, "Documentation  \u2014  MattRM2 fork of Instant Meshes 1.0")
    c.setFillColor(TEXT)
    c.setFont("Roboto", 9.5)
    c.drawCentredString(cx, y - 49 * mm, "Field-aligned automatic retopology with native Alembic (.abc) support, per-mesh")
    c.drawCentredString(cx, y - 54 * mm, "remeshing, percentage targets and the Matt Dark interface.")

    c.setFillColor(MUTED)
    c.setFont("Roboto", 8)
    c.drawCentredString(cx, 34 * mm, "Created by Matthieu Barbi\u00e9  \u00b7  2026")
    c.setFillColor(HexColor("#3A2A1A"))
    c.roundRect(cx - 17 * mm, 23 * mm, 34 * mm, 7 * mm, 1.5 * mm, stroke=0, fill=1)
    c.setFillColor(ORANGE)
    c.setFont("Roboto-Bold", 8.5)
    c.drawCentredString(cx, 25.3 * mm, "Release %s" % VERSION)
    c.setFillColor(MUTED)
    c.setFont("Roboto", 7)
    c.drawCentredString(cx, 14 * mm, "Based on Instant Meshes by W. Jakob, M. Tarini, D. Panozzo and "
                        "O. Sorkine-Hornung (BSD license). Unofficial fork.")


# --------------------------------------------------------------------------
# Building blocks
# --------------------------------------------------------------------------

class SectionHeader(Flowable):
    """Dark band with an orange left bar, a numbered circle, title and subtitle"""

    def __init__(self, number, title, subtitle=""):
        super().__init__()
        self.number, self.title, self.subtitle = number, title, subtitle
        self.height = 17 * mm if subtitle else 14 * mm

    def wrap(self, aw, ah):
        self.width = aw
        return aw, self.height

    def draw(self):
        c = self.canv
        c.setFillColor(BAND)
        c.rect(0, 0, self.width, self.height, stroke=0, fill=1)
        c.setFillColor(ORANGE)
        c.rect(0, 0, 1.4 * mm, self.height, stroke=0, fill=1)
        cy = self.height / 2
        c.circle(9 * mm, cy, 3.2 * mm, stroke=0, fill=1)
        c.setFillColor(HexColor("#FFFFFF"))
        c.setFont("Roboto-Bold", 9)
        c.drawCentredString(9 * mm, cy - 1.1 * mm, str(self.number))
        c.setFillColor(TEXT)
        c.setFont("Roboto-Bold", 14)
        if self.subtitle:
            c.drawString(15 * mm, cy + 0.6 * mm, self.title)
            c.setFillColor(MUTED)
            c.setFont("Roboto", 8)
            c.drawString(15 * mm, cy - 4.4 * mm, self.subtitle)
        else:
            c.drawString(15 * mm, cy - 1.6 * mm, self.title)


class SubHeader(Flowable):
    """Orange pill with the sub-section number, title on a dark band"""

    def __init__(self, number, title):
        super().__init__()
        self.number, self.title = number, title

    def wrap(self, aw, ah):
        self.width = aw
        return aw, 9 * mm

    def draw(self):
        c = self.canv
        c.setFillColor(BAND)
        c.rect(0, 0, self.width, 9 * mm, stroke=0, fill=1)
        c.setFillColor(ORANGE)
        c.roundRect(2 * mm, 2.3 * mm, 10 * mm, 4.4 * mm, 1 * mm, stroke=0, fill=1)
        c.setFillColor(HexColor("#1A1A1A"))
        c.setFont("Roboto-Bold", 7.5)
        c.drawCentredString(7 * mm, 3.7 * mm, self.number)
        c.setFillColor(TEXT)
        c.setFont("Roboto-Bold", 10.5)
        c.drawString(16 * mm, 3.3 * mm, self.title)


def p(text, style=body):
    return Paragraph(text, style)


def bullets(items):
    return [Paragraph(t, bullet, bulletText="\u2022") for t in items]


def table(rows, widths, header=True):
    data = [[Paragraph(str(v), head if (header and i == 0) else (cellb if j == 0 else cell))
             for j, v in enumerate(r)] for i, r in enumerate(rows)]
    t = Table(data, colWidths=widths, hAlign="LEFT")
    style = [
        ("BACKGROUND", (0, 0), (-1, -1), BAND),
        ("LINEBELOW", (0, 0), (-1, -1), 0.4, LINE),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
        ("LEFTPADDING", (0, 0), (-1, -1), 7),
    ]
    if header:
        style.append(("BACKGROUND", (0, 0), (-1, 0), ORANGE))
    t.setStyle(TableStyle(style))
    return t


def codeblock(lines):
    t = Table([[Paragraph(l.replace(" ", "&nbsp;").replace("<", "&lt;"), code)] for l in lines],
              colWidths=[W - 2 * MARGIN])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
        ("BOX", (0, 0), (-1, -1), 0.4, LINE),
        ("TOPPADDING", (0, 0), (-1, -1), 1.5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 1.5),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, 0), 6),
        ("BOTTOMPADDING", (0, -1), (-1, -1), 6),
    ]))
    return t


def callout(title, text):
    t = Table([[Paragraph("<b>%s</b>" % title, ParagraphStyle("ct", parent=body, textColor=GREEN))],
               [Paragraph(text, ParagraphStyle("cx", parent=body, fontName="Roboto-Light"))]],
              colWidths=[W - 2 * MARGIN])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), BAND2),
        ("LINEBEFORE", (0, 0), (0, -1), 3, GREEN),
        ("LEFTPADDING", (0, 0), (-1, -1), 10),
        ("TOPPADDING", (0, 0), (-1, 0), 7),
        ("BOTTOMPADDING", (0, -1), (-1, -1), 8),
    ]))
    return t


def steps(items):
    rows = [[Paragraph("<b>%d</b>" % (i + 1), ParagraphStyle("sn", parent=body, textColor=HexColor("#FFFFFF"),
                                                             alignment=TA_CENTER, fontSize=11)),
             Paragraph(t, body)] for i, t in enumerate(items)]
    t = Table(rows, colWidths=[10 * mm, W - 2 * MARGIN - 10 * mm])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (0, -1), ORANGE),
        ("BACKGROUND", (1, 0), (1, -1), BAND),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LINEBELOW", (0, 0), (-1, -1), 3, BG),
        ("TOPPADDING", (0, 0), (-1, -1), 7),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
        ("LEFTPADDING", (1, 0), (1, -1), 9),
    ]))
    return t


def swatches(rows):
    """Color legend: [(hex, role, note), ...]"""
    data = []
    for hexc, role, note in rows:
        sw = Table([[""]], colWidths=[9 * mm], rowHeights=[5 * mm])
        sw.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), HexColor(hexc))]))
        data.append([sw, Paragraph("<b>%s</b>" % role, cell), Paragraph(hexc, cell), Paragraph(note, cell)])
    t = Table(data, colWidths=[13 * mm, 40 * mm, 20 * mm, W - 2 * MARGIN - 73 * mm], hAlign="LEFT")
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), BAND),
        ("LINEBELOW", (0, 0), (-1, -1), 0.4, LINE),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
    ]))
    return t


class TocEntry(Flowable):
    def __init__(self, number, title, subs):
        super().__init__()
        self.number, self.title, self.subs = number, title, subs

    def wrap(self, aw, ah):
        self.width = aw
        self.height = 10 * mm + len(self.subs) * 5.2 * mm + 3 * mm
        return aw, self.height

    def draw(self):
        c = self.canv
        top = self.height
        c.setFillColor(BAND)
        c.rect(0, top - 10 * mm, self.width, 10 * mm, stroke=0, fill=1)
        c.setFillColor(ORANGE)
        c.setFont("Roboto-Bold", 11)
        c.drawString(4 * mm, top - 6.5 * mm, str(self.number))
        c.setFillColor(TEXT)
        c.drawString(14 * mm, top - 6.5 * mm, self.title)
        c.setFont("Roboto", 8.5)
        c.setFillColor(MUTED)
        for i, s in enumerate(self.subs):
            c.drawString(6 * mm, top - 15 * mm - i * 5.2 * mm, s)


# --------------------------------------------------------------------------
# Content
# --------------------------------------------------------------------------

SECTIONS = [
    (1, "Overview", ["1.1 What this fork adds", "1.2 Installation", "1.3 Files in the release"]),
    (2, "The Matt Dark Interface", ["2.1 Panel reference", "2.2 Target: faces or percentage",
                                    "2.3 Flow line colors", "2.4 Typical workflow"]),
    (3, "Alembic Support", ["3.1 What is read", "3.2 What is written", "3.3 Robustness"]),
    (4, "Command Line", ["4.1 Batch mode", "4.2 Options", "4.3 Examples"]),
    (5, "Per-Mesh Remeshing (Alembic and OBJ)", ["5.1 Rules: -m and --others", "5.2 Planning: --list and --dry-run",
                                                 "5.3 What happens to a remeshed mesh", "5.4 OBJ scenes"]),
    (6, "Reference", ["6.1 Accuracy of the targets", "6.2 Limitations", "6.3 Credits and licenses"]),
]


def build(cover_image=None):
    story = [Spacer(1, 1)]   # the cover itself is drawn by its page template
    story.append(NextPageTemplate("inner"))
    story.append(PageBreak())

    # ---------------- table of contents
    story.append(SectionHeader(0, "Table of Contents"))
    story.append(Spacer(1, 6 * mm))
    for n, t, subs in SECTIONS:
        story.append(TocEntry(n, t, subs))
        story.append(Spacer(1, 2.5 * mm))
    story.append(PageBreak())

    # ---------------- 1 overview
    story += [SectionHeader(1, "Overview", "Automatic retopology, now at home in an Alembic pipeline"),
              Spacer(1, 5 * mm),
              p("<b>Instant Meshes</b> turns dense or messy geometry (scans, sculpts, boolean results) into a clean "
                "quad or triangle mesh whose edges follow the shape of the model. This release is a fork of "
                "Instant Meshes 1.0 that keeps the original algorithm and its robustness, and adds what a VFX "
                "pipeline needs: <b>native Alembic</b> input and output, <b>per-mesh remeshing</b> inside an Alembic "
                "scene, <b>percentage targets</b>, and a restyled <b>Matt Dark</b> interface."),
              Spacer(1, 4 * mm), SubHeader("1.1", "What this fork adds"), Spacer(1, 3 * mm)]
    story += bullets([
        "<b>Alembic (.abc)</b> read and written natively: no Alembic library, no dependency, polygon meshes, "
        "transforms and instances.",
        "<b>Per-mesh remeshing</b>: remesh MeshA at 75% and MeshB at 85% of their faces, keep the rest of the "
        "scene untouched, write one file (-m / --others), for <b>Alembic and OBJ</b> scenes.",
        "<b>Percentage targets</b>: 75% means 75% of the original face count, in the interface and on the "
        "command line.",
        "<b>Typed targets</b>: the tiny density slider is replaced by a text box (faces or %), presets and a "
        "full-width slider.",
        "<b>Meaningful colors</b>: flow lines by direction (U / V), your strokes in orange, singularities with "
        "consistent colors.",
        "<b>OBJ reader fixed</b>: n-gons (faces with more than 4 corners used to be cut silently) and negative "
        "(relative) indices.",
        "<b>InstantMeshes.exe</b>, without a space: easier to call from scripts.",
    ])
    story += [Spacer(1, 4 * mm), SubHeader("1.2", "Installation"), Spacer(1, 3 * mm),
              steps(["Unzip the release anywhere, for example <b>D:\\Tools\\InstantMeshes</b>.",
                     "Run <b>InstantMeshes.exe</b>. Nothing to install: the executable is self-contained.",
                     "Optional: add the folder to your <b>PATH</b> to call <b>InstantMeshes</b> from any "
                     "terminal or script."]),
              Spacer(1, 4 * mm), SubHeader("1.3", "Files in the release"), Spacer(1, 3 * mm),
              table([["File", "Role"],
                     ["InstantMeshes.exe", "The application: interface and command line"],
                     ["InstantMeshes_Documentation.pdf", "This document"],
                     ["LICENSE.txt", "Instant Meshes BSD license"],
                     ["OFL.txt", "License of the Poppins typeface used by the interface"]],
                    [58 * mm, W - 2 * MARGIN - 58 * mm]),
              PageBreak()]

    # ---------------- 2 interface
    story += [SectionHeader(2, "The Matt Dark Interface", "Same workflow as Instant Meshes, clearer controls"),
              Spacer(1, 5 * mm),
              p("The panel keeps the original top-to-bottom workflow: open a mesh, choose the output, set a "
                "target, solve the orientation field, solve the position field, export. Sections are titled in "
                "orange; the main actions (Solve, Extract, Export, Save) are orange buttons."),
              Spacer(1, 4 * mm), SubHeader("2.1", "Panel reference"), Spacer(1, 3 * mm),
              table([["Control", "What it does"],
                     ["Open mesh", "Loads .obj, .ply, .abc or an .aln point cloud. The line below shows the file "
                                   "and its face count."],
                     ["Advanced", "Saves / loads the session, visualization modes, render layers, hierarchy level, "
                                  "crease angle."],
                     ["Remesh as", "Triangles (6/6), Quads 2/4 or Quads 4/4 (default)."],
                     ["Pure quad output", "Subdivides the extracted mesh once so that every face is a quad. Moved "
                                          "here from the Export menu because the target accounts for it."],
                     ["Configuration", "Extrinsic smoothing (recommended), align to open boundaries, keep sharp "
                                       "creases above an angle."],
                     ["Target", "Output density, typed in faces or in % of the input (2.2)."],
                     ["Orientation field", "Tools (comb, attractor, repeller), singularity count, flow line "
                                           "colors (2.3) and Solve."],
                     ["Position field", "Tools (edge brush, attractor, repeller), singularity count and Solve."],
                     ["Export mesh", "Smoothing iterations, Extract mesh, Show input / output, Save (.obj, "
                                     ".ply, .abc)."]],
                    [38 * mm, W - 2 * MARGIN - 38 * mm]),
              Spacer(1, 5 * mm), SubHeader("2.2", "Target: faces or percentage"), Spacer(1, 3 * mm),
              p("Choose <b>Faces</b> to type an exact face count, or <b>% of input</b> to type a percentage of the "
                "polygons of the loaded file (100% = as many faces as the input, 50% = half). Press "
                "<b>Enter</b> to apply. The line below the controls converts one into the other."),
              Spacer(1, 2 * mm)]
    story += bullets([
        "<b>Presets</b> 25 / 50 / 75 / 100% apply a percentage in one click.",
        "The <b>slider</b> spans 0 to 100% of the loaded polygons: its right end is the input face count. "
        "For more than 100%, type the value. Its <b>red range</b> marks targets the input is too coarse for: "
        "choosing one proposes to subdivide the input first.",
        "The target is the <b>final</b> face count: with <b>Pure quad output</b> the extraction aims at a "
        "quarter, the subdivision brings it back to the target.",
    ])
    story += [Spacer(1, 3 * mm),
              callout("Accuracy", "On real meshes the result lands within about \u00b13% of the target. Very small "
                                  "meshes (a few hundred faces) can deviate more, since each closed object needs a "
                                  "minimum number of faces."),
              PageBreak(),
              SubHeader("2.3", "Flow line colors"), Spacer(1, 3 * mm),
              p("Once the orientation field is solved, flow lines show how the edges of the output will run. "
                "Four modes, next to the Solve button:"),
              Spacer(1, 2 * mm),
              table([["Mode", "Use it to"],
                     ["Off", "See only your strokes and the singularities."],
                     ["Mono", "Read the flow without color noise (light grey)."],
                     ["Direction", "Tell edge loops apart at a glance: cyan lines follow U, violet lines follow "
                                   "V (green: third direction in triangle mode). Default."],
                     ["Per line", "Follow one loop around the model: one color per line."]],
                    [30 * mm, W - 2 * MARGIN - 30 * mm]),
              Spacer(1, 4 * mm),
              p("<b>Orange is reserved for what you draw.</b> Everything computed uses other colors:"),
              Spacer(1, 2 * mm),
              swatches([("#22D3EE", "U flow lines", "Direction mode, first family"),
                        ("#A78BFA", "V flow lines", "Direction mode, second family"),
                        ("#4ADE80", "W flow lines", "Triangle mode, third family"),
                        ("#FB923C", "Comb strokes", "Your orientation comb strokes"),
                        ("#F5F5F5", "Edge brush strokes", "Your edge brush strokes"),
                        ("#60A5FA", "Valence 3", "Orientation singularity"),
                        ("#F87171", "Valence 5", "Orientation singularity"),
                        ("#FACC15", "Position singularity", "Small offset"),
                        ("#F472B6", "Position singularity", "Large offset"),
                        ("#6B6B6B", "Input mesh", "Neutral clay so colors stand out")]),
              Spacer(1, 5 * mm), SubHeader("2.4", "Typical workflow"), Spacer(1, 3 * mm),
              steps(["<b>Open mesh</b> and pick <b>Remesh as</b> (Quads 4/4 for most models).",
                     "Set the <b>Target</b>, for example 50% of the input.",
                     "Click <b>Solve</b> under Orientation field. Check the flow in Direction mode; comb the "
                     "field or move singularities where needed, then stop the solver.",
                     "Click <b>Solve</b> under Position field, then open <b>Export mesh</b>.",
                     "<b>Extract mesh</b>, check it with <b>Show output</b>, then <b>Save</b> as .obj, .ply "
                     "or .abc."]),
              PageBreak()]

    # ---------------- 3 alembic
    story += [SectionHeader(3, "Alembic Support", "Native reader and writer, no Alembic library"),
              Spacer(1, 5 * mm),
              p("Alembic files are read and written by code written for this fork, on top of the Ogawa container "
                "used by every current DCC. The files it writes open in Blender (and any Alembic reader) exactly "
                "like those written by the official library, down to the content hashes readers use for caching."),
              Spacer(1, 4 * mm), SubHeader("3.1", "What is read"), Spacer(1, 3 * mm)]
    story += bullets([
        "Every <b>polygon mesh</b> of the file, placed in world space through its parent transforms "
        "(translate, rotate, scale, matrix, non-inheriting transforms).",
        "<b>Instances</b>: each instance appears at its own place.",
        "<b>N-gons</b> are triangulated for solving; quads keep the split of the OBJ reader, so an .abc and the "
        "same .obj give the same triangles.",
        "Animated files are read at their <b>first frame</b>.",
        "Cameras, curves, points and face sets are ignored; subdivision surfaces are skipped with a message.",
    ])
    story += [Spacer(1, 4 * mm), SubHeader("3.2", "What is written"), Spacer(1, 3 * mm)]
    story += bullets([
        "A new file: one transform and one polygon mesh named after the file, with quads and n-gons as "
        "extracted (no triangulation).",
        "In <b>per-mesh mode</b> (section 5): a copy of the input where only the chosen meshes change.",
        "The output may be the input file: it is fully read, then replaced in one atomic step. If anything "
        "fails, the original stays untouched.",
    ])
    story += [Spacer(1, 4 * mm), SubHeader("3.3", "Robustness"), Spacer(1, 3 * mm),
              p("Every position, size and count read from a file is checked before use; nesting depth, cycles and "
                "instancing loops are bounded. A damaged file gives an error message, never a crash."),
              Spacer(1, 3 * mm),
              callout("Tested", "More than 31,000 deliberately corrupted files are read in the test suite without "
                                "a single crash. Legacy HDF5-based Alembic files are refused with a clear message "
                                "(re-export them with the Ogawa backend)."),
              PageBreak()]

    # ---------------- 4 command line
    story += [SectionHeader(4, "Command Line", "Batch remeshing from scripts, farms and DCC tools"),
              Spacer(1, 5 * mm),
              p("Give an output file with <b>-o</b> and Instant Meshes runs without a window: it loads, remeshes, "
                "writes and exits with code 0 on success. Without <b>-o</b> the interface opens on the file."),
              Spacer(1, 4 * mm), SubHeader("4.1", "Batch mode"), Spacer(1, 3 * mm),
              codeblock(["InstantMeshes.exe input.abc -o output.abc -f 75%",
                         "InstantMeshes.exe input.obj -o output.obj -f 5000 -D"]),
              Spacer(1, 4 * mm), SubHeader("4.2", "Options"), Spacer(1, 3 * mm),
              table([["Option", "Meaning"],
                     ["-o, --output <file>", "Output .obj, .ply or .abc (batch mode)"],
                     ["-f, --faces <n> | <n>%", "Face count, or percentage of the input polygons (75%)"],
                     ["-v, --vertices <n>", "Vertex count"],
                     ["-s, --scale <length>", "Edge length in world units"],
                     ["-D, --dominant", "Quad-dominant output (no pure quad subdivision)"],
                     ["-r / -p <n>", "Orientation / position symmetry: 4/4 quads (default), 6/6 triangles"],
                     ["-c, --crease <deg>", "Keep creases sharper than this angle"],
                     ["-b, --boundaries", "Align to open boundaries"],
                     ["-S, --smooth <n>", "Smoothing iterations (default 2)"],
                     ["-d, --deterministic", "Same result on every run (slower)"],
                     ["-t, --threads <n>", "Number of threads"],
                     ["-m, --mesh <name>=<target>", "Per-mesh remeshing of an .abc or .obj (section 5)"],
                     ["--others <target>", "Target for every other mesh of the scene"],
                     ["--list / --dry-run", "List the meshes / print the plan and stop"],
                     ["-V, --version", "Print the version"]],
                    [52 * mm, W - 2 * MARGIN - 52 * mm]),
              Spacer(1, 4 * mm), KeepTogether([SubHeader("4.3", "Examples"), Spacer(1, 3 * mm),
              codeblock(["# Half the faces, pure quads",
                         "InstantMeshes.exe scan.obj -o scan_retopo.obj -f 50%",
                         "",
                         "# Triangles, exact count, sharp edges kept",
                         "InstantMeshes.exe part.abc -o part_tri.abc -r 6 -p 6 -f 20000 -c 30",
                         "",
                         "# Replace the input file in place",
                         "InstantMeshes.exe scene.abc -o scene.abc -f 60%"]),
              Spacer(1, 3 * mm),
              callout("Arguments are checked first", "Unknown output formats, invalid percentages or conflicting "
                                                     "options are reported before any computation, so a typo never "
                                                     "costs minutes of remeshing.")]),
              PageBreak()]

    # ---------------- 5 per-mesh
    story += [SectionHeader(5, "Per-Mesh Remeshing (Alembic and OBJ)", "Remesh some objects, keep the rest of the scene"),
              Spacer(1, 5 * mm),
              p("With <b>-m</b> and <b>--others</b>, each chosen mesh of an Alembic or OBJ scene is remeshed on its "
                "own, with its own target, and written back into a copy of the scene, in the same format. "
                "Everything else, other objects, cameras, curves, animation and metadata, is copied untouched."),
              Spacer(1, 3 * mm),
              codeblock(['InstantMeshes.exe scene.abc -o scene_retopo.abc -m "MeshA=75%" -m "MeshB=85%" --others 25%']),
              Spacer(1, 4 * mm), SubHeader("5.1", "Rules: -m and --others"), Spacer(1, 3 * mm),
              table([["Rule", "Selects"],
                     ["-m MeshA=75%", "The object named MeshA, anywhere, at 75% of its own faces"],
                     ["-m Props/MeshA=5000", "The object at that path from the root, 5000 faces"],
                     ['-m "Mesh*=60%"', "Every object whose name starts with Mesh (* = any characters)"],
                     ['-m "Rock_??=50%"', "Rock_01, Rock_AB... (? = exactly one character)"],
                     ['-m "Props/*=40%"', "The children of Props, and the meshes below them"],
                     ["--others 25%", "Every mesh no -m selects; without it they are copied untouched"]],
                    [45 * mm, W - 2 * MARGIN - 45 * mm]),
              Spacer(1, 3 * mm)]
    story += bullets([
        "A name selects the object and every mesh below it; names are case-sensitive; the leading / is optional.",
        "When several -m select the same mesh, <b>the last one wins</b>: write the general rule first, then the "
        "exceptions.",
        "A rule that matches nothing is an error, found before any computation.",
        "Quote wildcards (\"Mesh*=75%\"): Git Bash, Linux and macOS shells would expand them.",
    ])
    story += [Spacer(1, 4 * mm), SubHeader("5.2", "Planning: --list and --dry-run"), Spacer(1, 3 * mm),
              codeblock(["> InstantMeshes.exe scene.abc --list",
                         'Polygon meshes in "scene.abc": 2',
                         "   /Props/MeshA/MeshA    7872 faces   7958 vertices",
                         "   /Props/MeshB/MeshB     576 faces    576 vertices",
                         "",
                         '> InstantMeshes.exe scene.abc -m "Mesh*=75%" -m MeshB=85% --dry-run',
                         "   /Props/MeshA/MeshA    7872 faces  -> 75% (~5904)   [-m Mesh*=75%]",
                         "   /Props/MeshB/MeshB     576 faces  -> 85% (~490)    [-m MeshB=85%]",
                         "Dry run: nothing computed, nothing written."]),
              Spacer(1, 4 * mm), KeepTogether([SubHeader("5.3", "What happens to a remeshed mesh"), Spacer(1, 3 * mm),
              table([["Kept", "Dropped"],
                     ["Name, place in the hierarchy, parent transforms (the mesh is remeshed in world space and "
                      "written back in its own space)",
                      "UVs, normals and per-vertex / per-face attributes: they do not match the new topology"],
                     ["Object and user properties", "Several face sets (per-face materials): they cannot follow "
                                                    "the new faces"],
                     ["A single face set (one material), rebuilt over all new faces", ""]],
                    [(W - 2 * MARGIN) / 2, (W - 2 * MARGIN) / 2])]),
              Spacer(1, 3 * mm),
              callout("Refused on purpose", "Animated meshes and instanced meshes cannot be targets (-m reports them; "
                                            "--others copies them untouched and says so in the plan). If one mesh "
                                            "fails, nothing is written."),
              Spacer(1, 5 * mm), SubHeader("5.4", "OBJ scenes"), Spacer(1, 3 * mm),
              codeblock(['InstantMeshes.exe scene.obj --list',
                         'InstantMeshes.exe scene.obj -o scene_retopo.obj -m "MeshA=50%" --others 80%'])]
    story += bullets([
        "<b>Objects</b> are the <b>o</b> blocks of the file, or its <b>g</b> groups when it has no <b>o</b> "
        "line (Maya, ZBrush). --list shows the names found; paths are /Name.",
        "Untouched objects keep their lines as they are: positions, UVs, normals, materials, groups and "
        "comments. Only their face indices are renumbered, since the replaced objects change the vertex count.",
        "A replaced object keeps its <b>most used material</b>: OBJ faces inherit the last usemtl, so an object "
        "cannot be left without one. Its UVs, normals and smoothing groups are dropped.",
        "Vertices shared with an untouched object are kept; the .mtl file is not touched.",
        "The output must be an .obj; it may be the input file (atomic replacement).",
    ])
    story += [PageBreak()]

    # ---------------- 6 reference
    story += [SectionHeader(6, "Reference"), Spacer(1, 5 * mm),
              SubHeader("6.1", "Accuracy of the targets"), Spacer(1, 3 * mm),
              table([["Input", "Faces", "Deviation measured (25 to 150%, quad and -D modes)"],
                     ["Two-object scene", "8,448", "-1.8% to +1.6%"],
                     ["Open mesh", "1,968", "-2.6% to +2.0%"],
                     ["Tiny meshes", "86", "up to \u00b133%"]],
                    [45 * mm, 25 * mm, W - 2 * MARGIN - 70 * mm]),
              Spacer(1, 4 * mm), SubHeader("6.2", "Limitations"), Spacer(1, 3 * mm)]
    story += bullets([
        "Alembic: polygon meshes only; subdivision surfaces, curves and points are not remeshed; animation is "
        "read at the first frame.",
        "Remeshed meshes lose their UVs and normals (new topology).",
        "Point clouds (.aln) accept face counts, not percentages.",
        "The per-mesh mode (-m / --others, Alembic and OBJ) is available on the command line; the interface "
        "remeshes the whole loaded file.",
    ])
    story += [Spacer(1, 4 * mm), SubHeader("6.3", "Credits and licenses"), Spacer(1, 3 * mm),
              p("Instant Meshes: Wenzel Jakob, Marco Tarini, Daniele Panozzo, Olga Sorkine-Hornung, "
                "<i>Instant Field-Aligned Meshes</i>, ACM Transactions on Graphics (SIGGRAPH Asia 2015). "
                "BSD license, see LICENSE.txt."),
              Spacer(1, 2 * mm),
              p("This fork (Alembic support, per-mesh remeshing, percentage targets, Matt Dark interface): "
                "Matthieu Barbi\u00e9, 2026, same license. Source: https://%s" % REPO),
              Spacer(1, 2 * mm),
              p("Poppins typeface (interface): The Poppins Project Authors, SIL Open Font License 1.1, see OFL.txt. "
                "Hash algorithms: MurmurHash3 (Austin Appleby) and SpookyHash (Bob Jenkins), public domain."),
              ]
    return story


def main():
    cover = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cover.png")
    doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN,
                          topMargin=24 * mm, bottomMargin=22 * mm,
                          title="%s %s - Documentation" % (PRODUCT, VERSION),
                          author="Matthieu Barbie", subject="Instant Meshes MattRM2 fork")
    doc.cover_image = cover
    frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="f")
    doc.addPageTemplates([PageTemplate(id="cover", frames=[frame], onPage=cover_page),
                          PageTemplate(id="inner", frames=[frame], onPage=inner_page)])
    doc.build(build(cover))
    print("written", OUT)


if __name__ == "__main__":
    main()
