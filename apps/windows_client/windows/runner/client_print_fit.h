#pragma once
// Where a PDF page sits on the printer's paper (print-v1): scaled down to the
// printable area when it does not fit (never up), aspect kept, centred, and
// the paper turned to the page's own orientation. Units are Direct2D DIPs
// (1/96 inch). Pure arithmetic so the native smoke test can check it.
#include <algorithm>

namespace multitap {
struct PaperArea {
  float shortSide = 0;  // the paper's short edge
  float longSide = 0;   // the paper's long edge
  // The largest unprintable border on any side. One value for all four sides
  // keeps the result free of clipping whichever way the driver turns the
  // paper, and keeps the page centred on the paper.
  float margin = 0;
};
struct PageFit {
  float paperWidth = 0, paperHeight = 0;  // the paper in the page's orientation
  float scale = 1;                        // 0 < scale <= 1
  float offsetX = 0, offsetY = 0;         // where the scaled page starts
  bool landscape = false;
  bool fitted = false;  // false: no usable paper known; draw 1:1 as before
};

inline PageFit FitPage(float pageWidth, float pageHeight, const PaperArea& paper) {
  PageFit fit;
  fit.paperWidth = pageWidth;
  fit.paperHeight = pageHeight;
  if (!(pageWidth > 0 && pageHeight > 0)) return fit;
  fit.landscape = pageWidth > pageHeight;
  const float shortSide = std::min(paper.shortSide, paper.longSide);
  const float longSide = std::max(paper.shortSide, paper.longSide);
  if (!(shortSide > 0) || !(paper.margin >= 0) || shortSide - 2 * paper.margin < 1 || longSide - 2 * paper.margin < 1) return fit;
  fit.paperWidth = fit.landscape ? longSide : shortSide;
  fit.paperHeight = fit.landscape ? shortSide : longSide;
  const float areaWidth = fit.paperWidth - 2 * paper.margin, areaHeight = fit.paperHeight - 2 * paper.margin;
  fit.scale = std::min({1.0f, areaWidth / pageWidth, areaHeight / pageHeight});
  fit.offsetX = (fit.paperWidth - pageWidth * fit.scale) / 2;
  fit.offsetY = (fit.paperHeight - pageHeight * fit.scale) / 2;
  fit.fitted = true;
  return fit;
}
}  // namespace multitap
