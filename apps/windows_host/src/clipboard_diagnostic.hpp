#pragma once
// Host clipboard diagnostics (hostAction 253). The clipboard worker (an interactive-session process, which may not write the machine
// journal) sends `clip.diag` packets over its pipe; the service validates them and records one journal event each. Fixed numeric
// codes only: no names, paths, text, hashes or exception text ever appear. The admin console mirrors these tables
// (server/admin/js/labels.js; server/test/host-diagnostics.test.mjs compares them with this file).
#include "operational_log.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace host {
inline constexpr uint32_t clipboardDiagnosticAction = 253;
// stage: what happened. Receive = the peer's copy arrives on this VM, send = this VM's copy goes to the peer.
namespace ClipboardDiagnostic {
inline constexpr uint32_t receiveBegin = 1, sendBegin = 2, receiveEnd = 3, sendEnd = 4, receiveFailed = 5, sendFailed = 6,
                          peerFailed = 7, peerCancelled = 8, pullFailed = 9,
                          // Observation of this VM's own copies (no transfer): a change the Host saw but did not announce, one it announced,
                          // a read that found nothing it can send, a listener that could not be set up, an announcement dropped before it left.
                          // `error` is a ClipboardNote reason; `items` of changeAnnounced and readEmpty is the ClipboardFormats mask of what
                          // was on the clipboard (metadata only: IsClipboardFormatAvailable, no content is read for it).
                          changeSkipped = 10, changeAnnounced = 11, readEmpty = 12, watchFailed = 13, changeDropped = 14;
}
// error of the observation stages above (1-based index into hostClipboardNoteText in the admin console).
namespace ClipboardNote {
inline constexpr uint32_t appliedOwn = 1,    // the change is this Host's own publication of the peer's content
                          writerOwner = 2,   // the clipboard owner is this worker's own window
                          ownOffer = 3,      // the change is this Host's own paste-pull offer
                          watchDisabled = 4, // the client stopped watching (its 2 s grace after leaving the window had ended) before the announcement went out
                          notActive = 5,     // the listener was gone when the announcement was due
                          notAllowed = 6,    // policy or the interactive desktop no longer allowed it
                          listenerRejected = 7, timerFailed = 8, watchException = 9;
}
// items of changeAnnounced / readEmpty: which kinds of content were on the clipboard (native/windows_clipboard availableFormats()).
namespace ClipboardFormats {
inline constexpr uint32_t fileDrop = 1, shellIdList = 2, virtualFiles = 4, text = 8, image = 16, html = 32;
}
// format: 1 text, 2 files, 3 rich text, 4 image, 5 folder tree, 6 offered files/folders (pulled on paste).
inline uint32_t clipboardFormatCode(const std::string& format) {
  if (format == "text") return 1;
  if (format == "files") return 2;
  if (format == "richtext") return 3;
  if (format == "image") return 4;
  if (format == "tree") return 5;
  if (format == "offer") return 6;
  return 0;
}
// error: the fixed reason that went to (or came from) the peer; 0 none.
inline uint32_t clipboardReasonCode(const std::string& reason) {
  static const std::vector<std::string> reasons{"clipboard_empty", "clipboard_busy", "unsupported_clipboard_format", "clipboard_read_failed", "clipboard_write_failed",
    "clipboard_file_unavailable", "clipboard_transfer_failed", "no_interactive_session", "clipboard_cancelled", "clipboard_policy_denied", "clipboard_changed",
    "clipboard_limits_exceeded", "clipboard_disk_full", "clipboard_file_locked", "clipboard_selected_file_changed", "clipboard_offer_expired",
    // The scale revision names the specific limit (the relay downgrades them to clipboard_limits_exceeded for a peer without it).
    "clipboard_path_too_long", "clipboard_text_too_large", "clipboard_entry_count_limit", "clipboard_depth_limit", "clipboard_image_too_large"};
  if (reason.empty()) return 0;
  for (size_t i = 0; i < reasons.size(); ++i) if (reasons[i] == reason) return uint32_t(i + 1);
  return 99;
}
// cause: the internal condition behind a failure (the library's fixed error name or the worker's own check); 255 = not in the table.
// BEGIN-CAUSES
inline const std::vector<std::string>& clipboardCauses() {
  static const std::vector<std::string> causes{"Invalid clipboard transfer", "Invalid clipboard chunk size", "Noncanonical clipboard base64", "Unsupported clipboard packet",
    "Clipboard worker stopped", "Clipboard window class failed", "clipboard_allocation_failed", "clipboard_busy", "clipboard_cancelled", "clipboard_changed", "clipboard_chunk_limit",
    "clipboard_chunk_out_of_order_or_bounds", "clipboard_data_bounds", "clipboard_depth_limit", "clipboard_disk_full", "clipboard_duplicate_file_name", "clipboard_empty",
    "clipboard_empty_failed", "clipboard_entry_count_limit", "clipboard_expiry_busy", "clipboard_expiry_invalid", "clipboard_file_count_limit", "clipboard_file_list_invalid",
    "clipboard_file_locked", "clipboard_file_name_or_size_limit", "clipboard_file_not_complete", "clipboard_file_not_materialized", "clipboard_file_path_invalid",
    "clipboard_file_unavailable", "clipboard_format_not_allowed", "clipboard_hash_create_failed", "clipboard_hash_finish_failed", "clipboard_hash_mismatch",
    "clipboard_hash_property_failed", "clipboard_hash_provider_failed", "clipboard_hash_update_failed", "clipboard_html_invalid", "clipboard_image_invalid",
    "clipboard_image_too_large", "clipboard_invalid_unicode", "clipboard_lock_failed", "clipboard_offer_expired", "clipboard_owner_not_this_process",
    "clipboard_owner_unavailable", "clipboard_path_too_long", "clipboard_policy_denied", "clipboard_private_acl_failed", "clipboard_regular_files_only",
    "clipboard_rtf_invalid", "clipboard_selected_file_changed", "clipboard_selected_file_unavailable", "clipboard_text_not_terminated", "clipboard_text_too_large",
    "clipboard_text_too_large_or_invalid", "clipboard_timeout", "clipboard_total_size_limit", "clipboard_transfer_directory_failed", "clipboard_transfer_directory_lock_failed",
    "clipboard_transfer_directory_untrusted", "clipboard_transfer_failed", "clipboard_transfer_file_create_failed", "clipboard_transfer_flush_failed",
    "clipboard_transfer_hash_mismatch", "clipboard_transfer_id_failed", "clipboard_transfer_not_active", "clipboard_transfer_not_complete", "clipboard_transfer_parent_invalid",
    "clipboard_transfer_parent_not_absolute", "clipboard_transfer_parent_untrusted", "clipboard_transfer_write_failed", "clipboard_transport_closed", "clipboard_unexpected_ack",
    "clipboard_user_identity_failed", "clipboard_user_token_failed", "clipboard_window_full", "clipboard_write_failed", "unsupported_clipboard_format",
    // Appended (indices of earlier causes stay what recorded events mean): the interactive desktop was locked or a secure desktop (UAC) was showing.
    "clipboard_desktop_unavailable"};
  return causes;
}
// END-CAUSES
inline uint32_t clipboardCauseCode(const std::string& what) {
  const auto& causes = clipboardCauses();
  for (size_t i = 0; i < causes.size(); ++i) if (causes[i] == what) return uint32_t(i + 1);
  return 255;
}
// The service side: a `clip.diag` packet of the worker of session `sessionId` becomes one journal record. Anything out of range is
// dropped, never recorded as sent.
inline bool recordClipboardWorkerDiagnostic(const Json& packet, const std::string& sessionId) {
  if (packet.get("type").string() != "clip.diag" || packet.get("sessionId").string() != sessionId) return false;
  const auto number = [&](const char* key, double maximum) -> int64_t {
    const auto& item = packet.get(key);
    const auto* value = std::get_if<double>(&item.value);
    if (!value || !std::isfinite(*value) || *value < 0 || *value > maximum || std::floor(*value) != *value) return -1;
    return int64_t(*value);
  };
  const auto stage = number("stage", 99), error = number("error", 255), cause = number("cause", 255), format = number("format", 9), items = number("items", 65535), kib = number("kib", 4294967295.0);
  // win32 (optional; older workers do not send it): clip::writeFailureDetail of a failed transfer, 0 when no Windows call failed.
  const auto win32 = std::get_if<double>(&packet.get("win32").value) ? number("win32", 4294967295.0) : int64_t(0);
  // Optional read-time entry bound for an entry-limit failure. It describes
  // the negotiated cap actually used, never the number of entries in a copy.
  const auto* fields = std::get_if<Json::Object>(&packet.value);
  const bool hasExpected = fields && fields->find("expected") != fields->end();
  const auto expected = hasExpected ? number("expected", 200000) : int64_t(0);
  const auto* ok = std::get_if<bool>(&packet.get("ok").value);
  if (stage < 1 || error < 0 || cause < 0 || format < 0 || items < 0 || kib < 0 || win32 < 0 || expected < 0 || !ok) return false;
  if (expected > 0 && (*ok || cause != clipboardCauseCode("clipboard_entry_count_limit"))) return false;
  if (expected > 0) recordHostEvent("hostAction", {{"action", clipboardDiagnosticAction}, {"stage", stage}, {"ok", *ok ? 1 : 0}, {"error", error}, {"cause", cause}, {"win32", win32}, {"format", format}, {"items", items}, {"kib", kib}, {"expected", expected}});
  else recordHostEvent("hostAction", {{"action", clipboardDiagnosticAction}, {"stage", stage}, {"ok", *ok ? 1 : 0}, {"error", error}, {"cause", cause}, {"win32", win32}, {"format", format}, {"items", items}, {"kib", kib}});
  return true;
}
}  // namespace host
