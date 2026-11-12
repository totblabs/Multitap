#pragma once
#include <string>

namespace multitap {
// A page-scoped delta: retain the selected printer's paper, colour and duplex
// defaults, but explicitly describe this PDF page's orientation. AddPage's
// size alone is ignored by some printer drivers (Microsoft's AddPage contract).
inline std::string PageOrientationTicket(bool landscape) {
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
      "<psf:PrintTicket version=\"1\" "
      "xmlns:psf=\"http://schemas.microsoft.com/windows/2003/08/printing/printschemaframework\" "
      "xmlns:psk=\"http://schemas.microsoft.com/windows/2003/08/printing/printschemakeywords\">"
      "<psf:Feature name=\"psk:PageOrientation\"><psf:Option name=\"psk:" +
      (landscape ? "Landscape" : "Portrait") +
      "\"/></psf:Feature></psf:PrintTicket>";
}
}  // namespace multitap
