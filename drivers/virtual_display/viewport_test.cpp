// Standalone QA executable; never included in the product package.
#define wmain display_command_main
#include "display.cpp"
#undef wmain
int wmain(int argc, wchar_t **argv) {
  try {
    require(argc == 3 && elevated(), "Use an elevated own-file QA invocation.");
    const auto root = fs::absolute(argv[1]);
    const auto leaf = root.filename().wstring();
    require(!fs::exists(root) && leaf.size() == 36 &&
                leaf.rfind(L"run-", 0) == 0 &&
                leaf.find_first_not_of(L"run-0123456789abcdef") ==
                    std::wstring::npos,
            "Invalid fresh own QA directory.");
    const fs::path original(argv[2]);
    require(sha256(original) == viewport::originalHash,
            "Unexpected original settings fixture.");
    std::ifstream input(original, std::ios::binary);
    const std::string base((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    require(!base.empty(), "Cannot read original settings fixture.");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                &descriptor, nullptr),
            "QA ACL failed.");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const bool created = CreateDirectoryW(root.c_str(), &security) != FALSE;
    LocalFree(descriptor);
    require(created, "Cannot create own private fixture directory.");
    {
      viewport::DirectoryLock lock(root);
      const auto settings = root / L"vdd_settings.xml",
                 backup = root / viewport::backupName,
                 pending = root / viewport::journalName,
                 untouched = root / L"unrelated.xml";
      viewport::writeNew(settings, base, true);
      viewport::writeNew(backup, base);
      viewport::writeNew(untouched, "unrelated own QA sentinel");
      const auto updated =
          viewport::generated(base, {{1584, 884, 60}, {2200, 1100, 60}});
      viewport::Snapshot before;
      before.instance = L"ROOT\\DISPLAY\\QA-NOT-A-REAL-DEVICE";
      before.xml = base;
      before.target = 11;
      before.owned = {
          {11, {L"\\\\.\\DISPLAY2", 1024, 0, 1920, 1080, false, 60}}};
      before.external = {{L"\\\\.\\DISPLAY1", 0, 0, 1024, 768, true, 60}};
      viewport::writeNew(pending, viewport::journal(before));
      const auto firstUpdate = viewport::generated(base, {{1584, 884, 60}});
      viewport::replace(settings, firstUpdate);
      require(viewport::baseXml(root) == base &&
                  viewport::addedModes(viewport::baseXml(root),
                                       viewport::readFile(settings))
                          .size() == 1,
              "First generated runtime XML failed read-only validation.");
      viewport::replace(settings, updated);
      require(viewport::readFile(settings) == updated &&
                  viewport::baseXml(root) == base &&
                  viewport::addedModes(viewport::baseXml(root),
                                       viewport::readFile(settings))
                          .size() == 2 &&
                  viewport::readJournal(viewport::readFile(pending)).xml ==
                      base,
              "Actual journal/update bytes differ.");
      // Exercise the production atomic-file primitive under a sharing failure.
      {
        viewport::Handle busy(CreateFileW(settings.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, 0, nullptr));
        require(busy.h != INVALID_HANDLE_VALUE,
                "Cannot hold own settings fixture.");
        bool refused = false;
        try {
          viewport::replace(settings, base);
        } catch (...) {
          refused = true;
        }
        require(refused, "A busy target did not refuse replacement.");
      }
      require(viewport::readFile(settings) == updated && fs::exists(pending),
              "Sharing failure changed bytes or discarded recovery data.");
      viewport::replace(settings,
                        viewport::readJournal(viewport::readFile(pending)).xml);
      require(viewport::readFile(settings) == base &&
                  sha256(backup) == viewport::originalHash,
              "Original XML bytes were not restored.");
      require(DeleteFileW(settings.c_str()),
              "Cannot remove own missing-target fixture.");
      viewport::replace(settings, base);
      require(viewport::readFile(settings) == base,
              "Exact missing-target recovery failed.");
      const auto link = root / L"reparse.xml";
      require(CreateSymbolicLinkW(link.c_str(), settings.c_str(), 0),
              "Cannot create own reparse fixture.");
      bool refused = false;
      try {
        viewport::readFile(link);
      } catch (...) {
        refused = true;
      }
      require(refused, "Reparse settings were accepted.");
      require(DeleteFileW(link.c_str()), "Cannot remove own reparse fixture.");
      require(viewport::readFile(untouched) == "unrelated own QA sentinel",
              "An unrelated file changed.");
      for (const auto &path : {settings, backup, pending, untouched})
        require(DeleteFileW(path.c_str()),
                "Cannot remove an exact own QA file.");
      require(fs::is_empty(root), "Own file QA leaked a temporary file.");
    }
    require(RemoveDirectoryW(root.c_str()),
            "Cannot remove the empty own QA directory.");
    std::cout
        << "{\"viewportActualFileTestsPassed\":true,\"privateAclChecked\":true,"
           "\"atomicReplacementChecked\":true,\"busyTargetPreserved\":true,"
           "\"successiveRuntimeXmlReadOnlyValidation\":true,"
           "\"journalRestorationChecked\":true,"
           "\"missingTargetRecoveryChecked\":true,\"reparseRejected\":true,"
           "\"unrelatedFilesPreserved\":true,\"deviceApiCalled\":false,"
           "\"driverInstalled\":false,\"operatingSystemRebooted\":false}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
