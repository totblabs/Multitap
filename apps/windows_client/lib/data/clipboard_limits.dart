import '../l10n/l10n.dart';

/// Sent when the clipboard channel is opened so the relay knows this peer
/// understands the extended limits (a released relay ignores unknown headers).
const clipboardLimitsHeader = 'X-Multitap-Clipboard-Limits';
const clipboardLimitsHeaderValue = '2';

/// Sent in addition by a peer that can move rich text, images and folder trees
/// and uses the sliding window ("formats profile"). The relay tells both peers
/// about it only when both announced it; otherwise it behaves as before.
const clipboardFormatsHeader = 'X-Multitap-Clipboard-Formats';
const clipboardFormatsHeaderValue = '2';

/// Sent in addition (a relay that does not know it ignores it) by a peer that
/// can offer files and folders for pulling when the user pastes, and pull them
/// from the other side ("paste-pull profile"). The relay tells both peers only
/// when both announced it together with the formats profile.
const clipboardPullHeader = 'X-Multitap-Clipboard-Pull';
const clipboardPullHeaderValue = '1';

/// Sent in addition (a relay that does not know it ignores it) by a peer of the
/// scale revision: no limit on file sizes, totals or top-level items, text and
/// HTML up to 64 MiB, relative paths of 1024 characters on the wire (259 can be
/// pasted), an entry guard of 200,000. The relay tells both peers only when both
/// announced it together with the paste-pull profile.
const clipboardScaleHeader = 'X-Multitap-Clipboard-Scale';
const clipboardScaleHeaderValue = '1';

/// Sent in addition (a relay that does not know it ignores it) by a peer of the
/// on-paste revision (a copy is silent): files and folders are never
/// pushed at copy time, text and images only while small (1 MiB of text, 8 MiB of
/// PNG); everything else is announced and rendered when something pastes. The
/// relay tells both peers only when both announced it together with the scale
/// revision.
const clipboardOnPasteHeader = 'X-Multitap-Clipboard-OnPaste';
const clipboardOnPasteHeaderValue = '1';
const clipboardSettledHeader = 'X-Multitap-Clipboard-Settled';
const clipboardSettledHeaderValue = '1';
const clipboardSettledProtocol = 'text-image-250-v1';
const clipboardIntentHeader = 'X-Multitap-Clipboard-Intent';
const clipboardIntentHeaderValue = '1';
const clipboardIntentProtocol = 'text-image-pending-v1';

/// The relay's pull queue (on top of the on-paste revision): up to [clipboardPullQueueDepth] pulls of one requester outstanding, served
/// in order, each with a [clipboardPullQueueWindowBytes] / [clipboardPullQueueWindowUnits] window; the relay checks every chunk's base64.
const clipboardPullQueueHeader = 'X-Multitap-Clipboard-PullQueue';
const clipboardPullQueueHeaderValue = '1';
const clipboardPullQueueProtocol = 'pull-queue-4-v1';
const clipboardPullQueueDepth = 4;

/// Deferred manifest (on top of the on-paste revision, like RDP): a copy of files that is not small is announced without its list,
/// which is walked when the peer pastes (clipboard.manifest). docs/remote-session-protocol.md, "Deferred manifest".
const clipboardDeferredHeader = 'X-Multitap-Clipboard-Deferred';
const clipboardDeferredHeaderValue = '1';

/// This client takes a pair's clipboard.ready policy as its own (so a grant the administrator widens during the
/// stream applies when the relay replaces the pair). A client without it intersected the ready with the stream's
/// start and would refuse what the Host is then allowed to offer, so the relay widens only where it is sent.
const clipboardLivePolicyHeader = 'X-Multitap-Clipboard-Policy';
const clipboardLivePolicyHeaderValue = '1';
const clipboardDeferredProtocol = 'manifest-on-paste-v1';
const clipboardPullQueueWindowBytes = 4 * 1024 * 1024;
const clipboardPullQueueWindowUnits = 128;

/// A file this PC serves is read from the platform this many chunks at a time (one `pullRead` call, about 1 MiB); the native
/// adapter accepts up to readAheadChunks (native/windows_clipboard/pull.hpp) formats chunks per call.
const clipboardServeReadChunks = 22;

/// 2^53 - 1: sizes and offsets travel as JSON numbers, exact up to here.
const clipboardScaleBytes = 9007199254740991;

/// Explorer cannot create a pasted item whose relative path has this many
/// characters or more (MAX_PATH): a longer path is refused up front.
const clipboardPastePathChars = 259;

/// The size limits of one clipboard channel. The relay advertises the limits
/// that apply to the pair of peers in `clipboard.ready`: released Hosts and
/// clients only know [v1], so a pair that includes one of them stays on it and
/// the sender reports "too large" up front instead of failing mid-transfer.
class ClipboardLimits {
  const ClipboardLimits({
    required this.maxFiles,
    required this.maxFileBytes,
    required this.maxTotalBytes,
    required this.maxTextUnits,
    required this.maxTextBytes,
    required this.extended,
    this.formats = false,
    this.chunkBytes = 32768,
    this.windowUnits = 1,
    this.windowBytes = 32768,
    this.maxEntries = 0,
    this.maxDepth = 0,
    this.maxPathChars = 0,
    this.maxRichBytes = 0,
    this.maxImageBytes = 0,
    this.maxImagePixels = 0,
    this.pull = false,
    this.scale = false,
    this.onPaste = false,
  });

  /// Older released peers: 256 MiB per file, 512 MiB per
  /// transfer, 32768 text units, 128 KiB of UTF-8. Their exact values are
  /// checked by those peers, so they are never changed.
  static const v1 = ClipboardLimits(
    maxFiles: 128,
    maxFileBytes: 256 * 1024 * 1024,
    maxTotalBytes: 512 * 1024 * 1024,
    maxTextUnits: 32768,
    maxTextBytes: 128 * 1024,
    extended: false,
  );

  /// The v1 numbers with a relay that understands the newer failure reasons
  /// (the other peer of the pair is a released one).
  static const v1Extended = ClipboardLimits(
    maxFiles: 128,
    maxFileBytes: 256 * 1024 * 1024,
    maxTotalBytes: 512 * 1024 * 1024,
    maxTextUnits: 32768,
    maxTextBytes: 128 * 1024,
    extended: true,
  );

  /// 4 GiB per file, 8 GiB per transfer, 1 MiB of UTF-8 text, still 128 files.
  static const v2 = ClipboardLimits(
    maxFiles: 128,
    maxFileBytes: 4 * 1024 * 1024 * 1024,
    maxTotalBytes: 8 * 1024 * 1024 * 1024,
    maxTextUnits: 1024 * 1024,
    maxTextBytes: 1024 * 1024,
    extended: true,
  );

  /// The v2 numbers plus rich text (8 MiB of HTML / RTF), images (64 megapixels,
  /// 256 MiB of PNG), folder trees (10,000 entries, 32 levels, 160 characters of
  /// relative path) and a sliding window of 32 units / 1 MiB with 45 KiB chunks.
  static const formatsProfile = ClipboardLimits(
    maxFiles: 128,
    maxFileBytes: 4 * 1024 * 1024 * 1024,
    maxTotalBytes: 8 * 1024 * 1024 * 1024,
    maxTextUnits: 1024 * 1024,
    maxTextBytes: 1024 * 1024,
    extended: true,
    formats: true,
    chunkBytes: 46080,
    windowUnits: 32,
    windowBytes: 1024 * 1024,
    maxEntries: 10000,
    maxDepth: 32,
    maxPathChars: 160,
    maxRichBytes: 8 * 1024 * 1024,
    maxImageBytes: 256 * 1024 * 1024,
    maxImagePixels: 64000000,
  );

  /// The formats profile plus the paste-pull profile.
  static const pullProfile = ClipboardLimits(
    maxFiles: 128,
    maxFileBytes: 4 * 1024 * 1024 * 1024,
    maxTotalBytes: 8 * 1024 * 1024 * 1024,
    maxTextUnits: 1024 * 1024,
    maxTextBytes: 1024 * 1024,
    extended: true,
    formats: true,
    chunkBytes: 46080,
    windowUnits: 32,
    windowBytes: 1024 * 1024,
    maxEntries: 10000,
    maxDepth: 32,
    maxPathChars: 160,
    maxRichBytes: 8 * 1024 * 1024,
    maxImageBytes: 256 * 1024 * 1024,
    maxImagePixels: 64000000,
    pull: true,
  );

  /// The scale revision: files and totals have no limit (2^53 - 1 bytes),
  /// text and HTML 64 MiB, an entry guard of 200,000, 1024-character paths and
  /// components of 255. Everything big crosses as a paste-pull offer.
  static const scaleRevision = ClipboardLimits(
    maxFiles: 200000,
    maxFileBytes: clipboardScaleBytes,
    maxTotalBytes: clipboardScaleBytes,
    maxTextUnits: 64 * 1024 * 1024,
    maxTextBytes: 64 * 1024 * 1024,
    extended: true,
    formats: true,
    chunkBytes: 46080,
    windowUnits: 32,
    windowBytes: 1024 * 1024,
    maxEntries: 200000,
    maxDepth: 512,
    maxPathChars: 1024,
    maxRichBytes: 64 * 1024 * 1024,
    maxImageBytes: 256 * 1024 * 1024,
    maxImagePixels: 64000000,
    pull: true,
    scale: true,
  );

  /// The on-paste revision: the scale revision, and a copy is silent. Files and
  /// folders are always offered (never pushed); text and rich text above
  /// [pushTextBytes] and images above [pushImageBytes] are offered too.
  static const onPasteRevision = ClipboardLimits(
    maxFiles: 200000,
    maxFileBytes: clipboardScaleBytes,
    maxTotalBytes: clipboardScaleBytes,
    maxTextUnits: 64 * 1024 * 1024,
    maxTextBytes: 64 * 1024 * 1024,
    extended: true,
    formats: true,
    chunkBytes: 46080,
    windowUnits: 32,
    windowBytes: 1024 * 1024,
    maxEntries: 200000,
    maxDepth: 512,
    maxPathChars: 1024,
    maxRichBytes: 64 * 1024 * 1024,
    maxImageBytes: 256 * 1024 * 1024,
    maxImagePixels: 64000000,
    pull: true,
    scale: true,
    onPaste: true,
  );

  final int maxFiles, maxFileBytes, maxTotalBytes, maxTextUnits, maxTextBytes;

  /// The scale revision is negotiated for this pair (never without [pull]).
  final bool scale;

  /// The on-paste revision is negotiated for this pair (never without [scale]).
  final bool onPaste;

  /// Text (or text and HTML together) up to this many bytes, and a PNG up to
  /// [pushImageBytes], are still pushed at copy time in an on-paste pair.
  int get pushTextBytes => 1024 * 1024;
  int get pushImageBytes => 8 * 1024 * 1024;

  /// Characters of one name inside a transferred path.
  int get maxComponentChars => scale ? 255 : 240;

  /// The longest relative path that is sent: the profile's limit, and never more
  /// than Explorer can create at a paste destination.
  int get pastePathChars => maxPathChars < clipboardPastePathChars
      ? maxPathChars
      : clipboardPastePathChars;

  /// The relay and this pair understand the newer failure reasons
  /// (limits_exceeded, disk_full, file_locked) on the wire.
  final bool extended;

  /// Rich text, images and folder trees are allowed and the pair is windowed.
  final bool formats;

  /// Raw bytes per data chunk, and the sender's window (units = chunks and item
  /// ends not yet acknowledged; bytes = their data). A pair without the formats
  /// profile has a window of one chunk: stop and wait.
  final int chunkBytes, windowUnits, windowBytes;

  /// The paste-pull profile: files and folders are offered by their manifest
  /// and pulled when pasted (never true without [formats]).
  final bool pull;

  /// Offers kept per direction (the latest two) once [pull] is true.
  int get maxOffers => pull ? 2 : 0;

  /// Formats profile bounds (0 when the pair does not have it).
  final int maxEntries,
      maxDepth,
      maxPathChars,
      maxRichBytes,
      maxImageBytes,
      maxImagePixels;

  static const _v1Keys = {
    'chunkBytes',
    'maxFiles',
    'maxFileBytes',
    'maxTotalBytes',
    'maxTextUnits',
  };
  static const _formatsKeys = {
    'maxTextBytes',
    'limitsProfile',
    'formatsProfile',
    'maxEntries',
    'maxDepth',
    'maxPathChars',
    'maxRichBytes',
    'maxImageBytes',
    'maxImagePixels',
    'windowBytes',
    'windowUnits',
  };
  static const _pullKeys = {'pullProfile', 'maxOffers'};
  static const _scaleKeys = {'scaleProfile'};
  static const _onPasteKeys = {
    'onPasteProfile',
    'pushTextBytes',
    'pushImageBytes',
  };

  /// Reads the `limits` object of `clipboard.ready`. Returns null for anything
  /// that is not exactly one of the known profiles.
  static ClipboardLimits? parse(Object? value, {required int chunkBytes}) {
    if (value is! Map) return null;
    // The scale revision: the pull profile with exactly these numbers.
    if (value.containsKey('scaleProfile')) {
      final s = scaleRevision;
      // The on-paste revision adds exactly onPasteProfile, pushTextBytes and pushImageBytes to the scale numbers.
      final onPaste = value.containsKey('onPasteProfile');
      if (onPaste &&
          !(_onPasteKeys.every(value.containsKey) &&
              value['onPasteProfile'] == 1 &&
              value['pushTextBytes'] == onPasteRevision.pushTextBytes &&
              value['pushImageBytes'] == onPasteRevision.pushImageBytes)) {
        return null;
      }
      if (value.length !=
              _v1Keys.length +
                  _formatsKeys.length +
                  _pullKeys.length +
                  _scaleKeys.length +
                  (onPaste ? _onPasteKeys.length : 0) ||
          !_v1Keys.every(value.containsKey) ||
          !_formatsKeys.every(value.containsKey) ||
          !_pullKeys.every(value.containsKey)) {
        return null;
      }
      return value['scaleProfile'] == 1 &&
              value['pullProfile'] == 1 &&
              value['maxOffers'] == 2 &&
              value['limitsProfile'] == 2 &&
              value['formatsProfile'] == 2 &&
              value['chunkBytes'] == s.chunkBytes &&
              value['maxFiles'] == s.maxFiles &&
              value['maxFileBytes'] == s.maxFileBytes &&
              value['maxTotalBytes'] == s.maxTotalBytes &&
              value['maxTextUnits'] == s.maxTextUnits &&
              value['maxTextBytes'] == s.maxTextBytes &&
              value['maxEntries'] == s.maxEntries &&
              value['maxDepth'] == s.maxDepth &&
              value['maxPathChars'] == s.maxPathChars &&
              value['maxRichBytes'] == s.maxRichBytes &&
              value['maxImageBytes'] == s.maxImageBytes &&
              value['maxImagePixels'] == s.maxImagePixels &&
              value['windowBytes'] == s.windowBytes &&
              value['windowUnits'] == s.windowUnits
          ? (onPaste ? onPasteRevision : s)
          : null;
    }
    // The formats profile: every number is exactly the one both peers know.
    if (value['chunkBytes'] == formatsProfile.chunkBytes) {
      // The paste-pull profile adds exactly pullProfile and maxOffers to the formats numbers.
      final pull = value.containsKey('pullProfile');
      if (value.length !=
              _v1Keys.length +
                  _formatsKeys.length +
                  (pull ? _pullKeys.length : 0) ||
          !_v1Keys.every(value.containsKey) ||
          !_formatsKeys.every(value.containsKey) ||
          (pull && !_pullKeys.every(value.containsKey))) {
        return null;
      }
      final f = formatsProfile;
      return value['limitsProfile'] == 2 &&
              value['formatsProfile'] == 2 &&
              value['maxFiles'] == f.maxFiles &&
              value['maxFileBytes'] == f.maxFileBytes &&
              value['maxTotalBytes'] == f.maxTotalBytes &&
              value['maxTextUnits'] == f.maxTextUnits &&
              value['maxTextBytes'] == f.maxTextBytes &&
              value['maxEntries'] == f.maxEntries &&
              value['maxDepth'] == f.maxDepth &&
              value['maxPathChars'] == f.maxPathChars &&
              value['maxRichBytes'] == f.maxRichBytes &&
              value['maxImageBytes'] == f.maxImageBytes &&
              value['maxImagePixels'] == f.maxImagePixels &&
              value['windowBytes'] == f.windowBytes &&
              value['windowUnits'] == f.windowUnits &&
              (!pull || (value['pullProfile'] == 1 && value['maxOffers'] == 2))
          ? (pull ? pullProfile : f)
          : null;
    }
    if (value['chunkBytes'] != chunkBytes) return null;
    if (value.length == _v1Keys.length &&
        _v1Keys.every(value.containsKey) &&
        value['maxFiles'] == v1.maxFiles &&
        value['maxFileBytes'] == v1.maxFileBytes &&
        value['maxTotalBytes'] == v1.maxTotalBytes &&
        value['maxTextUnits'] == v1.maxTextUnits) {
      return v1;
    }
    // The newer relay adds maxTextBytes and limitsProfile to every limits
    // object it sends to a newer peer, with the pair's effective numbers.
    if (value.length == _v1Keys.length + 2 &&
        _v1Keys.every(value.containsKey) &&
        value['limitsProfile'] == 2 &&
        value['maxFiles'] == v1.maxFiles) {
      final bytes = value['maxTextBytes'];
      if (value['maxFileBytes'] == v1.maxFileBytes &&
          value['maxTotalBytes'] == v1.maxTotalBytes &&
          value['maxTextUnits'] == v1.maxTextUnits &&
          bytes == v1.maxTextBytes) {
        return v1Extended;
      }
      if (value['maxFileBytes'] == v2.maxFileBytes &&
          value['maxTotalBytes'] == v2.maxTotalBytes &&
          value['maxTextUnits'] == v2.maxTextUnits &&
          bytes == v2.maxTextBytes) {
        return v2;
      }
    }
    return null;
  }

  /// "4GB", "256MB", "128KB": the largest whole binary unit.
  static String size(int bytes) {
    const gib = 1024 * 1024 * 1024, mib = 1024 * 1024, kib = 1024;
    if (bytes >= gib && bytes % gib == 0) return '${bytes ~/ gib} GB';
    if (bytes >= mib && bytes % mib == 0) return '${bytes ~/ mib} MB';
    if (bytes >= kib && bytes % kib == 0) return '${bytes ~/ kib} KB';
    return l10n.text6(bytes);
  }

  /// `200000` as `200,000`.
  static String number(int value) {
    final digits = value.toString();
    final out = StringBuffer();
    for (var i = 0; i < digits.length; i++) {
      if (i > 0 && (digits.length - i) % 3 == 0) out.write(',');
      out.write(digits[i]);
    }
    return out.toString();
  }

  /// Why a limit stopped a transfer: `text`, `count`, `file` (one file too
  /// large), `total` (all files together), and with the formats profile
  /// `image`, `entries`, `depth` and `path` (`path:<n>` names the longest
  /// relative path, n characters); null when unknown (a peer or the relay
  /// reported it without saying which). A pair of the scale revision always
  /// says which limit it met.
  ///
  /// [sharedFolders]: this client has 공유 폴더 (Windows). Android has none,
  /// so its advice never points there.
  String message([String? detail, bool sharedFolders = true]) {
    final pack = sharedFolders ? l10n.zipThemMoveThemShared : l10n.zipThemFirst;
    final big = sharedFolders
        ? l10n.moveLargeFilesSharedFolder
        : l10n.splitThemIntoSmallerFiles;
    final kind = detail?.split(':').first;
    final chars = detail != null && detail.contains(':')
        ? int.tryParse(detail.split(':')[1])
        : null;
    final files = l10n.upPerFileOnce(size(maxFileBytes), size(maxTotalBytes));
    switch (kind) {
      case 'text':
        return l10n.textTooLongYouCan(size(maxTextBytes));
      case 'count':
        return l10n.tooManyFilesYouCan(maxFiles, pack);
      case 'file' || 'total':
        return l10n.fileTooLargeClipboardCan(files, big);
      case 'image':
        return l10n.imageTooLargeYouCan(size(formats ? maxImageBytes : 256 * 1024 * 1024));
      case 'entries':
        return l10n.tooManyItemsFolderYou(number(maxEntries > 0 ? maxEntries : 10000), pack);
      case 'depth':
        return l10n.folderTooDeepYouCan(maxDepth > 0 ? maxDepth : 32, pack);
      case 'path':
        // Explorer cannot paste a path of 260 or more characters, whatever the wire allows.
        final limit = maxPathChars > 0 ? pastePathChars : 160;
        return l10n.someFilesHavePathsToo(chars != null ? l10n.longestPathCharacters(chars) : '', limit, pack);
      default:
        if (scale) return l10n.overClipboardTransferLimit;
        return l10n.overClipboardLimitYouCan(files, maxFiles, size(maxTextBytes), big);
    }
  }
}
