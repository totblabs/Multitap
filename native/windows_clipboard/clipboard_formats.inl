// Included by clipboard.cpp inside namespace multitap::clipboard: HTML / RTF validation, DIB <-> PNG
// conversion (WIC) and publishing of rich text and images.
namespace {
using Microsoft::WRL::ComPtr;

// COM on the calling thread for the duration of a WIC operation. Threads that already initialised COM
// (the client's clipboard worker, the Host worker) keep their apartment.
struct ComScope {
  bool uninitialize = false;
  ComScope() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    uninitialize = SUCCEEDED(result);
    require(SUCCEEDED(result) || result == RPC_E_CHANGED_MODE, "clipboard_image_invalid");
  }
  ~ComScope() { if (uninitialize) CoUninitialize(); }
  ComScope(const ComScope&) = delete;
};
ComPtr<IWICImagingFactory> imagingFactory() {
  ComPtr<IWICImagingFactory> factory;
  require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "clipboard_image_invalid");
  return factory;
}

bool parseDecimal(const std::string& text, long long& value) {
  if (text.empty() || text.size() > 11) return false;
  size_t at = 0; bool negative = false;
  if (text[0] == '-') { negative = true; at = 1; if (text.size() == 1) return false; }
  long long result = 0;
  for (; at < text.size(); ++at) { if (text[at] < '0' || text[at] > '9') return false; result = result * 10 + (text[at] - '0'); }
  value = negative ? -result : result; return true;
}
}

namespace {
struct HtmlOffsets { long long startHtml = 0, endHtml = 0, startFragment = 0, endFragment = 0; };
HtmlOffsets parseHtmlHeader(const std::vector<uint8_t>& payload) {
  require(!payload.empty() && payload.size() <= maxRichBytes, "clipboard_html_invalid");
  require(std::find(payload.begin(), payload.end(), uint8_t(0)) == payload.end(), "clipboard_html_invalid");
  require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(payload.data()), int(payload.size()), nullptr, 0) > 0, "clipboard_html_invalid");
  // Header: "Key:Value" lines up to the first line that does not look like one (at most 16 lines).
  std::map<std::string, long long> numbers; bool versionSeen = false; size_t at = 0, headerEnd = 0;
  for (int line = 0; line < 16 && at < payload.size(); ++line) {
    size_t end = at; while (end < payload.size() && payload[end] != '\n' && payload[end] != '\r') ++end;
    const std::string text(reinterpret_cast<const char*>(payload.data()) + at, end - at);
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 20) { headerEnd = at; break; }
    const std::string key = text.substr(0, colon), value = text.substr(colon + 1);
    if (key == "Version") { require(!value.empty() && value.size() <= 8 && value.find_first_not_of("0123456789.") == std::string::npos, "clipboard_html_invalid"); versionSeen = true; }
    else if (key == "StartHTML" || key == "EndHTML" || key == "StartFragment" || key == "EndFragment" || key == "StartSelection" || key == "EndSelection") {
      long long number = 0; require(parseDecimal(value, number), "clipboard_html_invalid"); require(!numbers.count(key), "clipboard_html_invalid"); numbers[key] = number;
    }
    // SourceURL and unknown keys are ignored.
    if (end < payload.size() && payload[end] == '\r') ++end;
    if (end < payload.size() && payload[end] == '\n') ++end;
    at = end; headerEnd = at;
  }
  require(versionSeen && numbers.count("StartHTML") && numbers.count("EndHTML") && numbers.count("StartFragment") && numbers.count("EndFragment"), "clipboard_html_invalid");
  const long long size = static_cast<long long>(payload.size()), startHtml = numbers["StartHTML"], endHtml = numbers["EndHTML"],
      startFragment = numbers["StartFragment"], endFragment = numbers["EndFragment"];
  require(startHtml >= static_cast<long long>(headerEnd) && startHtml <= endHtml && endHtml <= size, "clipboard_html_invalid");
  require(startFragment >= startHtml && startFragment <= endFragment && endFragment <= endHtml, "clipboard_html_invalid");
  if (numbers.count("StartSelection") != numbers.count("EndSelection")) throw std::runtime_error("clipboard_html_invalid");
  if (numbers.count("StartSelection")) {
    const long long a = numbers["StartSelection"], b = numbers["EndSelection"];
    if (!(a == -1 && b == -1)) require(a >= startHtml && a <= b && b <= endHtml, "clipboard_html_invalid");
  }
  return HtmlOffsets{startHtml, endHtml, startFragment, endFragment};
}
}
void validateHtmlFormat(const std::vector<uint8_t>& payload) { parseHtmlHeader(payload); }
#include "html_sanitize.inl"

std::vector<uint8_t> sanitizeHtmlFormat(const std::vector<uint8_t>& payload) {
  const auto offsets = parseHtmlHeader(payload);
  // Only the fragment is kept (the peer's prologue, epilogue, SourceURL and selection markers are not): it is re-serialised
  // by the tokenizer and wrapped in a header this function writes itself, so the offsets are right by construction.
  const char* begin = reinterpret_cast<const char*>(payload.data());
  const std::string fragment(begin + offsets.startFragment, begin + offsets.endFragment);
  if (!fragment.empty()) require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fragment.data(), int(fragment.size()), nullptr, 0) > 0, "clipboard_html_invalid");
  const std::string clean = sanitizeHtmlFragment(fragment);
  static constexpr char prefix[] = "<html><body><!--StartFragment-->", suffix[] = "<!--EndFragment--></body></html>";
  const auto header = [](size_t startHtml, size_t endHtml, size_t startFragment, size_t endFragment) {
    char buffer[160]; snprintf(buffer, sizeof(buffer), "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n", startHtml, endHtml, startFragment, endFragment);
    return std::string(buffer);
  };
  const size_t headerSize = header(0, 0, 0, 0).size(), startFragment = headerSize + sizeof(prefix) - 1, endFragment = startFragment + clean.size();
  const std::string whole = header(headerSize, endFragment + sizeof(suffix) - 1, startFragment, endFragment) + prefix + clean + suffix;
  require(whole.size() <= maxRichBytes, "clipboard_html_invalid");
  return std::vector<uint8_t>(whole.begin(), whole.end());
}

void validateRtf(const std::vector<uint8_t>& payload) {
  static constexpr char prefix[] = "{\\rtf";
  require(payload.size() >= 6 && payload.size() <= maxRichBytes && std::equal(prefix, prefix + 5, payload.begin()) &&
          std::find(payload.begin(), payload.end(), uint8_t(0)) == payload.end(), "clipboard_rtf_invalid");
}

bool readPngHeader(const std::vector<uint8_t>& png, uint32_t& width, uint32_t& height) {
  static constexpr uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (png.size() < 33 || !std::equal(signature, signature + 8, png.begin())) return false;
  const auto big = [&](size_t at) { return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3]; };
  if (big(8) != 13 || std::memcmp(&png[12], "IHDR", 4) != 0) return false;
  width = big(16); height = big(20);
  if (!width || !height || width > 65535 || height > 65535) return false;
  require(uint64_t(width) * height <= maxImagePixels && png.size() <= maxImageBytes, "clipboard_image_too_large");
  return true;
}
Image describePng(std::vector<uint8_t> png) {
  Image image; require(readPngHeader(png, image.width, image.height), "clipboard_image_invalid");
  image.png = std::move(png); return image;
}

namespace {
// Pixel layout of a DIB: how to turn one stored pixel into B, G, R, A.
struct Channel { uint32_t mask = 0; int shift = 0; uint32_t maximum = 0; };
Channel channelOf(uint32_t mask) {
  Channel channel; channel.mask = mask; if (!mask) return channel;
  while (!((mask >> channel.shift) & 1)) ++channel.shift;
  channel.maximum = mask >> channel.shift; return channel;
}
uint8_t scaled(const Channel& channel, uint32_t pixel) {
  if (!channel.mask) return 255;
  const uint32_t value = (pixel & channel.mask) >> channel.shift;
  return uint8_t((uint64_t(value) * 255 + channel.maximum / 2) / channel.maximum);
}
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}

Image encodePngFromDib(const uint8_t* dib, size_t size) {
  require(dib && size >= 40, "clipboard_image_invalid");
  const uint32_t headerSize = le32(dib);
  require((headerSize == 40 || headerSize == 108 || headerSize == 124) && size >= headerSize, "clipboard_image_invalid");
  const int32_t widthValue = int32_t(le32(dib + 4)), heightValue = int32_t(le32(dib + 8));
  const uint16_t planes = uint16_t(dib[12] | (dib[13] << 8)), bits = uint16_t(dib[14] | (dib[15] << 8));
  const uint32_t compression = le32(dib + 16), colorsUsed = le32(dib + 32);
  require(planes == 1 && widthValue > 0 && heightValue != 0 && heightValue != INT32_MIN, "clipboard_image_invalid");
  const uint32_t width = uint32_t(widthValue), height = uint32_t(heightValue < 0 ? -int64_t(heightValue) : heightValue);
  const bool topDown = heightValue < 0;
  require(width <= 65535 && height <= 65535, "clipboard_image_too_large");
  require(uint64_t(width) * height <= maxImagePixels, "clipboard_image_too_large");
  require(bits == 1 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32, "clipboard_image_invalid");
  require(compression == BI_RGB || compression == BI_BITFIELDS || compression == 6 /* BI_ALPHABITFIELDS */, "clipboard_image_invalid");
  require(compression == BI_RGB || bits == 16 || bits == 32, "clipboard_image_invalid");
  size_t offset = headerSize; Channel red, green, blue, alpha;
  if (compression != BI_RGB) {
    if (headerSize == 40) {
      const size_t masks = compression == 6 ? 16 : 12; require(size >= offset + masks, "clipboard_image_invalid");
      red = channelOf(le32(dib + offset)); green = channelOf(le32(dib + offset + 4)); blue = channelOf(le32(dib + offset + 8));
      if (compression == 6) alpha = channelOf(le32(dib + offset + 12));
      offset += masks;
    } else {
      red = channelOf(le32(dib + 40)); green = channelOf(le32(dib + 44)); blue = channelOf(le32(dib + 48)); alpha = channelOf(le32(dib + 52));
    }
    require(red.mask && green.mask && blue.mask, "clipboard_image_invalid");
  }
  // Palette (indexed formats) follows the header and masks.
  std::vector<uint32_t> palette;
  if (bits <= 8) {
    const uint32_t entries = colorsUsed ? colorsUsed : (1u << bits);
    require(entries <= (1u << bits) && size >= offset + size_t(entries) * 4, "clipboard_image_invalid");
    for (uint32_t i = 0; i < entries; ++i) palette.push_back(le32(dib + offset + size_t(i) * 4));
    offset += size_t(entries) * 4;
  } else if (colorsUsed && compression == BI_RGB) {
    require(size >= offset + size_t(colorsUsed) * 4, "clipboard_image_invalid"); offset += size_t(colorsUsed) * 4;
  }
  const uint64_t stride = ((uint64_t(width) * bits + 31) / 32) * 4;
  require(stride * height <= size - offset, "clipboard_image_invalid");
  const uint8_t* pixels = dib + offset;
  // The picture is converted and handed to the encoder in strips: an 8 MB 1-bit DIB describes up to 64 Mpixels, which
  // as one BGRA buffer would be 32 times its size. Only the strip (about 4 MiB) is ever held in expanded form.
  const auto convertRow = [&](uint32_t y, uint8_t* out) {
    const uint8_t* row = pixels + stride * (topDown ? y : height - 1 - y);
    for (uint32_t x = 0; x < width; ++x, out += 4) {
      uint8_t b = 0, g = 0, r = 0, a = 255;
      switch (bits) {
        case 1: case 4: case 8: {
          const uint32_t bitIndex = x * bits, value = (row[bitIndex / 8] >> (8 - bits - (bitIndex % 8))) & ((1u << bits) - 1);
          const uint32_t color = value < palette.size() ? palette[value] : 0; b = uint8_t(color); g = uint8_t(color >> 8); r = uint8_t(color >> 16); break; }
        case 16: {
          const uint32_t pixel = row[x * 2] | (row[x * 2 + 1] << 8);
          if (compression == BI_RGB) { b = scaled(channelOf(0x1f), pixel); g = scaled(channelOf(0x3e0), pixel); r = scaled(channelOf(0x7c00), pixel); }
          else { b = scaled(blue, pixel); g = scaled(green, pixel); r = scaled(red, pixel); a = scaled(alpha, pixel); }
          break; }
        case 24: b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; break;
        default: {
          const uint32_t pixel = le32(row + size_t(x) * 4);
          if (compression == BI_RGB) { b = uint8_t(pixel); g = uint8_t(pixel >> 8); r = uint8_t(pixel >> 16); }
          else { b = scaled(blue, pixel); g = scaled(green, pixel); r = scaled(red, pixel); a = scaled(alpha, pixel); }
          break; }
      }
      out[0] = b; out[1] = g; out[2] = r; out[3] = a;
    }
  };
  // Only a bit-field DIB with an alpha mask can be transparent; one pass finds out before the pixel format is chosen.
  bool opaque = true;
  if ((bits == 16 || bits == 32) && compression != BI_RGB && alpha.mask) {
    std::vector<uint8_t> scratch(size_t(width) * 4);
    for (uint32_t y = 0; y < height && opaque; ++y) {
      convertRow(y, scratch.data());
      for (uint32_t x = 0; x < width; ++x) if (scratch[size_t(x) * 4 + 3] != 255) { opaque = false; break; }
    }
  }
  // A fully opaque picture is stored as 24-bit (a third smaller), otherwise with its alpha channel.
  const size_t pixelSize = opaque ? 3 : 4; const UINT rowBytes = UINT(width * pixelSize); const GUID format = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
  ComScope com; const auto factory = imagingFactory();
  ComPtr<IStream> stream; require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "clipboard_image_invalid");
  ComPtr<IWICBitmapEncoder> encoder; require(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)), "clipboard_image_invalid");
  require(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)), "clipboard_image_invalid");
  ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options;
  require(SUCCEEDED(encoder->CreateNewFrame(&frame, &options)) && SUCCEEDED(frame->Initialize(options.Get())) && SUCCEEDED(frame->SetSize(width, height)), "clipboard_image_invalid");
  WICPixelFormatGUID wanted = format;
  require(SUCCEEDED(frame->SetPixelFormat(&wanted)) && wanted == format, "clipboard_image_invalid");
  const uint32_t stripRows = uint32_t(std::max<size_t>(1, std::min<size_t>(height, (size_t(4) << 20) / (size_t(width) * 4))));
  std::vector<uint8_t> strip(size_t(stripRows) * width * 4);
  for (uint32_t y = 0; y < height; y += stripRows) {
    const uint32_t rows = std::min(stripRows, height - y);
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t* wide = strip.data() + size_t(r) * width * 4;
      convertRow(y + r, wide);
      if (opaque) { uint8_t* narrow = strip.data() + size_t(r) * rowBytes; for (uint32_t x = 0; x < width; ++x) { narrow[x * 3] = wide[x * 4]; narrow[x * 3 + 1] = wide[x * 4 + 1]; narrow[x * 3 + 2] = wide[x * 4 + 2]; } }
    }
    // For an opaque picture the 24-bit rows are packed from the front of the strip; row r reads row r's 32-bit pixels
    // only after rows before it were packed, which never overlaps (packed offset r*3w <= wide offset r*4w).
    require(SUCCEEDED(frame->WritePixels(rows, rowBytes, UINT(size_t(rows) * rowBytes), strip.data())), "clipboard_image_invalid");
  }
  require(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "clipboard_image_invalid");
  HGLOBAL memory = nullptr; require(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &memory)) && memory, "clipboard_image_invalid");
  const SIZE_T encoded = GlobalSize(memory);
  // The HGLOBAL can be larger than the written stream: use the stream's own position.
  STATSTG stat{}; require(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)), "clipboard_image_invalid");
  const uint64_t length = stat.cbSize.QuadPart; require(length > 0 && length <= encoded, "clipboard_image_invalid");
  require(length <= maxImageBytes, "clipboard_image_too_large");
  const auto* bytes = static_cast<const uint8_t*>(GlobalLock(memory)); require(bytes != nullptr, "clipboard_lock_failed");
  Image image; image.png.assign(bytes, bytes + length); GlobalUnlock(memory);
  image.width = width; image.height = height; return image;
}

std::vector<uint8_t> decodePngToBgra(const std::vector<uint8_t>& png, uint32_t expectedWidth, uint32_t expectedHeight) {
  uint32_t width = 0, height = 0;
  require(readPngHeader(png, width, height) && width == expectedWidth && height == expectedHeight, "clipboard_image_invalid");
  ComScope com; const auto factory = imagingFactory();
  ComPtr<IWICStream> stream; require(SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()))), "clipboard_image_invalid");
  ComPtr<IWICBitmapDecoder> decoder; require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)), "clipboard_image_invalid");
  ComPtr<IWICBitmapFrameDecode> frame; require(SUCCEEDED(decoder->GetFrame(0, &frame)), "clipboard_image_invalid");
  UINT w = 0, h = 0; require(SUCCEEDED(frame->GetSize(&w, &h)) && w == width && h == height, "clipboard_image_invalid");
  ComPtr<IWICFormatConverter> converter; require(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "clipboard_image_invalid");
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  require(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, UINT(bgra.size()), bgra.data())), "clipboard_image_invalid");
  return bgra;
}

namespace {
void appendNul(std::vector<uint8_t>& bytes) { bytes.push_back(0); }
}

DWORD putRich(HWND owner, const std::wstring& text, const std::vector<uint8_t>* html, const std::vector<uint8_t>* rtf, std::optional<DWORD> expectedSequence) {
  validText(text);
  // Rich Text Format is never published: RTF can embed OLE objects (\\object, \\objdata) and fetch external content
  // (\\field INCLUDEPICTURE / LINK, \\*\\template), and nothing in it can be made safe by checking. HTML is re-serialised by
  // the sanitiser (clipboard_html_invalid when it does not validate); the plain text is always delivered.
  (void)rtf;
  std::optional<std::vector<uint8_t>> cleanHtml;
  if (html) cleanHtml = sanitizeHtmlFormat(*html);
  html = cleanHtml ? &*cleanHtml : nullptr; rtf = nullptr;
  const UINT htmlFormat = html ? RegisterClipboardFormatW(L"HTML Format") : 0, rtfFormat = rtf ? RegisterClipboardFormatW(L"Rich Text Format") : 0;
  if ((html && !htmlFormat) || (rtf && !rtfFormat)) { noteWin32(GetLastError()); throw std::runtime_error("clipboard_write_failed"); }
  return publishWithHeal(expectedSequence, [&](std::optional<DWORD>& expected) {
    Memory unicode((text.size() + 1) * sizeof(wchar_t));
    unicode.put(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    std::optional<Memory> htmlMemory, rtfMemory;
    if (html) { auto bytes = *html; appendNul(bytes); htmlMemory.emplace(bytes.size()); htmlMemory->put(bytes.data(), bytes.size()); }
    if (rtf) { auto bytes = *rtf; appendNul(bytes); rtfMemory.emplace(bytes.size()); rtfMemory->put(bytes.data(), bytes.size()); }
    Clipboard clipboard(owner, writeBudgetMs); requireUnchangedSequence(expected, GetClipboardSequenceNumber());
    unicode.transfer(CF_UNICODETEXT);
    if (htmlMemory) htmlMemory->give(htmlFormat);
    if (rtfMemory) rtfMemory->give(rtfFormat);
    return GetClipboardSequenceNumber();
  });
}

namespace {
// The clipboard bitmap formats of a decoded picture (32-bit BGRA, top-down, straight alpha), shared by the direct publication
// (putImage) and the delayed rendering of an offered image.
HGLOBAL makeDibV5(const std::vector<uint8_t>& bgra, size_t width, size_t height) {
  // CF_DIBV5: 32-bit with straight alpha, bottom-up.
  Memory v5(sizeof(BITMAPV5HEADER) + width * height * 4);
  auto* base = v5.lock(); auto* header = reinterpret_cast<BITMAPV5HEADER*>(base);
  header->bV5Size = sizeof(BITMAPV5HEADER); header->bV5Width = LONG(width); header->bV5Height = LONG(height); header->bV5Planes = 1; header->bV5BitCount = 32;
  header->bV5Compression = BI_BITFIELDS; header->bV5SizeImage = DWORD(width * height * 4);
  header->bV5RedMask = 0x00ff0000; header->bV5GreenMask = 0x0000ff00; header->bV5BlueMask = 0x000000ff; header->bV5AlphaMask = 0xff000000;
  header->bV5CSType = LCS_sRGB; header->bV5Intent = LCS_GM_IMAGES;
  uint8_t* out = base + sizeof(BITMAPV5HEADER);
  for (size_t y = 0; y < height; ++y) std::memcpy(out + (height - 1 - y) * width * 4, bgra.data() + y * width * 4, width * 4);
  v5.unlock();
  return v5.release();
}
HGLOBAL makeDib24(const std::vector<uint8_t>& bgra, size_t width, size_t height) {
  // CF_DIB: 24-bit, transparency composited over white, for readers that ignore alpha.
  const size_t stride24 = ((width * 3 + 3) / 4) * 4;
  Memory plain(sizeof(BITMAPINFOHEADER) + stride24 * height);
  auto* base = plain.lock(); auto* header = reinterpret_cast<BITMAPINFOHEADER*>(base);
  header->biSize = sizeof(BITMAPINFOHEADER); header->biWidth = LONG(width); header->biHeight = LONG(height); header->biPlanes = 1; header->biBitCount = 24;
  header->biCompression = BI_RGB; header->biSizeImage = DWORD(stride24 * height);
  uint8_t* out = base + sizeof(BITMAPINFOHEADER);
  for (size_t y = 0; y < height; ++y) {
    uint8_t* row = out + (height - 1 - y) * stride24; const uint8_t* source = bgra.data() + y * width * 4;
    for (size_t x = 0; x < width; ++x, source += 4, row += 3) {
      const unsigned a = source[3];
      for (int c = 0; c < 3; ++c) row[c] = uint8_t((source[c] * a + 255 * (255 - a) + 127) / 255);
    }
  }
  plain.unlock();
  return plain.release();
}
}

DWORD putImage(HWND owner, const Image& image, std::optional<DWORD> expectedSequence) {
  require(image.width && image.height && uint64_t(image.width) * image.height <= maxImagePixels && !image.png.empty() && image.png.size() <= maxImageBytes, "clipboard_image_too_large");
  const auto bgra = decodePngToBgra(image.png, image.width, image.height);
  const size_t width = image.width, height = image.height;
  const UINT pngFormat = RegisterClipboardFormatW(L"PNG"); if (!pngFormat) { noteWin32(GetLastError()); throw std::runtime_error("clipboard_write_failed"); }
  return publishWithHeal(expectedSequence, [&](std::optional<DWORD>& expected) {
  Memory v5(makeDibV5(bgra, width, height), Memory::Adopt{}), plain(makeDib24(bgra, width, height), Memory::Adopt{});
  Memory png(image.png.size()); png.put(image.png.data(), image.png.size());
  Clipboard clipboard(owner, writeBudgetMs); requireUnchangedSequence(expected, GetClipboardSequenceNumber());
  png.transfer(pngFormat); v5.give(CF_DIBV5); plain.give(CF_DIB);
  return GetClipboardSequenceNumber();
  });
}
