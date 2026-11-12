// Per-chunk CPU cost of each step of a served and a received pull (README.md, "Throughput benchmark"), measured with the real
// functions on one 45 KiB chunk: reading the copied file, base64, building and parsing the JSON messages. The file is this run's own
// sparse file; no clipboard, network or service is touched.
//   stage_bench.exe <new folder named run-...>
#include "e2e_common.hpp"
#include "clipboard_worker.hpp"
#include <chrono>

using namespace host;
namespace {
template <class F> double perCall(size_t calls, F&& work) {
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < calls; ++i) work(i);
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / double(calls);
}
void row(const char* stage, double micros) {
  std::cout << "{\"stage\":\"" << stage << "\",\"usPerChunk\":" << micros << ",\"MBps\":" << double(clip::formatsChunkBytes) / micros << "}\n";
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2) throw std::runtime_error("pass a new folder named run-...");
    const auto root = std::filesystem::absolute(argv[1]);
    if (std::filesystem::exists(root) || root.filename().wstring().rfind(L"run-", 0) != 0) throw std::runtime_error("unsafe folder");
    const uint64_t size = 256ull * 1024 * 1024; const size_t chunk = clip::formatsChunkBytes, chunks = size_t(size / chunk);
    const auto file = root / L"sparse.bin"; e2e::makeSparse(file, size, "END");
    auto offer = clip::SourceOffer::capture("bench", {file}, clip::Bounds{});
    { auto reader = offer->open(0, 0); row("reader.next (open, read, SHA-256)", perCall(chunks, [&](size_t) { (void)reader->next(chunk); })); }
    std::vector<uint8_t> bytes(chunk); for (size_t i = 0; i < chunk; ++i) bytes[i] = uint8_t(i * 131);
    {  // The parts of reader.next: the open and identity check, a read from an open handle, the hash.
      const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      row("ReadFile on an open handle", perCall(chunks, [&](size_t) { DWORD got = 0; ReadFile(handle, bytes.data(), DWORD(chunk), &got, nullptr); }));
      CloseHandle(handle);
      BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0); BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
      row("SHA-256 (BCrypt)", perCall(4000, [&](size_t) { BCryptHashData(hash, bytes.data(), ULONG(chunk), 0); }));
      BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
      row("open + identity check + close", perCall(4000, [&](size_t) { auto r = offer->open(0, 0); }));
    }
    std::string text; row("base64 encode", perCall(4000, [&](size_t) { text.clear(); clipboardAppendBase64(text, bytes.data(), bytes.size()); }));
    const std::string message = "{\"type\":\"clipboard.chunk\",\"requestId\":\"pullc-1\",\"index\":0,\"offset\":0,\"data\":\"" + text + "\"}";
    std::string packet; row("worker packet wrap", perCall(4000, [&](size_t) { packet = "{\"type\":\"clip.packet\",\"sessionId\":\"s\",\"generation\":1,\"payload\":"; packet += message; packet += '}'; }));
    row("service parse (JsonParser, packet)", perCall(4000, [&](size_t) { (void)JsonParser::parse(packet); }));
    row("service re-quote (jsonQuote, data)", perCall(4000, [&](size_t) { (void)jsonQuote(text); }));
    row("worker parse (JsonParser, message)", perCall(4000, [&](size_t) { (void)JsonParser::parse(message); }));
    row("base64 decode (canonical)", perCall(4000, [&](size_t) { (void)clipboardChunk(text, chunk); }));
    offer.reset(); std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
