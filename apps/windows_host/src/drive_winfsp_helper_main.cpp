// `MultitapHost.exe --drive-fs` entry (docs/drive-redirection.md, "WinFsp backend").
// Its own translation unit: the WinFsp SDK header pulls in <winternl.h>, which
// cannot share a unit with the Host's other Windows headers (UNICODE_STRING).
#include "drive_winfsp_helper.hpp"

namespace host::drive::fs {
int driveFsHelperEntry() { return driveFsHelper(); }
}
