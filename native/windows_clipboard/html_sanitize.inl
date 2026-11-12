// Included by clipboard.cpp inside namespace multitap::clipboard: the HTML sanitiser for CF_HTML fragments.
//
// The peer is not trusted. A fragment that reaches the real clipboard is pasted into Word / Outlook / browsers,
// which fetch images and linked resources (a UNC or file: URL leaks the user's NTLM hash) and which act on
// scripts, OLE objects and Office conditional comments. The sanitiser is therefore a RE-SERIALISER, not a filter:
// it tokenises the input conservatively (the HTML tokenizer's own quoting and comment rules) and writes out only
// what it built itself - allow-listed element names with allow-listed attributes whose values were entity-decoded
// to a fixed point, judged, and re-escaped. Comments, doctypes, CDATA, processing instructions, unknown
// elements and every attribute not on the list are not copied. Anything it cannot parse is dropped, never kept.
namespace {
namespace htmlsan {
inline bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline std::string lowered(std::string value) { for (auto& c : value) c = lowerChar(c); return value; }

// Elements that are written out (void ones without an end tag). Everything else (link, base, meta, form, import, input,
// unknown and Office-namespace elements ...) is dropped as a tag; its text content stays unless the element is in
// dropContent().
const std::set<std::string>& keepTags() {
  static const std::set<std::string> tags = {"a", "abbr", "b", "big", "blockquote", "br", "caption", "center", "cite", "code", "col", "colgroup",
      "dd", "del", "dfn", "div", "dl", "dt", "em", "font", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "ins", "kbd", "li", "mark", "ol",
      "p", "pre", "q", "s", "samp", "small", "span", "strike", "strong", "sub", "sup", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "tt", "u", "ul", "var"};
  return tags;
}
// Elements whose whole content goes with the tag (code, plugins, frames, foreign content, Office XML islands).
const std::set<std::string>& dropContent() {
  static const std::set<std::string> tags = {"script", "style", "object", "embed", "iframe", "frame", "frameset", "noframes", "noembed", "noscript",
      "applet", "svg", "math", "template", "xml", "head", "title", "textarea", "xmp", "audio", "video", "canvas", "portal", "plaintext"};
  return tags;
}
bool voidTag(const std::string& name) { return name == "br" || name == "hr" || name == "img" || name == "col"; }
const std::set<std::string>& plainAttributes() {
  static const std::set<std::string> names = {"abbr", "align", "alt", "bgcolor", "border", "cellpadding", "cellspacing", "class", "color", "colspan",
      "dir", "face", "headers", "height", "hspace", "id", "lang", "name", "nowrap", "rowspan", "scope", "size", "span", "start", "title", "type",
      "valign", "value", "vspace", "width"};
  return names;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}
// One pass of character-reference decoding for an attribute value, deliberately lenient (a name from the table
// decodes even without its semicolon, so a consumer that is lenient cannot see more than the judge did).
// False when a reference cannot be resolved with certainty: such a value is dropped.
bool decodeOnce(const std::string& in, std::string& out) {
  static const std::map<std::string, uint32_t> named = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
      {"colon", ':'}, {"sol", '/'}, {"bsol", '\\'}, {"Tab", '\t'}, {"NewLine", '\n'}, {"lpar", '('}, {"rpar", ')'}, {"period", '.'}, {"comma", ','},
      {"semi", ';'}, {"equals", '='}, {"quest", '?'}, {"num", '#'}, {"percnt", '%'}, {"commat", '@'}, {"excl", '!'}, {"plus", '+'}, {"lowbar", '_'},
      {"hyphen", '-'}, {"dash", '-'}, {"lsqb", '['}, {"rsqb", ']'}, {"lbrack", '['}, {"rbrack", ']'}, {"lcub", '{'}, {"rcub", '}'}, {"lbrace", '{'}, {"rbrace", '}'},
      {"vert", '|'}, {"verbar", '|'}, {"grave", '`'}, {"dollar", '$'}, {"ast", '*'}, {"midast", '*'}, {"hat", '^'}, {"tilde", '~'}, {"NonBreakingSpace", 0xA0}};
  out.clear();
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '&') { out += in[i++]; continue; }
    if (i + 1 < in.size() && in[i + 1] == '#') {
      size_t j = i + 2; const bool hex = j < in.size() && (in[j] == 'x' || in[j] == 'X'); if (hex) ++j;
      uint64_t value = 0; size_t digits = 0;
      for (; j < in.size(); ++j, ++digits) {
        const char c = in[j]; int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        value = value * (hex ? 16 : 10) + uint64_t(digit); if (value > 0x10FFFF) return false;
      }
      if (digits == 0) { out += in[i++]; continue; }  // "&#" followed by nothing numeric stays literal text
      if (j < in.size() && in[j] == ';') ++j;
      if (value == 0 || (value >= 0xD800 && value <= 0xDFFF)) return false;
      appendUtf8(out, uint32_t(value)); i = j; continue;
    }
    size_t j = i + 1; while (j < in.size() && ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') || (in[j] >= '0' && in[j] <= '9'))) ++j;
    if (j == i + 1) { out += in[i++]; continue; }
    const std::string name = in.substr(i + 1, j - i - 1); const bool semicolon = j < in.size() && in[j] == ';';
    const auto found = named.find(name);
    if (found != named.end()) { appendUtf8(out, found->second); i = semicolon ? j + 1 : j; continue; }
    if (semicolon) return false;  // an entity this tool does not know: it could be anything
    out.append(in, i, j - i); i = j;
  }
  return true;
}
// Decodes to a fixed point (a value that still changes after three further passes is refused).
bool decodeFully(std::string& value) {
  for (int pass = 0; pass < 4; ++pass) {
    std::string next; if (!decodeOnce(value, next)) return false;
    if (next == value) return true;
    value = std::move(next);
  }
  return false;
}
std::string stripped(const std::string& value) {  // ASCII controls and spaces anywhere, as URL parsers ignore them
  std::string out; for (unsigned char c : value) if (c > 0x20 && c != 0x7F) out += char(c);
  return out;
}
// A URL is acceptable only with an explicit, plain scheme: http / https (and mailto for a link, data:image/... for an
// image). UNC, protocol-relative, file:, javascript:, ms-*: and every other relative form are refused.
bool urlAllowed(const std::string& decodedOnce, bool image) {
  std::string value = decodedOnce; if (!decodeFully(value)) return false;
  const std::string s = stripped(value);
  if (s.empty() || s.find('\\') != std::string::npos) return false;
  if (s[0] == '#') return !image;
  const size_t colon = s.find_first_of(":/?#");
  if (colon == std::string::npos || s[colon] != ':' || colon == 0 || !alpha(s[0])) return false;
  const std::string scheme = lowered(s.substr(0, colon));
  for (unsigned char c : scheme) if (!(alpha(char(c)) || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')) return false;
  const std::string rest = s.substr(colon + 1);
  if (scheme == "http" || scheme == "https") return rest.size() > 2 && rest[0] == '/' && rest[1] == '/' && rest[2] != '/' && rest[2] != '?' && rest[2] != '#';
  if (scheme == "mailto") return !image && !rest.empty();
  if (scheme == "data" && image) {
    const std::string lowerRest = lowered(rest);
    for (const char* type : {"image/png", "image/jpeg", "image/gif", "image/webp"}) {
      const size_t length = std::strlen(type);
      if (lowerRest.size() > length && lowerRest.compare(0, length, type) == 0 && (lowerRest[length] == ';' || lowerRest[length] == ',')) return true;
    }
  }
  return false;
}
// Inline style: anything that can load a resource, run code or hide such a thing is refused wholesale.
bool styleAllowed(const std::string& decodedOnce) {
  std::string value = decodedOnce; if (!decodeFully(value)) return false;
  const std::string s = lowered(stripped(value));
  for (const char* bad : {"url", "image", "expression", "behavior", "binding", "import", "script", "src(", "@", "\\", "/*", "<", ">", "&", "mso-"})
    if (s.find(bad) != std::string::npos) return false;
  return true;
}
void escapeAttribute(std::string& out, const std::string& value) {
  for (unsigned char c : value) {
    if (c == '&') out += "&amp;"; else if (c == '"') out += "&quot;"; else if (c == '<') out += "&lt;"; else if (c == '>') out += "&gt;";
    else if (c < 0x20 || c == 0x7F) { if (c == '\t' || c == '\n' || c == '\r') out += ' '; }
    else out += char(c);
  }
}
// Index just after the end of the element `name` that starts at/after `from` (its end tag), or size() when it never ends.
size_t afterEndTag(const std::string& in, size_t from, const std::string& name) {
  for (size_t at = in.find("</", from); at != std::string::npos; at = in.find("</", at + 2)) {
    if (at + 2 + name.size() > in.size()) break;
    if (lowered(in.substr(at + 2, name.size())) != name) continue;
    const size_t after = at + 2 + name.size();
    if (after < in.size() && !space(static_cast<unsigned char>(in[after])) && in[after] != '/' && in[after] != '>') continue;
    const size_t close = in.find('>', after); return close == std::string::npos ? in.size() : close + 1;
  }
  return in.size();
}
}  // namespace htmlsan
}  // namespace

std::string sanitizeHtmlFragment(const std::string& in) {
  using namespace htmlsan;
  std::string out; out.reserve(in.size());
  const size_t n = in.size(); size_t i = 0;
  while (i < n) {
    const char c = in[i];
    if (c != '<') {
      if (c == '>') out += "&gt;";
      else if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') { if (static_cast<unsigned char>(c) != 0x7F) out += c; }
      ++i; continue;
    }
    if (i + 1 >= n) { out += "&lt;"; ++i; continue; }
    const char d = in[i + 1];
    if (d == '!' || d == '?') {  // comment / CDATA / doctype / processing instruction: never copied
      if (in.compare(i, 4, "<!--") == 0) {
        size_t end;
        if (in.compare(i + 4, 1, ">") == 0) end = i + 5;
        else if (in.compare(i + 4, 2, "->") == 0) end = i + 6;
        else {
          const size_t a = in.find("-->", i + 4), b = in.find("--!>", i + 4);
          end = (a == std::string::npos && b == std::string::npos) ? n : (b == std::string::npos || (a != std::string::npos && a < b)) ? a + 3 : b + 4;
        }
        i = std::min(end, n); continue;
      }
      const size_t close = in.find('>', i + 2); i = close == std::string::npos ? n : close + 1; continue;
    }
    if (d == '/') {
      size_t j = i + 2; const size_t nameStart = j;
      while (j < n && !space(static_cast<unsigned char>(in[j])) && in[j] != '/' && in[j] != '>') ++j;
      const std::string name = lowered(in.substr(nameStart, j - nameStart));
      const size_t close = in.find('>', j); i = close == std::string::npos ? n : close + 1;
      if (!name.empty() && alpha(name[0]) && keepTags().count(name) && !voidTag(name)) out += "</" + name + ">";
      continue;
    }
    if (!alpha(d)) { out += "&lt;"; ++i; continue; }
    // Start tag, tokenised like a browser does: quoted values may contain '>', and an unterminated tag eats the rest.
    size_t j = i + 1; const size_t nameStart = j;
    while (j < n && !space(static_cast<unsigned char>(in[j])) && in[j] != '/' && in[j] != '>') ++j;
    const std::string name = lowered(in.substr(nameStart, j - nameStart));
    std::vector<std::pair<std::string, std::string>> attributes; bool terminated = false;
    while (j < n) {
      while (j < n && (space(static_cast<unsigned char>(in[j])) || in[j] == '/')) ++j;
      if (j >= n) break;
      if (in[j] == '>') { ++j; terminated = true; break; }
      const size_t nameAt = j; if (in[j] == '=') ++j;
      while (j < n && !space(static_cast<unsigned char>(in[j])) && in[j] != '/' && in[j] != '>' && in[j] != '=') ++j;
      std::string attributeName = lowered(in.substr(nameAt, j - nameAt)), value;
      size_t k = j; while (k < n && space(static_cast<unsigned char>(in[k]))) ++k;
      if (k < n && in[k] == '=') {
        ++k; while (k < n && space(static_cast<unsigned char>(in[k]))) ++k;
        if (k < n && (in[k] == '"' || in[k] == '\'')) {
          const size_t close = in.find(in[k], k + 1);
          if (close == std::string::npos) { j = n; break; }
          value = in.substr(k + 1, close - k - 1); j = close + 1;
        } else {
          const size_t valueAt = k; while (k < n && !space(static_cast<unsigned char>(in[k])) && in[k] != '>') ++k;
          value = in.substr(valueAt, k - valueAt); j = k;
        }
      }
      if (attributes.size() < 64) attributes.emplace_back(std::move(attributeName), std::move(value));
    }
    i = j;
    if (!terminated) { i = n; break; }
    if (dropContent().count(name)) {
      i = name == "plaintext" ? n : afterEndTag(in, i, name);
      continue;
    }
    if (!keepTags().count(name)) continue;
    std::string tag = "<" + name; std::set<std::string> seen;
    for (const auto& [attributeName, raw] : attributes) {
      if (attributeName.empty() || !seen.insert(attributeName).second) continue;
      std::string value; if (!decodeOnce(raw, value)) continue;
      bool keep = false;
      if (attributeName == "href") keep = name == "a" && urlAllowed(value, false);
      else if (attributeName == "src") keep = name == "img" && urlAllowed(value, true);
      else if (attributeName == "style") keep = styleAllowed(value);
      else keep = plainAttributes().count(attributeName) > 0;
      if (!keep) continue;
      tag += " " + attributeName + "=\""; escapeAttribute(tag, value); tag += "\"";
    }
    out += tag + ">";
  }
  return out;
}
