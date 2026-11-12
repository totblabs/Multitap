/// session-resume-v1 (Broker server/src/session-resume.mjs): a short loss of
/// the stream's socket no longer ends the session. The broker keeps it for a
/// grace window and this client continues it on a new socket with the
/// single-use token the broker gave the previous one; the picture restarts at
/// an IDR. The token is never logged or stored.
const sessionResumeProtocol = 'session-resume-v1';

/// The request header that carries the resume token.
const sessionResumeHeader = 'X-Multitap-Resume';

/// The desktop list shows the marker where its broker can resume (a Host that
/// codes an IDR on request); a released broker never shows it and refuses the
/// connect field.
bool sessionResumeAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['sessionResumeProtocol'] == sessionResumeProtocol;

final _token = RegExp(r'^[A-Za-z0-9_-]{43}$');

/// The token of a `session.resume.token` or `session.resumed` message, or null.
String? resumeToken(Object? value) =>
    value is String && _token.hasMatch(value) ? value : null;

/// A grace window the broker may announce: 1 s to 2 min.
int? validResumeGraceMs(Object? value) =>
    value is int && value >= 1000 && value <= 120000 ? value : null;
