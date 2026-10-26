#include "frame_ring.h"
#include <algorithm>
#include <cstdio>
#include <thread>
#include <memory>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    auto storage=std::make_unique<pwar::FrameRing>();
    auto& ring=*storage;
    std::array<float,pwar::FrameRing::capacity*2> input{},output{};
    for(unsigned i=0;i<input.size();++i) input[i]=float(i);
    CHECK(ring.read(output.data(),128)==0);
    CHECK(ring.write(nullptr,128)==0);
    CHECK(ring.write(input.data(),pwar::FrameRing::capacity)==pwar::FrameRing::capacity);
    CHECK(ring.write(input.data(),1)==0);
    CHECK(ring.read(output.data(),pwar::FrameRing::capacity)==pwar::FrameRing::capacity && input==output);
    // Independent callback and worker block sizes, including repeated ring wraps.
    constexpr unsigned total=500000;
    std::atomic<bool> bad{false};
    std::thread producer([&] {
        float data[97*2];
        for(unsigned cursor=0;cursor<total;) {
            const auto count=std::min(97u,total-cursor);
            for(unsigned f=0;f<count;++f) {data[2*f]=float(cursor+f);data[2*f+1]=-data[2*f];}
            if(ring.write(data,count)) cursor+=count; else std::this_thread::yield();
        }
    });
    float data[128*2];
    for(unsigned cursor=0;cursor<total;) {
        auto count=ring.read(data,128);
        for(unsigned f=0;f<count;++f)
            if(data[2*f]!=float(cursor+f) || data[2*f+1]!=-float(cursor+f)) bad=true;
        cursor+=count;
        if(!count) std::this_thread::yield();
    }
    producer.join(); CHECK(!bad && ring.available()==0);
    CHECK(ring.write(input.data(),128,1000)==128);
    CHECK(ring.write(input.data()+256,128,1100)==128);
    CHECK(ring.discardExpired(1100,100)==0); // Equality is still in budget.
    CHECK(ring.discardExpired(1101,100)==128);
    CHECK(ring.read(output.data(),128)==128);
    for(unsigned i=0;i<256;++i) CHECK(output[i]==input[i+256]);
    CHECK(ring.discardExpired(2000,100)==0);
    CHECK(ring.write(input.data(),128,2001)==128);
    CHECK(ring.discardExpired(2000,100)==0); // Producer can publish after the caller sampled time.
    CHECK(ring.read(output.data(),128)==128);
    return 0;
}
