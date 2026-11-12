// Unshipped regression executable. All device operations use sequence fixtures.
#define wmain display_command_main
#include "display.cpp"
#undef wmain

struct CatalogFixture {
  bool configuredInitial{}, loaded{}, rollbackOk=true, cleanup{}, journalSaved{},
      configuredAfter{}, failVerify{};
  std::string failAt, xml="original", next="catalog";
  std::vector<std::string> calls;
  bool configured() const { return configuredInitial; }
  unsigned verifiedModes() const { return loaded && !failVerify ? 18 : 2; }
  unsigned verifiedSources() const { return 2; }
  const std::string &currentXml() const { return xml; }
  const std::string &nextXml() const { return next; }
  void step(const char *name) {
    calls.emplace_back(name);
    if (failAt == name) throw std::runtime_error("mode_catalog_registration_timeout");
  }
  void saveOriginal() { step("original"); }
  void saveJournal() { step("journal"); journalSaved=true; }
  void replaceXml(const std::string &value) { step("replace"); xml=value; }
  void restart() { step("restart"); }
  void waitCatalog() { step("wait"); loaded=true; }
  void restoreTopology() { step("topology"); }
  void clearJournal() { step("clear"); cleanup=true; journalSaved=false; }
  bool rollback(bool &) {
    calls.emplace_back("rollback");
    if (rollbackOk) { xml="original"; journalSaved=false; }
    return rollbackOk;
  }
  bool configuredAfterRollback() const { return configuredAfter; }
};
int wmain(int argc, wchar_t **argv) {
  try {
    require(argc == 2, "mode_catalog_test_requires_original_xml");
    const fs::path original(argv[1]);
    require(sha256(original) == viewport::originalHash, "mode_catalog_original_pin_changed");
    std::ifstream file(original, std::ios::binary);
    const std::string base((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned cases=0;
    const auto modes=mode_catalog::plan({}, {});
    require(modes.size()==16 && mode_catalog::configured(modes) &&
        mode_catalog::count(common_modes::all())==18, "mode_catalog_common_plan_failed"); ++cases;
    for(size_t i=0;i<common_modes::all().size();++i) {
      viewport::validMode(common_modes::all()[i]);
      for(size_t j=0;j<i;++j)
        require(!common_modes::same(common_modes::all()[i],common_modes::all()[j]), "mode_catalog_duplicate_mode");
    } ++cases;
    const auto xml=viewport::generated(base,modes);
    require(viewport::addedModes(base,xml).size()==16 &&
        viewport::generated(base,mode_catalog::plan(modes,{}))==xml &&
        base.find(viewport::beginMark)==std::string::npos,
        "mode_catalog_original_or_idempotence_failed"); ++cases;
    const std::vector<Mode> custom={{1584,884,60},{2200,1100,60}};
    const auto upgraded=mode_catalog::plan(custom,{});
    require(upgraded.size()==18 && common_modes::same(upgraded[0],custom[0]) &&
        common_modes::same(upgraded[1],custom[1]) &&
        viewport::addedModes(base,viewport::generated(base,upgraded)).size()==18,
        "mode_catalog_old_runtime_upgrade_failed"); ++cases;
    std::vector<Mode> full;
    for(unsigned i=0;i<32;++i) full.push_back({320+i*2,180,60});
    const std::vector<viewport::Owned> active={{7,{L"\\\\.\\DISPLAY2",0,0,320,180,true,60}},
        {9,{L"\\\\.\\DISPLAY3",320,0,322,180,false,60}}};
    auto bounded=mode_catalog::plan(full,active);
    require(bounded.size()==32 && mode_catalog::configured(bounded) &&
        std::any_of(bounded.begin(),bounded.end(),[&](const Mode &m){return common_modes::same(m,full[0]);}) &&
        std::any_of(bounded.begin(),bounded.end(),[&](const Mode &m){return common_modes::same(m,full[1]);}),
        "mode_catalog_retired_active_mode"); ++cases;
    for(unsigned i=0;i<40;++i) viewport::cacheMode(bounded,{1000+i*2,600,60},active);
    require(bounded.size()==32 && mode_catalog::configured(bounded) &&
        std::any_of(bounded.begin(),bounded.end(),[&](const Mode &m){return common_modes::same(m,full[0]);}),
        "mode_catalog_dynamic_cache_evicted_common_mode"); ++cases;
    auto tampered=xml; tampered.replace(tampered.find("false"),5,"true");
    bool refused=false; try { viewport::addedModes(base,tampered); } catch(...) {refused=true;}
    require(refused,"mode_catalog_external_edit_accepted"); ++cases;

    Status state; state.owned=state.ready=true;state.deviceCount=1;
    state.displays={{L"\\\\.\\DISPLAY2",0,0,1920,1080,true,60}};
    require(!*mode_catalog::guard(state,false,true,true),"mode_catalog_ready_guard_failed");++cases;
    require(std::string(mode_catalog::guard(state,false,false,true))=="interactive_console_required", "mode_catalog_rdp_not_deferred");++cases;
    require(std::string(mode_catalog::guard(state,false,true,false))=="interactive_console_required", "mode_catalog_locked_not_deferred");++cases;
    require(std::string(mode_catalog::guard(state,true,false,false))=="viewport_recovery_required", "mode_catalog_recovery_hidden_by_rdp");++cases;
    state.foreignDeviceCount=1;
    require(std::string(mode_catalog::guard(state,false,false,false))=="existing_virtual_display_requires_review", "mode_catalog_conflict_hidden_by_rdp");++cases;
    state.foreignDeviceCount=0;state.vendorPathConflict=true;
    require(std::string(mode_catalog::guard(state,false,false,false))=="shared_vendor_path_conflict", "mode_catalog_vendor_conflict_hidden");++cases;
    state.vendorPathConflict=false;state.restartRequired=true;
    require(std::string(mode_catalog::guard(state,false,false,false))=="reboot_required", "mode_catalog_reboot_not_deferred");++cases;
    state.restartRequired=false;state.ready=false;
    require(std::string(mode_catalog::guard(state,false,true,true))=="owned_virtual_display_not_ready", "mode_catalog_device_pending_not_deferred");++cases;
    state.ready=true;state.displays.clear();
    require(std::string(mode_catalog::guard(state,false,true,true))=="owned_virtual_output_inactive", "mode_catalog_zero_output_not_deferred");++cases;
    state.owned=false;
    require(!mode_catalog::deferred(mode_catalog::guard(state,false,false,false)), "mode_catalog_missing_owner_was_deferred");++cases;

    CatalogFixture success;
    const auto applied=mode_catalog::transaction(success);
    require(applied.ready && applied.applied && applied.changed && applied.restarted && !applied.pending &&
        success.calls==std::vector<std::string>({"original","journal","replace","restart","wait","topology","clear"}),
        "mode_catalog_transaction_order_failed");++cases;
    CatalogFixture noChange;noChange.configuredInitial=noChange.loaded=true;noChange.xml=noChange.next;
    const auto unchanged=mode_catalog::transaction(noChange);
    require(unchanged.ready && !unchanged.changed && !unchanged.restartAttempted && noChange.calls.empty(),"mode_catalog_noop_restarted_device");++cases;
    CatalogFixture reload;reload.configuredInitial=true;reload.xml=reload.next;
    const auto reloaded=mode_catalog::transaction(reload);
    require(reloaded.ready && !reloaded.changed && reloaded.restarted &&
        std::find(reload.calls.begin(),reload.calls.end(),"replace")==reload.calls.end(),"mode_catalog_xml_only_false_success");++cases;
    for(const auto stage:{"replace","restart","wait","topology","clear"}) {
      CatalogFixture failed;failed.failAt=stage;
      const auto result=mode_catalog::transaction(failed);
      require(!result.ready && !result.applied && result.pending && result.rollbackAttempted && result.rollbackSucceeded &&
          failed.xml=="original" && failed.calls.back()=="rollback", "mode_catalog_failure_lost_recovery");++cases;
    }
    CatalogFixture uncertain;uncertain.failAt="wait";uncertain.rollbackOk=false;
    const auto partial=mode_catalog::transaction(uncertain);
    require(!partial.ready && partial.pending && partial.rollbackAttempted && !partial.rollbackSucceeded &&
        partial.reason=="viewport_rollback_incomplete" && uncertain.journalSaved,"mode_catalog_partial_failure_false_success");++cases;
    CatalogFixture verify;verify.failVerify=true;
    const auto invalid=mode_catalog::transaction(verify);
    require(!invalid.ready && invalid.rollbackAttempted && invalid.reason=="mode_catalog_not_verified", "mode_catalog_mode_verification_bypassed");++cases;
    for(const auto stage:{"original","journal"}) {
      CatalogFixture failed;failed.failAt=stage;bool threw=false;
      try{mode_catalog::transaction(failed);}catch(...){threw=true;}
      require(threw && failed.xml=="original" &&
          std::find(failed.calls.begin(),failed.calls.end(),"restart")==failed.calls.end(),"mode_catalog_backup_failure_mutated_device");++cases;
    }
    require(mode_catalog::safeReason("secret-or-file-path")=="mode_catalog_failed" &&
        mode_catalog::safeReason("raw_secret_token")=="mode_catalog_failed", "mode_catalog_raw_error_leak");++cases;
    std::cout<<"{\"modeCatalogTestsPassed\":true,\"cases\":"<<cases
        <<",\"requiredModes\":18,\"runtimeExtraModes\":16,\"originalPinnedXmlPreserved\":true,"
          "\"oldRuntimeXmlUpgraded\":true,\"commonAndActiveModesPinnedInCache\":true,"
          "\"transactionSequenceAndFailuresChecked\":true,\"deviceApiCalled\":false,"
          "\"registryChanged\":false,\"installedDriverRuntimeTest\":false}\n";
    return 0;
  }catch(const std::exception &failure){std::cerr<<failure.what()<<'\n';return 1;}
}
