// Pure CCD candidate fixtures only: no display query or mutation is invoked.
#define wmain display_command_main
#include "display.cpp"
#undef wmain

namespace {
viewport::Source source(UINT32 id, UINT32 connector, bool active) {
  viewport::Source value;
  value.name=L"\\\\.\\DISPLAY"+std::to_wstring(id+2);
  value.connector=connector;
  value.path.sourceInfo.adapterId={7,1};
  value.path.sourceInfo.id=id;
  value.path.targetInfo.adapterId={7,1};
  value.path.targetInfo.id=100+connector;
  value.path.targetInfo.targetAvailable=TRUE;
  value.path.flags=active?DISPLAYCONFIG_PATH_ACTIVE:0;
  return value;
}
void selected(const std::vector<viewport::Source>& candidates, UINT32 first, UINT32 second) {
  const auto result=viewport::selectSources(candidates);
  require(result.size()==2&&result[0].connector==0&&result[1].connector==1&&
      result[0].path.sourceInfo.id==first&&result[1].path.sourceInfo.id==second,
      "Incorrect owned source/connector pairing");
}
void refused(const std::vector<viewport::Source>& candidates) {
  bool failed=false;try{viewport::selectSources(candidates);}catch(...){failed=true;}
  require(failed,"Ambiguous or cross-adapter mapping was accepted");
}
}
int wmain() {
  try {
    std::vector<viewport::Source> grid{source(0,0,true),source(1,1,true),source(0,1,false),source(1,0,false)};
    std::vector<int> order{0,1,2,3};unsigned permutations=0;
    do {std::vector<viewport::Source> values;for(const auto i:order)values.push_back(grid[size_t(i)]);selected(values,0,1);++permutations;}
    while(std::next_permutation(order.begin(),order.end()));
    selected({source(0,0,true),source(0,1,false),source(1,0,false),source(1,1,false)},0,1);
    selected({source(1,1,true),source(0,1,false),source(1,0,false),source(0,0,false)},0,1);
    selected({source(0,0,false),source(1,1,false)},0,1);
    selected({source(0,0,false),source(0,0,true),source(1,1,true),source(1,1,false)},0,1);
    selected({source(1,0,true),source(0,1,true),source(0,0,false),source(1,1,false)},1,0);
    refused({source(0,0,false),source(1,1,false),source(0,1,false),source(1,0,false)});
    refused({source(0,0,true),source(0,1,true),source(1,1,false)});
    refused({source(0,0,true),source(1,0,true),source(1,1,false)});
    refused({source(0,0,true)});
    refused({source(0,0,true),source(1,1,true),source(2,2,false)});
    auto foreign=source(1,1,false);foreign.path.targetInfo.adapterId.LowPart=8;
    refused({source(0,0,true),foreign});
    foreign=source(1,1,true);foreign.path.sourceInfo.adapterId.LowPart=8;foreign.path.targetInfo.adapterId.LowPart=8;
    refused({source(0,0,true),foreign});
    auto alias=source(1,1,false);alias.name=source(0,0,true).name;
    refused({source(0,0,true),alias});
    alias=source(1,1,false);alias.path.targetInfo.id=100;
    refused({source(0,0,true),alias});
    auto inactiveDuplicate=source(0,0,false);inactiveDuplicate.path.sourceInfo.modeInfoIdx=1234;
    const auto active=viewport::selectSources({inactiveDuplicate,source(0,0,true),source(1,1,true)});
    require(active[0].path.flags&DISPLAYCONFIG_PATH_ACTIVE,"Inactive duplicate displaced the active CCD path");
    std::cout<<"{\"kind\":\"owned-ccd-source-mapping\",\"activePermutationCases\":"<<permutations
        <<",\"additionalAcceptedCases\":6,\"rejectedCases\":9,\"actualDisplayQueries\":0,\"deviceRegistryOrDisplayChanges\":0}\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
