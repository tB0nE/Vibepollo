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
// The budget of the request being handled on this thread. A function-local thread_local:
// MinGW emits a TLS init function for an inline namespace-scope one in every object file
// and then fails to link them ("multiple definition of TLS init function").
inline budget_t &request() {
 static thread_local budget_t value;
 return value;
}
struct scope {
 budget_t previous;
 explicit scope(budget_t value):previous(request()){request()=value;}
 ~scope(){request()=previous;}
 scope(const scope&)=delete;
 scope& operator=(const scope&)=delete;
};
}
