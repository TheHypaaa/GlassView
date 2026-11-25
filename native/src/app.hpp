// GlassView Native – application core (shared by all feature files).
#pragma once
// One process: GLFW window + OpenCASCADE viewer (V3d/AIS, OpenGL) + Dear ImGui interface.
#include "model.hpp"
#include "platform.hpp"
#include "ui.hpp"
#include "blur.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#ifdef _WIN32
extern "C" __declspec(dllimport) long __stdcall DwmSetWindowAttribute(void* hwnd, unsigned long attr, const void* pv, unsigned long cb);
#endif

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ColoredShape.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Prs3d_Presentation.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <SelectMgr_SortCriterion.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <AIS_AnimationCamera.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Graphic3d_Vec2.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_DatumAspect.hxx>
#include <Prs3d_TextAspect.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Prs3d_IsoAspect.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <Message.hxx>
#include <Message_PrinterOStream.hxx>
#include <Standard_Version.hxx>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace gv;

// ======================================================================== settings
struct Settings {
  int quality = 1;          // 0..3
  bool catiaEdges = true;   // sharp black, tangent soft
  bool ortho = true;
  bool wheelReverse = false;
  int theme = 2;            // 0 light, 1 dark, 2 auto (follows Windows)
  bool tree = true;
  int displayMode = 1;      // 0 shaded, 1 edges, 2 wire, 3 mesh
  bool grid = true;
  int zoomSpeed = 100;      // percent
  std::map<std::string, std::string> kv; // tool settings, shortcuts, units …
  std::string get(const std::string& k, const std::string& def) const { auto it = kv.find(k); return it == kv.end() ? def : it->second; }
  int geti(const std::string& k, int def) const { auto it = kv.find(k); return it == kv.end() ? def : atoi(it->second.c_str()); }
  double getd(const std::string& k, double def) const { auto it = kv.find(k); return it == kv.end() ? def : atof(it->second.c_str()); }

  void load() {
    std::ifstream f(settingsPath());
    std::string line;
    while (std::getline(f, line)) {
      size_t e = line.find('=');
      if (e == std::string::npos) continue;
      std::string k = line.substr(0, e);
      int v = atoi(line.c_str() + e + 1);
      if (k == "quality") quality = std::max(0, std::min(3, v));
      else if (k == "catiaEdges") catiaEdges = v;
      else if (k == "ortho") ortho = v;
      else if (k == "wheelReverse") wheelReverse = v;
      else if (k == "theme") theme = v >= 0 && v <= 2 ? v : 2;
      else if (k == "tree") tree = v;
      else if (k == "displayMode") displayMode = std::max(0, std::min(3, v));
      else if (k == "grid") grid = v;
      else if (k == "zoomSpeed") zoomSpeed = std::max(25, std::min(300, v));
      else kv[k] = line.substr(e + 1);
    }
  }
  void save() const {
    std::ofstream f(settingsPath());
    f << "quality=" << quality << "\ncatiaEdges=" << catiaEdges << "\northo=" << ortho << "\nwheelReverse=" << wheelReverse
      << "\ntheme=" << theme << "\ntree=" << tree << "\ndisplayMode=" << displayMode << "\ngrid=" << grid << "\nzoomSpeed=" << zoomSpeed << "\n";
    for (auto& [k, v] : kv) f << k << "=" << v << "\n";
  }
};

// ======================================================================== feature hooks (features.cpp / measure.cpp)
class App;
struct Doc;
struct DocExt; // per-document feature state (measurements, colour overrides …)
void addRecent(App&, const std::string&);
namespace feat {
void init(App&);
void toolButtons(App&); void colorButton(App&); bool uiHidden(); void treeRightClick(App&, gv::Node*);
void settingsExtra(App&); void settingsKeys(App&); void recentButton(App&); void shutdown(App&); bool hoverSuppressed(); void runAction(App&, const std::string&); void setScript(const std::string&);
void drawUi(App&);                                  // panels and overlays (inside the ImGui frame)
bool onKey(App&, int key, int mods);                // keyboard shortcuts (customisable)
bool onPick(App&, const Graphic3d_Vec2i& px, int mods); // left click in the view; true = consumed
void onRightClick(App&, const Graphic3d_Vec2i& px);
void afterFlush(App&);                              // after OCCT processed input (hover is known)
void onDocShown(App&);                              // active tab changed or its model was (re)built
void onPartsAdded(App&, Doc&, size_t firstPart);
bool wantsFrames(App&);
void beforeRedraw(App&);
}

// ======================================================================== documents
struct Doc {
  std::shared_ptr<DocExt> ext;
  std::string path, name;
  std::shared_ptr<Model> model;
  std::vector<Handle(AIS_Shape)> prs;   // one per part
  std::vector<Handle(AIS_Shape)> soft;  // tangent edges per part (may be null)
  std::unordered_map<const AIS_InteractiveObject*, int> partOf;
  Handle(Graphic3d_Camera) camera;      // saved when switching tabs
  // loading
  std::shared_ptr<LoadProgress> prog;
  std::shared_ptr<std::shared_ptr<Model>> result;
  std::shared_ptr<std::mutex> mtx;
  std::thread worker;
  bool loading = false;
  bool displayed = false;
  std::string error;
  Node* selected = nullptr;
  ~Doc() {
    if (worker.joinable()) { if (prog) prog->cancel = true; worker.join(); }
  }
};

// ======================================================================== ground grid
/** Fading ground grid under the model (not selectable, ignored by Fit). */
class GridPrs : public AIS_InteractiveObject {
public:
  double cx = 0, cy = 0, z = 0, step = 10, extent = 300;
  double width = 0.1; // world width of a 1-pixel line at the current zoom
  bool night = false;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    // lines are cut into short pieces and sorted into distance bands; each band gets its own
    // transparency, which gives a soft radial fade (OpenGL line aspects carry one alpha each)
    const int n = (int)std::ceil(extent / step);
    const int sub = 2;
    const double R = n * step;
    const int NB = 7;
    std::vector<std::vector<gp_Pnt>> pts(NB * 2);
    const double ox = std::round(cx / step) * step, oy = std::round(cy / step) * step;
    auto add = [&](const gp_Pnt& a, const gp_Pnt& b, int major) {
      const double mx = (a.X() + b.X()) * 0.5 - ox, my = (a.Y() + b.Y()) * 0.5 - oy;
      const double d = std::sqrt(mx * mx + my * my) / R;
      if (d >= 1.0) return;
      const int band = std::min(NB - 1, (int)(d * NB));
      pts[band * 2 + major].push_back(a);
      pts[band * 2 + major].push_back(b);
    };
    for (int i = -n; i <= n; i++) {
      const double gx = ox + i * step, gy = oy + i * step;
      const int kx = std::llabs(std::llround(gx / step)) % 10 == 0, ky = std::llabs(std::llround(gy / step)) % 10 == 0;
      for (int k = 0; k < 2 * n * sub; k++) {
        const double a = -R + k * step / sub, b = a + step / sub;
        add(gp_Pnt(gx, oy + a, z), gp_Pnt(gx, oy + b, z), kx);
        add(gp_Pnt(ox + a, gy, z), gp_Pnt(ox + b, gy, z), ky);
      }
    }
    // OpenGL lines are always drawn opaque by OCCT, so each line piece becomes a thin quad
    // (transparent fills are blended in OCCT's transparency pass)
    const Quantity_Color col = night ? Quantity_Color(1, 1, 1, Quantity_TOC_RGB) : Quantity_Color(0.0, 0.03, 0.10, Quantity_TOC_RGB);
    for (int band = 0; band < NB; band++) {
      const double t = (band + 0.5) / NB, fade = std::max(0.0, 1.0 - t * t);
      for (int major = 0; major < 2; major++) {
        auto& v = pts[band * 2 + major];
        if (v.empty()) continue;
        const double hw = width * (major ? 0.75 : 0.5);
        const int nq = (int)v.size() / 2;
        Handle(Graphic3d_ArrayOfTriangles) arr = new Graphic3d_ArrayOfTriangles(nq * 4, nq * 6);
        for (int q = 0; q < nq; q++) {
          const gp_Pnt& a = v[q * 2];
          const gp_Pnt& b = v[q * 2 + 1];
          const bool alongY = std::fabs(b.X() - a.X()) < std::fabs(b.Y() - a.Y());
          const gp_Vec off = alongY ? gp_Vec(hw, 0, 0) : gp_Vec(0, hw, 0);
          const int i0 = arr->AddVertex(a.Translated(-off));
          arr->AddVertex(a.Translated(off));
          arr->AddVertex(b.Translated(off));
          arr->AddVertex(b.Translated(-off));
          arr->AddEdges(i0, i0 + 1, i0 + 2);
          arr->AddEdges(i0, i0 + 2, i0 + 3);
        }
        const double base = major ? (night ? 0.24 : 0.22) : (night ? 0.10 : 0.09);
        const float alpha = (float)(base * fade);
        Handle(Graphic3d_AspectFillArea3d) asp = new Graphic3d_AspectFillArea3d();
        asp->SetInteriorStyle(Aspect_IS_SOLID);
        asp->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
        asp->SetFaceCulling(Graphic3d_TypeOfBackfacingModel_DoubleSided);
        Graphic3d_MaterialAspect mat(Graphic3d_NameOfMaterial_UserDefined);
        mat.SetColor(col);
        mat.SetTransparency(1.0f - alpha);
        asp->SetFrontMaterial(mat);
        asp->SetBackMaterial(mat);
        asp->SetInteriorColor(Quantity_ColorRGBA(col, alpha));
        asp->SetAlphaMode(Graphic3d_AlphaMode_Blend);
        asp->SetDrawEdges(false);
        Handle(Graphic3d_Group) g = prs->NewGroup();
        g->SetGroupPrimitivesAspect(asp);
        g->AddPrimitiveArray(arr);
      }
    }
  }
};

// ======================================================================== view controller
class App;
class ViewCtl : public AIS_ViewController {
public:
  explicit ViewCtl(App* a) : app(a) {
    myMouseGestureMap.Clear();
    myMouseGestureMap.Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_RotateOrbit);
    myMouseGestureMap.Bind(Aspect_VKeyMouse_MiddleButton, AIS_MouseGesture_Pan);
    myMouseGestureMap.Bind(Aspect_VKeyMouse_RightButton, AIS_MouseGesture_Pan);
    myMouseGestureMap.Bind(Aspect_VKeyMouse_LeftButton | Aspect_VKeyFlags_CTRL, AIS_MouseGesture_Pan);
    myMouseGestureMap.Bind(Aspect_VKeyMouse_LeftButton | Aspect_VKeyFlags_SHIFT, AIS_MouseGesture_Pan);
    SetRotationMode(AIS_RotationMode_BndBoxScene);
    myScrollZoomRatio = 15.0f;
    SetStickToRayOnZoom(true);
    SetStickToRayOnRotation(true);
    SetAllowDragging(false);
  }
  Handle(AIS_AnimationCamera) animation() { return myViewAnimation; }
  bool hasPivot = false;
  gp_Pnt pivot;
  /** Rotation centre: the double-clicked point, otherwise the centre of the model. */
  gp_Pnt GravityPoint(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override {
    if (hasPivot) return pivot;
    return AIS_ViewController::GravityPoint(ctx, view);
  }
  bool wantsNextFrame() const { return myToAskNextFrame; }
  Graphic3d_Vec2i pickPx;
  int pickMods = 0;
  /** Left click without drag: features (measure …) may take it instead of selection. */
  void handleSelectionPick(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override {
    if (myGL.Selection.Tool == AIS_ViewSelectionTool_Picking && !myGL.Selection.Points.IsEmpty()) {
      const Graphic3d_Vec2i p = myGL.Selection.Points.Last();
      if (feat::onPick(*app, p, pickMods)) { myGL.Selection.Points.Clear(); return; }
    }
    AIS_ViewController::handleSelectionPick(ctx, view);
  }
  bool animating() const { return !myViewAnimation.IsNull() && !myViewAnimation->IsStopped(); }

protected:
  void handleViewRedraw(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override {
    AIS_ViewController::handleViewRedraw(ctx, view);
  }

private:
  App* app;
};

// ======================================================================== material
/** Neutral CAD material: the part colour drives diffuse and ambient, with a soft highlight. */
inline Graphic3d_MaterialAspect cadMaterial() {
  Graphic3d_MaterialAspect m(Graphic3d_NameOfMaterial_UserDefined);
  m.SetMaterialType(Graphic3d_MATERIAL_ASPECT);
  m.SetAmbientColor(Quantity_Color(0.55, 0.55, 0.55, Quantity_TOC_RGB));
  m.SetDiffuseColor(Quantity_Color(0.80, 0.80, 0.80, Quantity_TOC_RGB));
  m.SetSpecularColor(Quantity_Color(0.16, 0.16, 0.16, Quantity_TOC_RGB));
  m.SetEmissiveColor(Quantity_Color(0, 0, 0, Quantity_TOC_RGB));
  m.SetShininess(0.45f);
  return m;
}

// ======================================================================== app
inline const ImU32 kAccent = IM_COL32(10, 132, 255, 255);

class App {
public:
  GLFWwindow* win = nullptr;
  Handle(Aspect_DisplayConnection) disp;
  Handle(OpenGl_GraphicDriver) driver;
  Handle(V3d_Viewer) viewer;
  Handle(V3d_View) view;
  Handle(AIS_InteractiveContext) ctx;
  Handle(Aspect_NeutralWindow) occtWin;
  Handle(AIS_ViewCube) cube;
  std::unique_ptr<ViewCtl> ctl;
  std::vector<std::unique_ptr<Doc>> docs;
  int active = -1;
  Settings set;
  bool night = false;
  bool xray = false;
  float scale = 1.f;
  int framesLeft = 4;
  bool showSettings = false;
  std::string toast;
  double toastUntil = 0;
  bool toastErr = false;
  // test / automation
  std::string shotPath;
  int shotFrames = -1;
  Blur blur;
  int frameNo = 0;
  std::map<std::string, ImVec4> panelRects; // for shadows
  Ui ui;

  // -------------------------------------------------------------- setup
  bool init(int w, int h) {
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    win = glfwCreateWindow(w, h, "GlassView", nullptr, nullptr);
    if (!win) return false;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetWindowUserPointer(win, this);
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(win, &sx, &sy);
    scale = std::max(1.f, sx);

    // ---- OpenCASCADE on the GLFW context
#ifdef _WIN32
    disp = new Aspect_DisplayConnection();
#else
    disp = new Aspect_DisplayConnection((Aspect_XDisplay*)glfwGetX11Display());
#endif
    driver = new OpenGl_GraphicDriver(disp, false);
    driver->ChangeOptions().buffersNoSwap = true;   // GLFW swaps after the interface is drawn
    driver->ChangeOptions().buffersOpaqueAlpha = true;
    viewer = new V3d_Viewer(driver);
    // studio lighting: soft ambient, a key light that follows the camera, a fill from above
    {
      Handle(V3d_AmbientLight) amb = new V3d_AmbientLight(Quantity_NOC_WHITE);
      amb->SetIntensity(0.62f);
      Handle(V3d_DirectionalLight) key = new V3d_DirectionalLight(gp_Dir(0.35, -0.55, -1.0), Quantity_NOC_WHITE, true);
      key->SetIntensity(0.62f);
      Handle(V3d_DirectionalLight) top = new V3d_DirectionalLight(gp_Dir(-0.2, 0.3, -1.0), Quantity_NOC_WHITE, false);
      top->SetIntensity(0.22f);
      for (const Handle(V3d_Light)& l : {Handle(V3d_Light)(amb), Handle(V3d_Light)(key), Handle(V3d_Light)(top)}) { viewer->AddLight(l); viewer->SetLightOn(l); }
    }
    viewer->SetDefaultShadingModel(Graphic3d_TypeOfShadingModel_Phong);
    ctx = new AIS_InteractiveContext(viewer);
    view = viewer->CreateView();
    view->SetImmediateUpdate(false);
    occtWin = new Aspect_NeutralWindow();
#ifdef _WIN32
    occtWin->SetNativeHandle((Aspect_Drawable)glfwGetWin32Window(win));
    Aspect_RenderingContext glCtx = (Aspect_RenderingContext)glfwGetWGLContext(win);
#else
    occtWin->SetNativeHandle((Aspect_Drawable)glfwGetX11Window(win));
    Aspect_RenderingContext glCtx = (Aspect_RenderingContext)glfwGetGLXContext(win);
#endif
    int fw, fh;
    glfwGetFramebufferSize(win, &fw, &fh);
    occtWin->SetSize(fw, fh);
    view->SetWindow(occtWin, glCtx);
    view->ChangeRenderingParams().NbMsaaSamples = 4;
    view->ChangeRenderingParams().IsAntialiasingEnabled = true;
    view->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);
    setupDrawers();
    ctl = std::make_unique<ViewCtl>(this);

    cube = new AIS_ViewCube();
    cube->SetSize(56 * scale);
    cube->SetRoundRadius(0.42);
    cube->SetBoxFacetExtension(56 * scale * 0.035);
    cube->SetBoxEdgeGap(0);
    cube->SetBoxEdgeMinSize(2 * scale);
    cube->SetBoxCornerMinSize(2 * scale);
    cube->SetFontHeight(10.5 * scale);
    cube->SetTextColor(Quantity_Color(0.38, 0.40, 0.45, Quantity_TOC_sRGB));
    cube->SetBoxColor(Quantity_Color(0.985, 0.988, 0.992, Quantity_TOC_sRGB));
    cube->SetAxesPadding(5 * scale);
    cube->SetAxesRadius(0.9 * scale);
    cube->SetAxesConeRadius(2.6 * scale);
    cube->SetAxesSphereRadius(2.4 * scale);
    cube->SetDrawAxes(true);
    {
      // soft studio look: mostly ambient, gentle shading, like the old rounded cube
      Graphic3d_MaterialAspect cm(Graphic3d_NameOfMaterial_UserDefined);
      cm.SetMaterialType(Graphic3d_MATERIAL_ASPECT);
      cm.SetAmbientColor(Quantity_Color(0.80, 0.80, 0.80, Quantity_TOC_RGB));
      cm.SetDiffuseColor(Quantity_Color(0.30, 0.30, 0.30, Quantity_TOC_RGB));
      cm.SetEmissiveColor(Quantity_Color(0.30, 0.30, 0.31, Quantity_TOC_RGB));
      cm.SetSpecularColor(Quantity_Color(0.08, 0.08, 0.08, Quantity_TOC_RGB));
      cm.SetShininess(0.3f);
      cube->SetMaterial(cm);
      cube->SetBoxColor(Quantity_Color(0.985, 0.988, 0.992, Quantity_TOC_sRGB));
      Handle(Prs3d_Drawer) hl = new Prs3d_Drawer();
      hl->SetLink(ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic));
      hl->SetColor(Quantity_Color(0.55, 0.76, 1.0, Quantity_TOC_sRGB));
      hl->SetDisplayMode(0);
      hl->SetMethod(Aspect_TOHM_COLOR);
      cube->SetDynamicHilightAttributes(hl);
    }
    cube->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i((int)(90 * scale), (int)(150 * scale))));
    cube->SetViewAnimation(ctl->animation());
    cube->SetFixedAnimationLoop(false);
    cube->SetAutoStartAnimation(true);
    cube->SetDuration(0.35);
    {
      Handle(Prs3d_DatumAspect) da = new Prs3d_DatumAspect();
      cube->Attributes()->SetDatumAspect(da);
      const Quantity_Color ax[3] = {Quantity_Color(0.93, 0.27, 0.23, Quantity_TOC_sRGB), Quantity_Color(0.19, 0.72, 0.35, Quantity_TOC_sRGB), Quantity_Color(0.04, 0.52, 1.0, Quantity_TOC_sRGB)};
      const Prs3d_DatumParts parts[3] = {Prs3d_DatumParts_XAxis, Prs3d_DatumParts_YAxis, Prs3d_DatumParts_ZAxis};
      for (int i = 0; i < 3; i++) {
        if (!da->TextAspect(parts[i]).IsNull()) {
          da->TextAspect(parts[i])->SetColor(ax[i]);
          da->TextAspect(parts[i])->SetFont(Font_NOF_SANS_SERIF);
          da->TextAspect(parts[i])->SetHeight(11 * scale);
          da->TextAspect(parts[i])->Aspect()->SetFont("Segoe UI");
          da->TextAspect(parts[i])->Aspect()->SetTextFontAspect(Font_FontAspect_Bold);
        }
        if (!da.IsNull() && !da->ShadingAspect(parts[i]).IsNull()) da->ShadingAspect(parts[i])->SetColor(ax[i]);
      }
    }
    ctx->Display(cube, false);

    // ---- ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ui.init(scale);
    applyProjection();
    applyTheme();
    installCallbacks();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(nullptr);
    return true;
  }

  void setupDrawers() {
    Handle(Prs3d_Drawer) d = ctx->DefaultDrawer();
    d->SetAutoTriangulation(false);
    d->SetFaceBoundaryDraw(true);
    d->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0));
    d->UIsoAspect()->SetNumber(0);
    d->VIsoAspect()->SetNumber(0);
    d->ShadingAspect()->SetMaterial(cadMaterial());
    d->SetTypeOfDeflection(Aspect_TOD_RELATIVE);
    // selection & hover
    Handle(Prs3d_Drawer) sel = ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected);
    sel->SetColor(Quantity_Color(0.04, 0.52, 1.0, Quantity_TOC_sRGB));
    sel->SetDisplayMode(-1); // highlight in the object's own mode (shaded or wireframe)
    sel->SetTransparency(0.0f);
    Handle(Prs3d_Drawer) hov = ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic);
    hov->SetColor(Quantity_Color(0.62, 0.80, 1.0, Quantity_TOC_sRGB));
    hov->SetDisplayMode(-1);
    hov->SetTransparency(0.0f);
    ctx->SetPixelTolerance((int)(4 * scale));
  }

  // -------------------------------------------------------------- input
  static App* self(GLFWwindow* w) { return (App*)glfwGetWindowUserPointer(w); }
  Aspect_VKeyFlags mods(int m) const {
    Aspect_VKeyFlags f = Aspect_VKeyFlags_NONE;
    if (m & GLFW_MOD_SHIFT) f |= Aspect_VKeyFlags_SHIFT;
    if (m & GLFW_MOD_CONTROL) f |= Aspect_VKeyFlags_CTRL;
    if (m & GLFW_MOD_ALT) f |= Aspect_VKeyFlags_ALT;
    return f;
  }
  Graphic3d_Vec2i cursorPx() const {
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    int ww, wh, fw, fh;
    glfwGetWindowSize(win, &ww, &wh);
    glfwGetFramebufferSize(win, &fw, &fh);
    return Graphic3d_Vec2i((int)(x * fw / std::max(1, ww)), (int)(y * fh / std::max(1, wh)));
  }
  Aspect_VKeyMouse btn(int b) const {
    return b == GLFW_MOUSE_BUTTON_LEFT ? Aspect_VKeyMouse_LeftButton : b == GLFW_MOUSE_BUTTON_RIGHT ? Aspect_VKeyMouse_RightButton : Aspect_VKeyMouse_MiddleButton;
  }

  void installCallbacks() {
    glfwSetFramebufferSizeCallback(win, [](GLFWwindow* w, int fw, int fh) {
      App* a = self(w);
      if (fw <= 0 || fh <= 0) return;
      a->occtWin->SetSize(fw, fh);
      a->view->MustBeResized();
      a->view->Invalidate();
      a->framesLeft = 3;
    });
    glfwSetWindowRefreshCallback(win, [](GLFWwindow* w) { self(w)->framesLeft = 2; });
    glfwSetCursorPosCallback(win, [](GLFWwindow* w, double, double) {
      App* a = self(w);
      a->framesLeft = std::max(a->framesLeft, 2);
      a->poke();
      if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse && a->ctl->PressedMouseButtons() == Aspect_VKeyMouse_NONE) {
        a->ctx->ClearDetected(false);
        return;
      }
      a->ctl->UpdateMousePosition(a->cursorPx(), a->ctl->PressedMouseButtons(), a->ctl->LastMouseFlags(), false);
    });
    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int b, int action, int m) {
      App* a = self(w);
      a->framesLeft = std::max(a->framesLeft, 3);
      if (action == GLFW_PRESS && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return;
      if (action == GLFW_PRESS) {
        a->showSettingsClose();
        if (b == GLFW_MOUSE_BUTTON_LEFT) {
          const double t = glfwGetTime();
          const Graphic3d_Vec2i c = a->cursorPx();
          if (t - a->lastClickT < 0.35 && std::abs(c.x() - a->lastClick.x()) + std::abs(c.y() - a->lastClick.y()) < 8 * a->scale) {
            a->setPivotAt(c);
            a->lastClickT = 0;
            return;
          }
          a->lastClickT = t;
          a->lastClick = c;
        }
        if (b == GLFW_MOUSE_BUTTON_RIGHT) { a->rightDown = a->cursorPx(); a->rightArmed = true; }
        a->ctl->pickMods = m;
        a->ctl->PressMouseButton(a->cursorPx(), a->btn(b), a->mods(m), false);
      } else if (a->ctl->PressedMouseButtons() != Aspect_VKeyMouse_NONE) {
        a->ctl->ReleaseMouseButton(a->cursorPx(), a->btn(b), a->mods(m), false);
        if (b == GLFW_MOUSE_BUTTON_RIGHT && a->rightArmed) {
          const Graphic3d_Vec2i c = a->cursorPx();
          if (std::abs(c.x() - a->rightDown.x()) + std::abs(c.y() - a->rightDown.y()) < 5 * a->scale) feat::onRightClick(*a, c);
          a->rightArmed = false;
        }
      }
    });
    glfwSetScrollCallback(win, [](GLFWwindow* w, double, double dy) {
      App* a = self(w);
      a->framesLeft = std::max(a->framesLeft, 3);
      if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return;
      a->ctl->UpdateMouseScroll(Aspect_ScrollDelta(a->cursorPx(), (a->set.wheelReverse ? -1.0 : 1.0) * dy * a->set.zoomSpeed / 100.0));
    });
    glfwSetKeyCallback(win, [](GLFWwindow* w, int key, int, int action, int m) {
      App* a = self(w);
      a->framesLeft = std::max(a->framesLeft, 3);
      if (action == GLFW_RELEASE) { feat::onKey(*a, key, m | 0x10000); return; }
      if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
      feat::onKey(*a, key, m);
    });
    glfwSetDropCallback(win, [](GLFWwindow* w, int n, const char** paths) {
      App* a = self(w);
      for (int i = 0; i < n; i++) a->open(paths[i]);
      a->framesLeft = 4;
    });
  }

  Graphic3d_Vec2i rightDown;
  bool rightArmed = false;
  double lastClickT = 0;
  Graphic3d_Vec2i lastClick;
  double pivotShownUntil = 0;
  Handle(GridPrs) grid;

  /** Double-click: rotate around the clicked point; on empty space back to the model centre. */
  void setPivotAt(const Graphic3d_Vec2i& px) {
    const Handle(StdSelect_ViewerSelector3d)& sel = ctx->MainSelector();
    sel->Pick(px.x(), px.y(), view);
    bool hit = false;
    for (int i = 1; i <= sel->NbPicked(); i++) {
      Handle(SelectMgr_EntityOwner) o = sel->Picked(i);
      if (o.IsNull() || o->Selectable() == cube) continue;
      ctl->pivot = sel->PickedData(i).Point;
      hit = true;
      break;
    }
    ctl->hasPivot = hit;
    pivotShownUntil = glfwGetTime() + 1.4;
    if (!hit) toastMsg("Rotation centre: model centre", false, 1.5);
    framesLeft = 4;
    poke();
  }

  void showSettingsClose() {
    if (showSettings && !ImGui::GetIO().WantCaptureMouse) showSettings = false;
  }

  void onKey(int key, int m) {
    const bool ctrl = m & GLFW_MOD_CONTROL, shift = m & GLFW_MOD_SHIFT;
    if (ctrl) {
      if (key == GLFW_KEY_O) openDialog();
      else if (key == GLFW_KEY_W && active >= 0) closeDoc(active);
      else if (key == GLFW_KEY_TAB && docs.size() > 1) activate((active + (shift ? (int)docs.size() - 1 : 1)) % (int)docs.size());
      return;
    }
    switch (key) {
      case GLFW_KEY_F: fitAll(true); break;
      case GLFW_KEY_H: case GLFW_KEY_0: case GLFW_KEY_KP_0: animateTo(V3d_XposYnegZpos); break;
      case GLFW_KEY_7: case GLFW_KEY_KP_7: animateTo(V3d_Zpos); break;
      case GLFW_KEY_8: case GLFW_KEY_KP_8: animateTo(V3d_Xpos); break;
      case GLFW_KEY_9: case GLFW_KEY_KP_9: animateTo(V3d_Yneg); break;
      case GLFW_KEY_1: setDisplayMode(0); break;
      case GLFW_KEY_2: setDisplayMode(1); break;
      case GLFW_KEY_3: setDisplayMode(2); break;
      case GLFW_KEY_4: setDisplayMode(3); break;
      case GLFW_KEY_X: setXray(!xray); break;
      case GLFW_KEY_P: set.ortho = !set.ortho; applyProjection(); set.save(); break;
      case GLFW_KEY_E: set.catiaEdges = !set.catiaEdges; applyEdgeStyle(); set.save(); break;
      case GLFW_KEY_T: set.tree = !set.tree; set.save(); break;
      case GLFW_KEY_B: set.theme = night ? 0 : 1; applyTheme(); set.save(); break;
      case GLFW_KEY_COMMA: showSettings = !showSettings; break;
      case GLFW_KEY_ESCAPE: ctx->ClearSelected(false); showSettings = false; if (active >= 0) docs[active]->selected = nullptr; break;
      default: return;
    }
    view->Invalidate();
  }

  // -------------------------------------------------------------- view helpers
  void applyProjection() {
    view->Camera()->SetProjectionType(set.ortho ? Graphic3d_Camera::Projection_Orthographic : Graphic3d_Camera::Projection_Perspective);
    if (!set.ortho) view->Camera()->SetFOVy(32.0);
    view->Invalidate();
  }

  void applyTheme() {
    night = set.theme == 1 || (set.theme == 2 && systemPrefersDark());
    if (night) {
      view->SetBgGradientColors(Quantity_Color(0.215, 0.240, 0.290, Quantity_TOC_sRGB), Quantity_Color(0.075, 0.085, 0.110, Quantity_TOC_sRGB), Aspect_GradientFillMethod_Vertical, false);
      cube->SetBoxColor(Quantity_Color(0.80, 0.82, 0.86, Quantity_TOC_sRGB));
    } else {
      view->SetBgGradientColors(Quantity_Color(0.975, 0.98, 0.99, Quantity_TOC_sRGB), Quantity_Color(0.80, 0.835, 0.88, Quantity_TOC_sRGB), Aspect_GradientFillMethod_Vertical, false);
      cube->SetBoxColor(Quantity_Color(0.97, 0.975, 0.985, Quantity_TOC_sRGB));
    }
    ctx->Redisplay(cube, false);
    ui.setTheme(night);
    if (!grid.IsNull()) updateGrid();
#ifdef _WIN32
    int dark = night ? 1 : 0;
    DwmSetWindowAttribute((void*)glfwGetWin32Window(win), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
#endif
    applyEdgeStyle();
  }

  Quantity_Color edgeColor() const { return night && set.displayMode == 2 ? Quantity_Color(0.90, 0.92, 0.95, Quantity_TOC_sRGB) : Quantity_Color(0.03, 0.035, 0.045, Quantity_TOC_sRGB); }
  Quantity_Color softColor() const { return night && set.displayMode == 2 ? Quantity_Color(0.45, 0.48, 0.53, Quantity_TOC_sRGB) : Quantity_Color(0.56, 0.59, 0.64, Quantity_TOC_sRGB); }

  /** Fit the model into the part of the window not covered by the tree, toolbars and tabs. */
  void fitFree(double margin) {
    view->FitAll(margin, false);
    int fw, fh, ww, wh;
    glfwGetFramebufferSize(win, &fw, &fh);
    glfwGetWindowSize(win, &ww, &wh);
    if (fw <= 0 || fh <= 0 || ww <= 0) return;
    const double px = (double)fw / ww; // framebuffer pixels per UI unit
    const double left = (set.tree && active >= 0 ? (12 + 270 + 10) * scale : 0) * px;
    const double top = 64 * scale * px, bottom = 64 * scale * px;
    const double fx = (fw - left) / fw, fy = (fh - top - bottom) / fh;
    const double k = 1.0 / std::max(0.2, std::min(fx, fy));
    Handle(Graphic3d_Camera) cam = view->Camera();
    cam->SetScale(cam->Scale() * k);
    // shift so the model sits in the middle of the free area
    const double wpp = cam->Scale() / fh; // world units per framebuffer pixel (height based)
    const double aspect = (double)fw / fh;
    (void)aspect;
    const gp_Dir dir = cam->Direction(), up = cam->Up();
    const gp_Vec side = gp_Vec(dir).Crossed(gp_Vec(up));
    const double dx = left * 0.5 * wpp, dy = (top - bottom) * 0.5 * wpp;
    const gp_Vec shift = side.Normalized() * (-dx) + gp_Vec(up) * dy;
    cam->SetEyeAndCenter(cam->Eye().Translated(shift), cam->Center().Translated(shift));
    view->ZFitAll();
  }

  void fitAll(bool animate) {
    if (ctl) ctl->hasPivot = false;
    if (!animate || !ctl) { fitFree(0.04); view->Invalidate(); return; }
    Handle(Graphic3d_Camera) start = new Graphic3d_Camera(); start->Copy(view->Camera());
    fitFree(0.04);
    Handle(Graphic3d_Camera) end = new Graphic3d_Camera(); end->Copy(view->Camera());
    view->Camera()->Copy(start);
    startAnimation(start, end);
  }

  void animateTo(V3d_TypeOfOrientation o) {
    Handle(Graphic3d_Camera) start = new Graphic3d_Camera(); start->Copy(view->Camera());
    view->SetProj(o, false);
    fitFree(0.04);
    Handle(Graphic3d_Camera) end = new Graphic3d_Camera(); end->Copy(view->Camera());
    view->Camera()->Copy(start);
    startAnimation(start, end);
  }

  void startAnimation(const Handle(Graphic3d_Camera)& a, const Handle(Graphic3d_Camera)& b) {
    Handle(AIS_AnimationCamera) an = ctl->animation();
    an->Stop();
    an->SetView(view);
    an->SetCameraStart(a);
    an->SetCameraEnd(b);
    an->SetOwnDuration(0.35);
    an->StartTimer(0.0, 1.0, true, false);
    framesLeft = 3;
  }

  // -------------------------------------------------------------- documents
  void openDialog() {
#ifdef _WIN32
    for (auto& p : openFileDialog(glfwGetWin32Window(win))) open(p);
#else
    toastMsg("No file dialog in this build – drop a file or pass it on the command line", true, 4);
#endif
  }

  void open(const std::string& path) {
    if (!isSupported(path)) { toastMsg("Unsupported file: " + fileNameOf(path), true, 4); return; }
    addRecent(*this, path);
    auto d = std::make_unique<Doc>();
    d->path = path;
    d->name = fileNameOf(path);
    d->prog = std::make_shared<LoadProgress>();
    d->result = std::make_shared<std::shared_ptr<Model>>();
    d->mtx = std::make_shared<std::mutex>();
    d->loading = true;
    auto prog = d->prog; auto res = d->result; auto mtx = d->mtx;
    Quality q = (Quality)set.quality;
    d->worker = std::thread([path, q, prog, res, mtx]() {
      auto m = loadModel(path, q, *prog);
      { std::lock_guard<std::mutex> lk(*mtx); *res = m; }
      glfwPostEmptyEvent(); // wake the UI thread
    });
    docs.push_back(std::move(d));
    activate((int)docs.size() - 1);
  }

  void retessellate() {
    size_t n = 0;
    for (auto& dp : docs) {
      Doc& d = *dp;
      if (d.loading || !d.model) continue;
      n += remesh(*d.model, (Quality)set.quality);
      for (auto& p : d.prs) if (!p.IsNull()) { p->SetToUpdate(); if (ctx->IsDisplayed(p)) ctx->Redisplay(p, false); }
    }
    if (!docs.empty()) toastMsg("Re-tessellated · " + std::to_string(n) + " triangles", false, 2.5);
    view->Invalidate();
    framesLeft = 4;
  }

  void closeDoc(int i) {
    if (i < 0 || i >= (int)docs.size()) return;
    Doc& d = *docs[i];
    for (auto& p : d.prs) if (!p.IsNull()) ctx->Remove(p, false);
    for (auto& p : d.soft) if (!p.IsNull()) ctx->Remove(p, false);
    docs.erase(docs.begin() + i);
    active = -1;
    if (!docs.empty()) activate(std::min(i, (int)docs.size() - 1));
    else { updateGrid(); feat::onDocShown(*this); }
    view->Invalidate();
  }

  void activate(int i) {
    if (active >= 0 && active < (int)docs.size() && active != i) {
      Doc& o = *docs[active];
      if (o.displayed) {
        o.camera = new Graphic3d_Camera();
        o.camera->Copy(view->Camera());
        for (auto& p : o.prs) if (!p.IsNull()) ctx->Erase(p, false);
        for (auto& p : o.soft) if (!p.IsNull()) ctx->Erase(p, false);
      }
    }
    active = i;
    ctx->ClearSelected(false);
    if (i >= 0) {
      Doc& d = *docs[i];
      if (d.displayed) {
        showVisible(d);
        if (!d.camera.IsNull()) { view->Camera()->Copy(d.camera); applyProjection(); }
      }
    }
    glfwSetWindowTitle(win, i >= 0 ? (docs[i]->name + " — GlassView").c_str() : "GlassView");
    ctl->hasPivot = false;
    updateGrid();
    feat::onDocShown(*this);
    view->Invalidate();
    framesLeft = 3;
  }

  /** Called every frame: moves finished background loads onto the screen. */
  void pollLoads() {
    for (int i = 0; i < (int)docs.size(); i++) {
      Doc& d = *docs[i];
      if (!d.loading) continue;
      std::shared_ptr<Model> m;
      { std::lock_guard<std::mutex> lk(*d.mtx); m = *d.result; }
      if (!m) continue;
      if (d.worker.joinable()) d.worker.join();
      d.loading = false;
      if (!m->error.empty()) {
        toastMsg(d.name + ": " + m->error, true, 6);
        closeDoc(i);
        return;
      }
      d.model = m;
      const double tb0 = glfwGetTime();
      build(d);
      const double tb1 = glfwGetTime();
      if (i == active) { showVisible(d); ctl->hasPivot = false; updateGrid(); fitAll(false); feat::onDocShown(*this); }
      if (getenv("GV_PROFILE"))
        fprintf(stderr, "[profile] %s: read %.0f ms, mesh %.0f ms, worker total %.0f ms, build %.0f ms, display %.0f ms\n", d.name.c_str(),
                m->readMs, m->meshMs, m->totalMs, (tb1 - tb0) * 1000, (glfwGetTime() - tb1) * 1000);
      profT = glfwGetTime();
      framesLeft = 4;
    }
  }

  void build(Doc& d, size_t first = 0) {
    const Model& m = *d.model;
    d.prs.resize(m.parts.size());
    d.soft.resize(m.parts.size());
    for (size_t k = first; k < m.parts.size(); k++) {
      const Part& p = m.parts[k];
      Handle(AIS_ColoredShape) s = new AIS_ColoredShape(p.shape);
      for (const auto& sc : p.subColors) s->SetCustomColor(sc.first, sc.second);
      s->SetLocalTransformation(p.loc.Transformation());
      s->SetColor(p.hasColor ? p.color : Quantity_Color(0.62, 0.68, 0.76, Quantity_TOC_sRGB));
      s->SetMaterial(cadMaterial());
      d.prs[k] = s;
      d.partOf[s.get()] = (int)k;
      if (p.hasSoft) {
        Handle(AIS_Shape) e = new AIS_Shape(p.softEdges);
        e->SetLocalTransformation(p.loc.Transformation());
        e->SetColor(softColor());
        e->SetWidth(1.0);
        e->SetDisplayMode(AIS_WireFrame);
        d.soft[k] = e;
      }
      styleObject(d, (int)k);
    }
    d.displayed = true;
  }

  void styleObject(Doc& d, int k) {
    const Handle(AIS_Shape)& s = d.prs[k];
    Handle(Prs3d_Drawer) dr = s->Attributes();
    const bool edges = set.displayMode == 1 || set.displayMode == 2;
    dr->SetFaceBoundaryDraw(set.displayMode == 1 && !d.model->parts[k].isMesh);
    dr->SetFaceBoundaryAspect(new Prs3d_LineAspect(edgeColor(), Aspect_TOL_SOLID, set.catiaEdges ? 1.25 : 1.0));
    dr->SetFaceBoundaryUpperContinuity(set.catiaEdges ? GeomAbs_C0 : GeomAbs_CN);
    dr->ShadingAspect()->Aspect()->SetDrawEdges(set.displayMode == 3);
    dr->ShadingAspect()->Aspect()->SetEdgeColor(Quantity_Color(0.05, 0.06, 0.08, Quantity_TOC_sRGB));
    dr->ShadingAspect()->Aspect()->SetEdgeWidth(1.0);
    dr->ShadingAspect()->SetTransparency(xray ? 0.7 : 0.0);
    if (set.displayMode == 2) {
      dr->SetWireAspect(new Prs3d_LineAspect(edgeColor(), Aspect_TOL_SOLID, 1.0));
      dr->SetFreeBoundaryAspect(new Prs3d_LineAspect(edgeColor(), Aspect_TOL_SOLID, 1.0));
      dr->SetUnFreeBoundaryAspect(new Prs3d_LineAspect(edgeColor(), Aspect_TOL_SOLID, 1.0));
    }
    (void)edges;
    if (!d.soft[k].IsNull()) d.soft[k]->SetColor(softColor());
  }

  bool nodeVisible(const Node* n) const { for (; n; n = n->parent) if (!n->visible) return false; return true; }

  void showVisible(Doc& d) {
    if (!d.displayed) return;
    std::vector<char> vis(d.prs.size(), 0);
    std::function<void(Node*)> walk = [&](Node* n) {
      if (nodeVisible(n)) for (int p : n->parts) vis[p] = 1;
      for (auto& k : n->kids) walk(k.get());
    };
    walk(d.model->root.get());
    const int mode = set.displayMode == 2 ? AIS_WireFrame : AIS_Shaded;
    for (size_t k = 0; k < d.prs.size(); k++) {
      if (vis[k]) {
        if (ctx->IsDisplayed(d.prs[k])) ctx->SetDisplayMode(d.prs[k], mode, false);
        else ctx->Display(d.prs[k], mode, 0, false);
      } else ctx->Erase(d.prs[k], false);
      const bool softOn = vis[k] && set.catiaEdges && (set.displayMode == 1 || set.displayMode == 2) && !d.soft[k].IsNull();
      if (!d.soft[k].IsNull()) {
        if (softOn) { if (!ctx->IsDisplayed(d.soft[k])) ctx->Display(d.soft[k], AIS_WireFrame, -1, false); }
        else ctx->Erase(d.soft[k], false);
      }
    }
    feat::onDocShown(*this);
    view->Invalidate();
  }

  void restyleAll() {
    for (auto& dp : docs) {
      Doc& d = *dp;
      if (!d.displayed) continue;
      for (int k = 0; k < (int)d.prs.size(); k++) {
        styleObject(d, k);
        if (ctx->IsDisplayed(d.prs[k])) ctx->Redisplay(d.prs[k], false);
        if (!d.soft[k].IsNull() && ctx->IsDisplayed(d.soft[k])) ctx->Redisplay(d.soft[k], false);
      }
    }
    if (active >= 0) showVisible(*docs[active]);
    view->Invalidate();
    framesLeft = 3;
  }

  double gridScale = 0;
  double gridPixelWidth() {
    int fw, fh;
    glfwGetFramebufferSize(win, &fw, &fh);
    return view->Camera()->Scale() / std::max(1, fh) * 1.15;
  }
  /** Keep grid lines about one pixel wide while zooming. */
  void maintainGrid() {
    if (grid.IsNull() || !ctx->IsDisplayed(grid)) return;
    const double sc = view->Camera()->Scale();
    if (gridScale > 0 && sc / gridScale < 1.25 && gridScale / sc < 1.25) return;
    grid->width = gridPixelWidth();
    gridScale = sc;
    ctx->Redisplay(grid, false);
    grid->SetInfiniteState(false);
    grid->SetInfiniteState(true);
  }

  /** Grid under the visible model of the active tab, spacing picked from the model size. */
  void updateGrid() {
    if (grid.IsNull()) grid = new GridPrs();
    Doc* d = active >= 0 ? docs[active].get() : nullptr;
    if (!set.grid || !d || !d->displayed || d->model->box.IsVoid()) { if (ctx->IsDisplayed(grid)) ctx->Erase(grid, false); return; }
    double x0, y0, z0, x1, y1, z1;
    d->model->box.Get(x0, y0, z0, x1, y1, z1);
    const double size = std::max({x1 - x0, y1 - y0, (z1 - z0) * 0.5, 1e-3});
    grid->step = std::pow(10.0, std::floor(std::log10(size / 8.0)));
    if (size / grid->step > 60) grid->step *= 5;
    grid->extent = std::max(size * 2.2, grid->step * 12);
    grid->cx = (x0 + x1) * 0.5; grid->cy = (y0 + y1) * 0.5;
    grid->z = z0 - size * 1e-4;
    grid->night = night;
    grid->width = gridPixelWidth();
    gridScale = view->Camera()->Scale();
    if (ctx->IsDisplayed(grid)) ctx->Redisplay(grid, false);
    else ctx->Display(grid, 0, -1, false);
    // OCCT only flags presentations that exist when the state is set: re-apply after (re)display
    grid->SetInfiniteState(false);
    grid->SetInfiniteState(true);
    view->Invalidate();
  }

  void setDisplayMode(int m) { set.displayMode = m; set.save(); restyleAll(); }
  void applyEdgeStyle() { restyleAll(); }
  void setXray(bool on) { xray = on; restyleAll(); }

  void toastMsg(const std::string& s, bool err, double secs) {
    toast = s; toastErr = err; toastUntil = glfwGetTime() + secs; framesLeft = 3;
  }

  // selection sync: view -> tree
  void syncSelection() {
    if (active < 0) return;
    Doc& d = *docs[active];
    if (!d.displayed) return;
    ctx->InitSelected();
    if (!ctx->MoreSelected()) return;
    Handle(AIS_InteractiveObject) o = ctx->SelectedInteractive();
    auto it = d.partOf.find(o.get());
    if (it == d.partOf.end()) return;
    const int part = it->second;
    if (d.selected && std::find(d.selected->parts.begin(), d.selected->parts.end(), part) != d.selected->parts.end()) return;
    // find the node holding this part
    std::function<Node*(Node*)> find = [&](Node* n) -> Node* {
      for (int p : n->parts) if (p == part) return n;
      for (auto& k : n->kids) if (Node* r = find(k.get())) return r;
      return nullptr;
    };
    if (Node* n = find(d.model->root.get())) {
      if (d.selected != n) {
        d.selected = n;
        for (Node* p = n->parent; p; p = p->parent) p->open = true;
        ui.scrollToSelected = true;
      }
    }
  }

  void selectNode(Doc& d, Node* n) {
    d.selected = n;
    ctx->ClearSelected(false);
    std::function<void(Node*)> walk = [&](Node* x) {
      for (int p : x->parts) if (ctx->IsDisplayed(d.prs[p])) ctx->AddOrRemoveSelected(d.prs[p], false);
      for (auto& k : x->kids) walk(k.get());
    };
    if (n) walk(n);
    view->Invalidate();
  }

  // -------------------------------------------------------------- frame
  std::vector<std::string> pendingOpen; // files forwarded from a second instance
  std::mutex pendingMtx;

  void frame() {
    {
      std::vector<std::string> p;
      { std::lock_guard<std::mutex> lk(pendingMtx); p.swap(pendingOpen); }
      for (auto& f : p) open(f);
    }
    pollLoads();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    const bool blurOn = set.geti("blur", 1);
    ui.blurTex = blurOn ? blur.texture() : 0;
    drawUi();
    ImGui::Render();
    // 3D first, then the interface on top
    feat::beforeRedraw(*this);
    ctl->FlushViewEvents(ctx, view, true);
    feat::afterFlush(*this);
    maintainGrid();
    syncSelection();
    view->Invalidate();
    view->Redraw();
    if (blurOn) {
      int fw, fh;
      glfwGetFramebufferSize(win, &fw, &fh);
      blur.update(fw, fh);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    frameNo++;
    if (!shotPath.empty() && frameNo >= shotFrames) { screenshot(shotPath); glfwSetWindowShouldClose(win, 1); }
    glfwSwapBuffers(win);
    if (profT > 0 && getenv("GV_PROFILE")) { fprintf(stderr, "[profile] first frame %.0f ms\n", (glfwGetTime() - profT) * 1000); profT = 0; }
  }
  double profT = 0;

  void screenshot(const std::string& path);

  /** Event-driven loop: draws only while something changes, sleeps (0 % CPU) otherwise. */
  void run() {
    while (!glfwWindowShouldClose(win)) {
      const double now = glfwGetTime();
      const bool busy = framesLeft > 0 || now < liveUntil || ctl->animating() || ctl->wantsNextFrame() || !shotPath.empty() || feat::wantsFrames(*this);
      if (busy) glfwPollEvents();
      else if (anyLoading()) glfwWaitEventsTimeout(0.05);
      else if (toastUntil > now) glfwWaitEventsTimeout(toastUntil - now + 0.01);
      else glfwWaitEvents();
      frame();
      if (framesLeft > 0) framesLeft--;
    }
  }
  double liveUntil = 0; // keep drawing briefly after input so hover effects and tooltips settle
  void poke() { liveUntil = glfwGetTime() + 0.8; }

  bool anyLoading() const { for (auto& d : docs) if (d->loading) return true; return false; }

  void shutdown() {
    docs.clear();
    feat::shutdown(*this);
    blur.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    view->Remove();
    glfwDestroyWindow(win);
    glfwTerminate();
  }

  // -------------------------------------------------------------- interface
  void drawUi();
};

