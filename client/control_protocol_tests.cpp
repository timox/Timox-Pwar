#include "control_protocol.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    const pwar::Json status={{"state","waiting_clock"},{"callbacks",0},{"latency_ms",nullptr}};
    bool stop=false;
    auto answer=pwar::controlCommand(R"({"version":1,"id":"a","command":"status"})",status,stop);
    CHECK(answer["ok"]==true && answer["id"]=="a" && answer["status"]==status && !stop);
    answer=pwar::controlCommand(R"({"version":2,"id":"b","command":"stop"})",status,stop);
    CHECK(answer["error"]["code"]=="UNSUPPORTED_VERSION" && !stop);
    for(const auto* invalid:{"{", "null", "[]", R"({"version":1,"id":"","command":"stop"})",
        R"({"version":1.0,"id":"a","command":"stop"})",R"({"version":1,"id":"a","command":"stop","extra":1})",
        R"({"version":1,"id":"a","command":"status","command":"stop"})"}) {
        answer=pwar::controlCommand(invalid,status,stop);CHECK(answer["ok"]==false && !stop);
    }
    answer=pwar::controlCommand(std::string(pwar::maxControlBytes+1,'x'),status,stop);
    CHECK(answer["error"]["code"]=="MESSAGE_TOO_LARGE" && !stop);
    answer=pwar::controlCommand(std::string(200,'['),status,stop);CHECK(answer["ok"]==false && !stop);
    answer=pwar::controlCommand(R"({"version":1,"id":"a","command":"launch-shell"})",status,stop);
    CHECK(answer["error"]["code"]=="UNKNOWN_COMMAND" && !stop);
    answer=pwar::controlCommand(R"({"version":1,"id":"stop-1","command":"stop"})",status,stop);
    CHECK(answer["ok"]==true && stop && answer["status"]["state"]=="stopping");
    CHECK(status["state"]=="waiting_clock");
    return 0;
}
