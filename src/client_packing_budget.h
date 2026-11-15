#pragma once
#include <optional>
#include <map>
#include <string>
#include <utility>
namespace client_packing {
struct budget_t {
 bool active=false, valid=true, strict=false, seen=false, fits=true;
 int axis=8192, physical_w=0, physical_h=0, physical_scale=100;
 long long pixels=35389440;
 std::map<std::string,std::pair<int,int>> physical_modes, requested_sizes;
};
inline thread_local budget_t request;
struct scope {
 budget_t previous;
 explicit scope(budget_t value):previous(request){request=value;}
 ~scope(){request=previous;}
 scope(const scope&)=delete;
 scope& operator=(const scope&)=delete;
};
}
