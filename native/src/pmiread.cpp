// GlassView Native – reads STEP AP242 PMI (dimensions, GD&T, datums) into gv::Model.
// No graphics here, so it can also be used by the Explorer preview DLL.
#include "model.hpp"
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <TDF_LabelSequence.hxx>
#include <TCollection_HAsciiString.hxx>
#include <cmath>
#include <cstdio>
using namespace gv;

// ======================================================================== reading
static std::string hstr(const Handle(TCollection_HAsciiString)& h) { return h.IsNull() ? "" : std::string(h->ToCString()); }

static std::string num(double v) {
  char b[48];
  snprintf(b, sizeof b, "%.4g", v);
  return b;
}

static const char* dimTypeName(XCAFDimTolObjects_DimensionType t, std::string& prefix) {
  prefix.clear();
  switch (t) {
    case XCAFDimTolObjects_DimensionType_Size_Diameter: prefix = "Ø"; return "Diameter";
    case XCAFDimTolObjects_DimensionType_Size_SphericalDiameter: prefix = "SØ"; return "Spherical diameter";
    case XCAFDimTolObjects_DimensionType_Size_Radius: prefix = "R"; return "Radius";
    case XCAFDimTolObjects_DimensionType_Size_SphericalRadius: prefix = "SR"; return "Spherical radius";
    case XCAFDimTolObjects_DimensionType_Size_Thickness: return "Thickness";
    case XCAFDimTolObjects_DimensionType_Size_Angular:
    case XCAFDimTolObjects_DimensionType_Location_Angular: return "Angle";
    case XCAFDimTolObjects_DimensionType_Size_CurveLength:
    case XCAFDimTolObjects_DimensionType_Location_CurvedDistance: return "Curve length";
    case XCAFDimTolObjects_DimensionType_Size_ToroidalMinorDiameter:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalMajorDiameter:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalHighMajorDiameter:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalLowMajorDiameter: prefix = "Ø"; return "Toroidal diameter";
    case XCAFDimTolObjects_DimensionType_Size_ToroidalMinorRadius:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalMajorRadius:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalHighMajorRadius:
    case XCAFDimTolObjects_DimensionType_Size_ToroidalLowMajorRadius: prefix = "R"; return "Toroidal radius";
    case XCAFDimTolObjects_DimensionType_CommonLabel:
    case XCAFDimTolObjects_DimensionType_DimensionPresentation: return "Note";
    default: return t >= XCAFDimTolObjects_DimensionType_Location_LinearDistance && t <= XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToInner ? "Distance" : "Dimension";
  }
}

static const char* tolTypeName(XCAFDimTolObjects_GeomToleranceType t) {
  switch (t) {
    case XCAFDimTolObjects_GeomToleranceType_Angularity: return "Angularity";
    case XCAFDimTolObjects_GeomToleranceType_CircularRunout: return "Circular runout";
    case XCAFDimTolObjects_GeomToleranceType_CircularityOrRoundness: return "Circularity";
    case XCAFDimTolObjects_GeomToleranceType_Coaxiality: return "Coaxiality";
    case XCAFDimTolObjects_GeomToleranceType_Concentricity: return "Concentricity";
    case XCAFDimTolObjects_GeomToleranceType_Cylindricity: return "Cylindricity";
    case XCAFDimTolObjects_GeomToleranceType_Flatness: return "Flatness";
    case XCAFDimTolObjects_GeomToleranceType_Parallelism: return "Parallelism";
    case XCAFDimTolObjects_GeomToleranceType_Perpendicularity: return "Perpendicularity";
    case XCAFDimTolObjects_GeomToleranceType_Position: return "Position";
    case XCAFDimTolObjects_GeomToleranceType_ProfileOfLine: return "Profile of a line";
    case XCAFDimTolObjects_GeomToleranceType_ProfileOfSurface: return "Profile of a surface";
    case XCAFDimTolObjects_GeomToleranceType_Straightness: return "Straightness";
    case XCAFDimTolObjects_GeomToleranceType_Symmetry: return "Symmetry";
    case XCAFDimTolObjects_GeomToleranceType_TotalRunout: return "Total runout";
    default: return "Tolerance";
  }
}

static std::string fitClass(const Handle(XCAFDimTolObjects_DimensionObject)& o) {
  if (!o->IsDimWithClassOfTolerance()) return "";
  Standard_Boolean hole = false;
  XCAFDimTolObjects_DimensionFormVariance fv;
  XCAFDimTolObjects_DimensionGrade g;
  if (!o->GetClassOfTolerance(hole, fv, g)) return "";
  static const char* fvs[] = {"", "A", "B", "C", "CD", "D", "E", "EF", "F", "FG", "G", "H", "JS", "J", "K", "M", "N", "P", "R", "S", "T", "U", "V", "X", "Y", "Z", "ZA", "ZB", "ZC"};
  std::string f = (int)fv >= 0 && (int)fv < 29 ? fvs[(int)fv] : "";
  if (!hole) for (auto& c : f) c = (char)tolower((unsigned char)c);
  const int gi = (int)g;
  const std::string grade = gi == 0 ? "01" : std::to_string(gi - 1);
  return f + grade;
}

static void refsOf(const Handle(XCAFDoc_ShapeTool)& st, const TDF_Label& l, std::vector<TopoDS_Shape>& out) {
  TDF_LabelSequence a, b;
  if (!XCAFDoc_DimTolTool::GetRefShapeLabel(l, a, b)) return;
  for (int i = 1; i <= a.Length(); i++) { TopoDS_Shape s = st->GetShape(a.Value(i)); if (!s.IsNull()) out.push_back(s); }
  for (int i = 1; i <= b.Length(); i++) { TopoDS_Shape s = st->GetShape(b.Value(i)); if (!s.IsNull()) out.push_back(s); }
}

void gv::readPmi(gv::Model& m) {
  m.pmi.clear();
  if (m.doc.IsNull()) return;
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(m.doc->Main());
  if (!XCAFDoc_DocumentTool::CheckDimTolTool(m.doc->Main())) return;
  Handle(XCAFDoc_DimTolTool) dt = XCAFDoc_DocumentTool::DimTolTool(m.doc->Main());
  if (dt.IsNull()) return;

  // datums first (tolerances refer to them by name)
  TDF_LabelSequence dl;
  dt->GetDatumLabels(dl);
  for (int i = 1; i <= dl.Length(); i++) {
    Handle(XCAFDoc_Datum) at;
    if (!dl.Value(i).FindAttribute(XCAFDoc_Datum::GetID(), at)) continue;
    Handle(XCAFDimTolObjects_DatumObject) o = at->GetObject();
    if (o.IsNull()) continue;
    Pmi p;
    p.kind = 2;
    p.title = hstr(o->GetName());
    if (p.title.empty()) p.title = "Datum";
    p.detail = "Datum feature";
    p.pres = o->GetPresentation();
    p.hasPt = o->HasPointText();
    if (p.hasPt) p.pt = o->GetPointTextAttach();
    refsOf(st, dl.Value(i), p.refs);
    m.pmi.push_back(p);
  }

  TDF_LabelSequence dims;
  dt->GetDimensionLabels(dims);
  for (int i = 1; i <= dims.Length(); i++) {
    Handle(XCAFDoc_Dimension) at;
    if (!dims.Value(i).FindAttribute(XCAFDoc_Dimension::GetID(), at)) continue;
    Handle(XCAFDimTolObjects_DimensionObject) o = at->GetObject();
    if (o.IsNull()) continue;
    Pmi p;
    p.kind = 0;
    std::string prefix;
    const char* tn = dimTypeName(o->GetType(), prefix);
    const bool angle = std::string(tn) == "Angle";
    std::string val;
    if (o->IsDimWithRange()) val = num(o->GetLowerBound()) + "–" + num(o->GetUpperBound());
    else val = num(o->GetValue());
    if (angle) val += "°";
    std::string tol;
    if (o->IsDimWithPlusMinusTolerance()) {
      const double up = o->GetUpperTolValue(), lo = o->GetLowerTolValue();
      if (up > 0 && std::fabs(std::fabs(up) - std::fabs(lo)) < 1e-12) tol = " ±" + num(up);
      else tol = std::string(" ") + (up >= 0 ? "+" : "") + num(up) + " / " + (lo > 0 ? "+" : lo == 0 ? "" : "") + num(lo);
    }
    const std::string fit = fitClass(o);
    p.title = prefix + val + (fit.empty() ? "" : " " + fit) + tol;
    const std::string pn = hstr(o->GetPresentationName());
    p.detail = std::string(tn) + (pn.empty() || pn == p.title ? "" : " · " + pn);
    p.pres = o->GetPresentation();
    p.hasPt = o->HasTextPoint();
    if (p.hasPt) p.pt = o->GetPointTextAttach();
    refsOf(st, dims.Value(i), p.refs);
    m.pmi.push_back(p);
  }

  TDF_LabelSequence tols;
  dt->GetGeomToleranceLabels(tols);
  for (int i = 1; i <= tols.Length(); i++) {
    Handle(XCAFDoc_GeomTolerance) at;
    if (!tols.Value(i).FindAttribute(XCAFDoc_GeomTolerance::GetID(), at)) continue;
    Handle(XCAFDimTolObjects_GeomToleranceObject) o = at->GetObject();
    if (o.IsNull()) continue;
    Pmi p;
    p.kind = 1;
    std::string datums;
    TDF_LabelSequence ds;
    if (XCAFDoc_DimTolTool::GetDatumOfTolerLabels(tols.Value(i), ds))
      for (int k = 1; k <= ds.Length(); k++) {
        Handle(XCAFDoc_Datum) da;
        if (ds.Value(k).FindAttribute(XCAFDoc_Datum::GetID(), da) && !da->GetObject().IsNull()) datums += " | " + hstr(da->GetObject()->GetName());
      }
    const bool dia = o->GetTypeOfValue() == XCAFDimTolObjects_GeomToleranceTypeValue_Diameter;
    p.title = std::string(tolTypeName(o->GetType())) + "  " + (dia ? "Ø" : "") + num(o->GetValue()) + datums;
    p.detail = "Geometric tolerance";
    p.pres = o->GetPresentation();
    p.hasPt = o->HasPointText();
    if (p.hasPt) p.pt = o->GetPointTextAttach();
    refsOf(st, tols.Value(i), p.refs);
    m.pmi.push_back(p);
  }
}

