// Pure operation state-machine and Windows security-descriptor QA. None of the
// injected callbacks can operate on PnP, HKLM, services or real driver packages.
#define wmain display_command_main
#include "display.cpp"
#undef wmain
namespace {
using namespace repair;
static size_t cases=0;
static Value str(const std::wstring& text){Value value{true,REG_SZ,std::vector<BYTE>((text.size()+1)*sizeof(wchar_t))};std::memcpy(value.bytes.data(),text.c_str(),value.bytes.size());return value;}
static Device device(const wchar_t* id,const wchar_t* name,const wchar_t* hardware){Device value;value.id=id;value.name=name;value.hardware={hardware};value.ready=true;return value;}
struct Fake {
  Snapshot state;bool administrator=true,trusted=true,backupValid=true,installFails=false,installLeavesNewDevice=false,acknowledgedButPresent=false,removeThenFail=false;
  int removeFailure=-1;size_t verified=0,backedUp=0,removed=0,cleared=0,installed=0,restored=0;
  std::function<void()> afterBackup,afterRemoval;
  std::string configFile="existing configuration must be preserved",driverStore="shared signed driver store must be preserved";
  Fake(){state.packagePath=L"C:\\Program Files\\Multitap\\driver";state.values[0]=str(L"C:\\Old virtual display");state.devices={device(L"ROOT\\BASIC\\0000",L"Microsoft Basic Display",L"ROOT\\BasicDisplay"),device(L"ROOT\\REMOTE\\0000",L"Microsoft Remote Display",L"ROOT\\RdpIdd_IndirectDisplay"),device(L"PCI\\PROXMOX\\0000",L"Proxmox console",L"PCI\\VEN_1234&DEV_1111"),device(L"ROOT\\OTHER\\0000",L"Unrelated virtual display",L"ROOT\\OtherDisplay"),device(L"ROOT\\MTTVDD\\0000",L"Old shared virtual display",kHardware)};state=plan(state).snapshot;}
  Access access(){return{
    [this]{return administrator;},
    [this]{++verified;require(trusted,"driver_signature_untrusted");},
    [this]{return state;},
    [this](const Snapshot& snapshot){++backedUp;const auto bytes=serialized(snapshot);require(backupValid&&digest(bytes)==digest(serialized(plan(state).snapshot)),"repair_backup_verification_failed");if(afterBackup)afterBackup();return std::string(32,'a');},
    [this](const Device& selected){require(target(selected),"unsafe_test_removal");if(removeFailure>=0&&removed==size_t(removeFailure))throw std::runtime_error("fixture_removal_failed");++removed;if(!acknowledgedButPresent)std::erase_if(state.devices,[&](const Device& current){return current.id==selected.id;});if(afterRemoval)afterRemoval();if(removeThenFail)throw std::runtime_error("fixture_removal_failed");},
    [this](size_t index,const Value& before){require(state.values[index]==before,"repair_registry_changed");++cleared;state.values[index]={};},
    [this](const Snapshot& before,const std::vector<size_t>& changed){++restored;for(const auto& current:state.devices)if(target(current)&&std::none_of(before.devices.begin(),before.devices.end(),[&](const Device& old){return old==current;}))return false;for(const auto index:changed)if(state.values[index].present&&state.values[index]!=before.values[index])return false;for(const auto index:changed)state.values[index]=before.values[index];return true;},
    [this](Result& result){++installed;if(installLeavesNewDevice)state.devices.push_back(device(L"ROOT\\MTTVDD\\NEW",L"New unresolved virtual device",kHardware));if(installFails)throw std::runtime_error("fixture_install_failed");result.installed=true;result.configurationPending=true;}
  };}
  Result run(){const auto id=plan(state).id;return execute(id,access());}
};
template<class F>static void check(F work){work();++cases;}
static void expect(const std::function<void()>& work,const char* reason){try{work();throw std::runtime_error("missing_expected_rejection");}catch(const std::exception& error){if(std::string(error.what())!=reason)throw std::runtime_error(std::string("unexpected_rejection_reason:")+error.what());}}
static void tests(){
  check([]{Fake f;const auto value=plan(f.state);require(value.devices.size()==1&&value.devices[0].id==L"ROOT\\MTTVDD\\0000"&&value.values==std::vector<size_t>{0},"exact_target_plan_failed");const auto output=planJson(value,true,"");require(output.find("Microsoft")==std::string::npos&&output.find("Proxmox")==std::string::npos&&output.find("Unrelated")==std::string::npos&&output.find("Old shared")!=std::string::npos,"unrelated_device_exposed_as_removal_target");});
  check([]{Fake f;std::erase_if(f.state.devices,target);const auto value=plan(f.state);require(value.devices.empty()&&value.values.size()==1,"orphan_configuration_plan_failed");const auto result=f.run();require(result.installed&&result.removed==0&&f.cleared==1&&f.installed==1,"orphan_configuration_repair_failed");});
  check([]{Fake f;f.state.values[1]=str(L"PCI\\PROXMOX\\0000");f.state.values[2]=str(L"C:\\Old virtual display");const auto value=plan(f.state);require(value.devices.size()==1&&value.devices[0].id!=L"PCI\\PROXMOX\\0000"&&value.values.size()==3,"stale_owner_selected_physical_device");});
  check([]{Fake f;f.state.values[1]=str(L"ROOT\\MTTVDD\\0000");f.state.values[2]=f.state.values[0];const auto value=plan(f.state);require(value.devices.empty()&&value.values.empty(),"healthy_owned_device_selected");require(f.run().reason=="display_repair_not_required"&&f.backedUp==0,"healthy_owned_device_reinstalled");});
  check([]{Fake f;const auto original=f.state;auto access=f.access();const auto id=plan(f.state).id;f.state.values[0]=str(L"C:\\Changed");const auto result=execute(id,access);require(result.reason=="repair_plan_changed"&&f.backedUp==0&&f.removed==0&&f.cleared==0&&original.values[0]!=f.state.values[0],"stale_plan_mutated_os");});
  check([]{Fake f;f.administrator=false;require(f.run().reason=="administrator_required"&&f.verified==0&&f.backedUp==0,"unprivileged_repair_reached_trust_or_mutation");});
  check([]{Fake f;f.trusted=false;require(f.run().reason=="driver_signature_untrusted"&&f.backedUp==0&&f.removed==0,"untrusted_package_reached_mutation");});
  check([]{Fake f;f.backupValid=false;require(f.run().reason=="repair_backup_verification_failed"&&f.removed==0&&f.cleared==0,"unverified_backup_reached_mutation");});
  check([]{Fake f;f.afterBackup=[&]{f.state.devices.push_back(device(L"ROOT\\MTTVDD\\NEW",L"Racing device",kHardware));};const auto result=f.run();require(result.reason=="repair_state_changed"&&f.removed==0&&f.cleared==0,"new_conflict_race_was_removed");});
  check([]{Fake f;f.afterRemoval=[&]{f.state.devices.push_back(device(L"ROOT\\OTHER\\NEW",L"Racing unrelated display",L"ROOT\\Unrelated"));};const auto result=f.run();require(result.reason=="repair_state_changed"&&result.partialRemoval&&result.removed==1&&f.cleared==0&&f.state.values[0].present,"partial_removal_race_not_reported");});
  check([]{Fake f;f.state.devices.push_back(device(L"ROOT\\MTTVDD\\0001",L"Second shared virtual display",kHardware));f.removeFailure=1;const auto result=f.run();require(result.partialRemoval&&result.removed==1&&result.removeRequested==1&&f.cleared==0&&f.state.devices.size()==5,"partial_device_failure_not_reported");});
  check([]{Fake f;f.acknowledgedButPresent=true;const auto result=f.run();require(!result.installed&&result.removed==0&&result.removeRequested==1&&result.removalUncertain&&f.cleared==0,"pending_pnp_removal_claimed_complete");});
  check([]{Fake f;f.removeThenFail=true;const auto result=f.run();require(result.partialRemoval&&result.removed==1&&result.removeRequested==0&&result.removalUncertain,"failed_pnp_side_effect_not_observed");});
  check([]{Fake f;f.installFails=true;const auto original=f.state.values;const auto result=f.run();require(!result.installed&&result.partialRemoval&&result.configurationRestored&&f.state.values==original&&f.restored==1&&f.state.devices.size()==4,"failed_install_did_not_restore_configuration_only");});
  check([]{Fake f;f.installFails=true;f.installLeavesNewDevice=true;const auto result=f.run();require(!result.installed&&result.partialRemoval&&!result.configurationRestored&&f.restored==1&&!f.state.values[0].present,"new_device_configuration_was_overwritten_on_recovery");});
  check([]{Fake f;f.state.values={};f.installFails=true;f.installLeavesNewDevice=true;const auto result=f.run();require(!result.installed&&result.configurationRestoreAttempted&&!result.configurationRestored&&f.restored==1,"failed_install_without_prior_values_claimed_restored");});
  check([]{Fake f;auto access=f.access();access.installTrusted=[&](Result&){f.state.values[0]=str(L"C:\\External concurrent writer");throw std::runtime_error("fixture_install_failed");};const auto result=execute(plan(f.state).id,access);require(!result.configurationRestored&&valueText(f.state.values[0])==L"C:\\External concurrent writer","external_registry_change_was_overwritten");});
  check([]{Fake f;const auto before=f.state.devices;const auto result=f.run();require(result.installed&&result.cleanupCompleted&&!result.partialRemoval&&result.removed==1&&f.cleared==1&&f.installed==1&&f.backedUp==1,"successful_repair_not_confirmed");for(const auto& item:before)if(!target(item))require(std::find(f.state.devices.begin(),f.state.devices.end(),item)!=f.state.devices.end(),"unrelated_device_changed");require(f.configFile=="existing configuration must be preserved"&&f.driverStore=="shared signed driver store must be preserved","non_target_files_changed");});
  check([]{Fake f;f.state.values[0].type=REG_BINARY;expect([&]{plan(f.state);},"repair_registry_type_unsupported");});
  check([]{Fake f;const auto selected=std::find_if(f.state.devices.begin(),f.state.devices.end(),target);require(selected!=f.state.devices.end(),"missing_target_fixture");selected->hardware.push_back(L"ROOT\\Unknown");expect([&]{plan(f.state);},"repair_ambiguous_hardware_id");});
  check([]{Fake f;auto access=f.access();const auto result=execute(std::string(64,'z'),access);require(result.reason=="invalid_repair_plan"&&f.verified==0&&f.backedUp==0,"invalid_plan_id_reached_mutation");});
  check([]{Fake f;auto changed=f.state;changed.devices[0].driverIdentity=L"Different binding";require(plan(f.state).id!=plan(changed).id,"changed_driver_binding_not_in_plan_digest");changed=f.state;changed.values[0].bytes[0]^=1;require(plan(f.state).id!=plan(changed).id,"registry_bytes_not_in_plan_digest");});
  check([]{for(const auto& [sddl,expected]:std::vector<std::pair<const wchar_t*,bool>>{{L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",true},{L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)",false},{L"O:BUG:BUD:P(A;;GA;;;BU)",false},{L"O:BAG:BAD:NO_ACCESS_CONTROL",false}}){PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr),"security_fixture_invalid");const auto safe=trustedDescriptor(descriptor);LocalFree(descriptor);require(safe==expected,"unsafe_acl_accepted");}});
  check([]{expect([]{viewport::DirectoryLock lock(fs::path(L"relative\\directory"));},"viewport_package_path_invalid");expect([]{viewport::DirectoryLock lock(fs::path(L"C:\\..\\directory"));},"viewport_package_path_invalid");});
  check([]{Fake f;const auto bytes=serialized(f.state);require(digest(bytes)!=digest(bytes+"changed"),"backup_digest_corruption_not_detected");const auto output=planJson(plan(f.state),true,"");require(std::all_of(output.begin(),output.end(),[](unsigned char value){return value>=0x20&&value<0x7f;}),"plan_json_not_ascii");});
}
}
int wmain(){try{tests();std::cout<<"{\"repairModelTestsPassed\":true,\"cases\":"<<cases<<",\"actualSecurityDescriptorChecks\":4,\"actualPathValidationChecks\":2,\"pnpChangesPerformed\":0,\"registryChangesPerformed\":0,\"serviceChangesPerformed\":0,\"driverStoreChangesPerformed\":0,\"realRepairAcceptance\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<"case "<<cases+1<<": "<<error.what()<<'\n';return 1;}}
