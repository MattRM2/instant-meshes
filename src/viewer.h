/*
    viewer.h: Contains the graphical user interface of Instant Meshes

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include "glutil.h"
#include "widgets.h"
#include "hierarchy.h"
#include "field.h"
#include "bvh.h"
#include "meshstats.h"
#include "meshio.h"
#include "menubar.h"
#include "outliner.h"
#include "project.h"
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

using nanogui::Alignment;
using nanogui::Arcball;
using nanogui::BoxLayout;
using nanogui::Button;
using nanogui::CheckBox;
using nanogui::Color;
using nanogui::ComboBox;
using nanogui::GLFramebuffer;
using nanogui::GroupLayout;
using nanogui::ImagePanel;
using nanogui::Label;
using nanogui::MessageDialog;
using nanogui::Orientation;
using nanogui::Popup;
using nanogui::PopupButton;
using nanogui::ProgressBar;
using nanogui::Screen;
using nanogui::Slider;
using nanogui::TextBox;
using nanogui::ToolButton;
using nanogui::VScrollPanel;
using nanogui::Widget;
using nanogui::Window;
using nanogui::frustum;
using nanogui::lookAt;
using nanogui::project;
using nanogui::scale;
using nanogui::translate;
using nanogui::unproject;
using nanogui::utf8;

struct CurvePoint;

class Viewer : public Screen {
public:
    Viewer(bool fullscreen, bool deterministic);
    virtual ~Viewer();

    bool mouseMotionEvent(const Vector2i &p, const Vector2i &rel,
                                  int button, int modifiers);

    bool mouseButtonEvent(const Vector2i &p, int button, bool down,
                                  int modifiers);

    bool keyboardEvent(int key, int scancode, int action, int modifiers);

    bool scrollEvent(const Vector2i &p, const Vector2f &rel);

    void loadInput(std::string filename,
                   Float creaseAngle = std::numeric_limits<Float>::infinity(),
                   Float scale = -1, int face_count = -1, int vertex_count = -1,
                   int rosy = 4, int posy = 4, int knn_points = 10);

    /// Opens a file by its kind: a project (.imd), a scene (.abc, .obj,
    /// .usd*: its meshes in the Outliner), a mesh or point cloud; asks for
    /// one when 'filename' is empty
    void openFile(const std::string &filename);

    bool dropEvent(const std::vector<std::string> &filenames) override;
    /// The window is closing: false keeps it open (unsaved changes)
    bool closeRequested();

    void setSymmetry(int rosy, int posy);
    void setExtrinsic(bool extrinsic);

    /// Face target as a percentage of the input polygons (-f N% at startup)
    void setTargetPercent(Float percent);

    void resetState();
    void loadState(std::string filename, bool compat = false);
    void saveState(std::string filename);
    void renderMitsuba();
    void setFloorPosition();
    void draw(NVGcontext *ctx);

protected:
    void extractMesh();
    void uploadOutputMesh();
    void showResults();
    void extractConsensusGraph();

    void drawContents();
    void drawOverlay();

    /* Loads a triangle mesh (or point cloud) into the viewport */
    void loadMesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons, const std::string &label,
                  Float creaseAngle, Float scale, int face_count, int vertex_count, int rosy, int posy,
                  int knn_points);

    /* Projects, scenes and the Outliner */
    void buildMenus();
    void buildOutliner();
    void layoutOutliner();
    void openScene(const std::string &filename);
    void openProject(const std::string &imd, const std::string &source = std::string());
    void newProject();
    void saveProject(bool as);
    void exportMesh();
    void writeScene();
    void processChecked();
    void cancelProcessing();
    void pollWorker();
    void openObject(int index);
    void openWholeScene();
    void useViewportResult();
    void splitViewportResult();
    void drawModeBanner(NVGcontext *ctx);
    void setSelectedTarget(bool clear);
    void refreshOutliner();
    void setDirty(bool dirty = true);
    void updateTitle();
    RemeshParams guiParams() const;
    void guiToOptions();
    void optionsToGui();
    std::string commandLine() const;
    void confirm(const std::string &question, const std::function<void()> &then);
    void addRecent(const std::string &file);
    std::vector<std::string> recentFiles() const;
    void captureWork();
    void restoreWork(int index);
    void clearViewport();
    bool busy() const { return mWorkerBusy; }
    bool hasProjectSelection() const;

    bool resizeEvent(const Vector2i &size);

    void refreshColors();

    void traceFlowLines();

    void refreshStrokes();

    void showProgress(const std::string &caption, Float value);

    void computeCameraMatrices(Eigen::Matrix4f &model,
                               Eigen::Matrix4f &view,
                               Eigen::Matrix4f &proj);

    void setLevel(int level);
    void applyMattDarkTheme();
    void applyTargetFaces(Float faces, bool prompt);
    void refreshTargetUI();
    Float facesPerVertex() const;
    Float targetReference() const;
    void setFlowColorMode(int mode);
    void setTargetScale(Float scale);
    void setTargetVertexCount(uint32_t v);
    void setTargetVertexCountPrompt(uint32_t v);

    bool createSmoothPath(const std::vector<Vector2i> &curve);

    void repaint();

    void setCreaseAnglePrompt(bool enabled, Float creaseAngle);
    void shareGLBuffers();
    bool refreshPositionSingularities();
    bool refreshOrientationSingularities();
    std::pair<Vector3f, Vector3f> singularityPositionAndNormal(uint32_t v) const;
    bool toolActive() const;

    /* Viewport selection, navigation */
    void pickObject(const Vector2i &p, int modifiers);
    bool faceObjects();
    void refreshHighlight();
    void uploadHighlight();
    void frameSelection();
    void showPreferences();
    void loadPreferences();
    void savePreferences() const;

protected:
    struct CameraParameters {
        Arcball arcball;
        float zoom = 1.0f, viewAngle = 45.0f;
        float dnear = 0.05f, dfar = 100.0f;
        Eigen::Vector3f eye = Eigen::Vector3f(0.0f, 0.0f, 5.0f);
        Eigen::Vector3f center = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 5.0f);
        Eigen::Vector3f modelTranslation = Eigen::Vector3f::Zero();
        Eigen::Vector3f modelTranslation_start = Eigen::Vector3f::Zero();
        float modelZoom = 1.0f;
    };

    std::string mFilename;
    bool mDeterministic;
    bool mUseHalfFloats;

    /* Data being processed */
    std::map<uint32_t, uint32_t> mCreaseMap;
    std::set<uint32_t> mCreaseSet;
    VectorXb mNonmanifoldVertices;
    VectorXb mBoundaryVertices;
    MultiResolutionHierarchy mRes;
    Optimizer mOptimizer;
    BVH *mBVH;
    MeshStats mMeshStats;
    int mSelectedLevel;
    Float mCreaseAngle;
    Matrix4f mFloor;
    VectorXu mE2E;

    /* Painting tools */
    std::vector<Vector2i> mScreenCurve;
    std::vector<std::pair<uint32_t, std::vector<CurvePoint>>> mStrokes;

    /* Extraction result */
    MatrixXu mF_extracted;
    MatrixXf mV_extracted;
    MatrixXf mN_extracted, mNf_extracted;

    /* Camera / navigation / misc */
    CameraParameters mCamera;
    CameraParameters mCameraSnapshots[12];
    Vector2i mTranslateStart;
    bool mTranslate, mDrag;
    /* Navigation (Preferences): Instant Meshes (left drag orbits), Maya, Blender */
    enum Navigation { NavigationClassic = 0, NavigationMaya = 1, NavigationBlender = 2 };
    int mNavigation = NavigationClassic;
    int mOrbitButton = -1, mPanButton = -1, mZoomButton = -1;
    Vector2i mZoomStart, mClickStart;
    float mZoomStartValue = 1.0f;
    bool mClickPending = false;        /* a left press that has not moved: a click selects */
    /* Whole scene view: the mesh of each face, the selected ones highlighted */
    std::vector<int> mFaceObject;
    bool mHighlightStale = true;
    uint32_t mHighlightFaces = 0;
    std::map<uint32_t, uint32_t> mOrientationSingularities;
    std::map<uint32_t, Vector2i> mPositionSingularities;
    bool mContinueWithPositions;

    /* Colors */
    Vector3f mSpecularColor, mBaseColor;
    Vector3f mInteriorFactor, mEdgeFactor0;
    Vector3f mEdgeFactor1, mEdgeFactor2;

    /* OpenGL objects */
    GLFramebuffer mFBO;
    SerializableGLShader mPointShader63, mPointShader24, mPointShader44;
    SerializableGLShader mMeshShader63, mMeshShader24, mMeshShader44;
    SerializableGLShader mOrientationFieldShader;
    SerializableGLShader mPositionFieldShader;
    SerializableGLShader mPositionSingularityShader;
    SerializableGLShader mOrientationSingularityShader;
    SerializableGLShader mFlowLineShader, mStrokeShader, mHighlightShader;
    SerializableGLShader mOutputMeshShader;
    SerializableGLShader mOutputMeshWireframeShader;
    bool mNeedsRepaint;
    uint32_t mDrawIndex;

    /* GUI-related */
    enum Layers {
        InputMesh,
        InputMeshWireframe,
        FaceLabels,
        VertexLabels,
        FlowLines,
        OrientationField,
        OrientationFieldSingularities,
        PositionField,
        PositionFieldSingularities,
        BrushStrokes,
        OutputMesh,
        OutputMeshWireframe,
        LayerCount
    };

    CheckBox *mLayers[LayerCount];
    ComboBox *mVisualizeBox, *mSymmetryBox;
    CheckBox *mExtrinsicBox, *mAlignToBoundariesBox;
    CheckBox *mCreaseBox, *mPureQuadBox;
    ProgressButton *mSolveOrientationBtn, *mSolvePositionBtn;
    Button *mHierarchyMinusButton, *mHierarchyPlusButton;
    Button *mSaveBtn, *mSwitchBtn;
    PopupButton *mExportBtn;
    Button *mAboutBtn = nullptr;
    ToolButton *mOrientationComb, *mOrientationAttractor, *mOrientationScareBrush;
    ToolButton *mEdgeBrush, *mPositionAttractor, *mPositionScareBrush;
    TextBox *mHierarchyLevelBox, *mCreaseAngleBox;
    TextBox *mOrientationSingularityBox, *mPositionSingularityBox, *mSmoothBox;
    Slider *mCreaseAngleSlider, *mSmoothSlider;
    Slider *mOrientationFieldSizeSlider, *mOrientationFieldSingSizeSlider;
    Slider *mPositionFieldSingSizeSlider, *mFlowLineSlider;
#ifdef VISUALIZE_ERROR
    Graph *mGraph;
#endif

    /* Target (faces or % of the input polygons) and flow line colors */
    enum TargetMode { TargetFaces = 0, TargetPercent = 1 };
    Button *mTargetModeBtn[2], *mTargetPresetBtn[4], *mFlowModeBtn[4];
    TextBox *mTargetBox;
    Slider *mTargetSlider;
    Label *mTargetInfo, *mInputInfoLabel;
    int mTargetMode = TargetPercent;
    int mFlowColorMode = 2;            /* 1 = mono, 2 = direction, 3 = per line */
    Float mTargetFaces = 0;            /* output faces, pure quad subdivision included */
    Float mUnsafeVertexCount = std::numeric_limits<Float>::infinity();
    uint64_t mInputPolygons = 0;
    SceneUnits mUnits;                 /* of the loaded file, written to a .usda */

    /* Projects, scenes, Outliner, menus */
    MenuBar mMenuBar;
    Window *mWindow = nullptr, *mOutlinerWindow = nullptr;
    OutlinerView *mOutliner = nullptr;
    Label *mOutlinerInfo = nullptr, *mBatchLabel = nullptr;
    TextBox *mFilterBox = nullptr, *mObjectTargetBox = nullptr, *mOthersBox = nullptr;
    ComboBox *mUVBox = nullptr, *mExportUVBox = nullptr;
    CheckBox *mKeepBorderBox = nullptr, *mProxyBox = nullptr, *mSkipFailedBox = nullptr, *mDeterministicBox = nullptr;
    Button *mProcessBtn = nullptr, *mCancelBtn = nullptr, *mUseResultBtn = nullptr, *mWriteSceneBtn = nullptr;
    Button *mOpenObjectBtn = nullptr, *mWholeSceneBtn = nullptr, *mShowResultBtn = nullptr;
    ProgressBar *mBatchBar = nullptr;
    std::unique_ptr<Project> mProject;
    std::mutex mProjectLock;
    std::string mProjectFile;
    bool mDirty = false;
    int mOpenObject = -1;
    int mShownDone = -1;
    bool mClosing = false;
    bool mTargetAsked = false, mAskingTarget = false;
    static const int OutlinerWidth = 410;
    std::thread mWorker;
    std::atomic<bool> mWorkerBusy { false }, mCancel { false };
    std::atomic<int> mWorkerDone { 0 }, mWorkerTotal { 0 };
    std::string mWorkerCurrent;              /* guarded by mProjectLock */
    std::vector<std::string> mWorkerErrors;  /* guarded by mProjectLock */

    /* Progress display */
    std::function<void(const std::string &, Float)> mProgress;
    Window *mProgressWindow;
    ProgressBar *mProgressBar;
    Label *mProgressLabel;
    tbb::spin_mutex mProgressMutex;
    double mLastProgressMessage;
    double mOperationStart;
    uint32_t mOutputMeshFaces, mOutputMeshLines;
    uint32_t mFlowLineFaces, mStrokeFaces;
};
