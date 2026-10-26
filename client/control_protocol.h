#pragma once
#include <nlohmann/json.hpp>
#include <string_view>
#include <set>

namespace pwar {
using Json=nlohmann::json;
inline constexpr std::size_t maxControlBytes=16*1024;
inline Json controlError(const Json& id,const char* code) {
    return {{"version",1},{"id",id},{"ok",false},{"error",{{"code",code}}}};
}
inline Json controlCommand(std::string_view line,const Json& status,bool& stopRequested) {
    if(line.size()>maxControlBytes) return controlError(nullptr,"MESSAGE_TOO_LARGE");
    // Reject excessive nesting before allocating a parsed JSON object.
    unsigned depth=0;bool quoted=false,escaped=false;
    for(const char c:line) {
        if(quoted) {
            if(escaped) escaped=false;
            else if(c=='\\') escaped=true;
            else if(c=='"') quoted=false;
        } else if(c=='"') quoted=true;
        else if(c=='{' || c=='[') {if(++depth>8) return controlError(nullptr,"INVALID_REQUEST");}
        else if((c=='}' || c==']') && depth) --depth;
    }
    std::set<std::string> keys;bool duplicate=false;
    const auto request=Json::parse(line.begin(),line.end(),[&](int depth,Json::parse_event_t event,Json& value) {
        if(depth==1 && event==Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second) duplicate=true;
        return true;
    },false);
    if(duplicate || !request.is_object() || request.size()!=3 || !request.contains("id") ||
       !request["id"].is_string() || request["id"].get_ref<const std::string&>().empty() ||
       request["id"].get_ref<const std::string&>().size()>128 || !request.contains("version") ||
       !request["version"].is_number_integer() || !request.contains("command") || !request["command"].is_string())
        return controlError(nullptr,"INVALID_REQUEST");
    const auto& id=request["id"];
    if(request["version"]!=1) return controlError(id,"UNSUPPORTED_VERSION");
    const auto& command=request["command"];
    if(command!="status" && command!="stop") return controlError(id,"UNKNOWN_COMMAND");
    Json response={{"version",1},{"id",id},{"ok",true},{"status",status}};
    if(command=="stop") {stopRequested=true;response["status"]["state"]="stopping";}
    return response;
}
}
