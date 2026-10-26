extern "C" {
#include "pwar_rcv_buffer.h"
}
#include <array>
#include <iostream>
#include <string>

#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " #x "\n"; return 1; } } while(false)
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode=argv[1];
    std::array<float,512> source{}, out{};
    for(unsigned c=0;c<2;++c)
        for(unsigned f=0;f<256;++f) source[c*256+f]=float(1000*c+f+1);
    if(mode=="invalid") {
        CHECK(pwar_rcv_buffer_add_buffer(source.data(),0,2)<0);
        CHECK(pwar_rcv_buffer_add_buffer(source.data(),256,0)<0);
        CHECK(pwar_rcv_buffer_add_buffer(nullptr,256,2)<0);
        CHECK(pwar_rcv_get_chunk(nullptr,2,64)<0);
        CHECK(pwar_rcv_get_chunk(out.data(),2,0)<0);
        CHECK(pwar_rcv_get_chunk(out.data(),17,64)<0);
        CHECK(pwar_rcv_get_chunk(out.data(),2,4097)<0);
        return 0;
    }
    CHECK(pwar_rcv_buffer_add_buffer(source.data(),256,2)==0);
    // Existing ping-pong contract primes by returning one silent chunk.
    CHECK(pwar_rcv_get_chunk(out.data(),2,64)==0);
    const unsigned first=mode=="shrink" ? 128 : 64;
    CHECK(pwar_rcv_get_chunk(out.data(),2,first)==1);
    for(unsigned c=0;c<2;++c)
        for(unsigned f=0;f<first;++f) CHECK(out[c*first+f]==source[c*256+f]);
    const unsigned next=mode=="grow" ? 96 : mode=="shrink" ? 64 : 256;
    out.fill(-1);
    CHECK(pwar_rcv_get_chunk(out.data(),2,next)==1);
    for(unsigned c=0;c<2;++c)
        for(unsigned f=0;f<next;++f)
            CHECK(out[c*next+f]==(first+f<256 ? source[c*256+first+f] : 0.0f));
    return 0;
}
