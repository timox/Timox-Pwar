#include <winsock2.h>
#include <windows.h>
HRESULT WINAPI TestActivateDriver(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID*);
int WSAAPI TestSendTo(SOCKET,const char*,int,int,const sockaddr*,int);
// Real client, queues and UDP; simulated COM driver and explicit send failure injection.
#define CoCreateInstance TestActivateDriver
#define sendto TestSendTo
#define main PwarClientCommandForTest
#include "asio_client.cpp"
#undef main
#undef CoCreateInstance
#undef sendto

static std::atomic<int> injectedSendError{0};
int WSAAPI TestSendTo(SOCKET socket,const char* data,int size,int flags,const sockaddr* address,int length) {
    const auto error=injectedSendError.exchange(0);
    if(error) {WSASetLastError(error);return SOCKET_ERROR;}
    return ::sendto(socket,data,size,flags,address,length);
}

namespace {
struct FakeDriver final : IASIO {
    ASIOCallbacks* callbacks=nullptr;
    float samples[4][2][2048]{};
    double rate=48000;
    unsigned stops=0,disposals=0,releases=0;
    bool running=false,failStart=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override {return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {++releases;return 0;}
    ASIOBool init(void*) override {return ASIOTrue;}
    void getDriverName(char* name) override {std::strcpy(name,"PWAR test clock");}
    long getDriverVersion() override {return 1;}
    void getErrorMessage(char* text) override {std::strcpy(text,"Injected failure");}
    ASIOError start() override {running=!failStart;return failStart?ASE_HWMalfunction:ASE_OK;}
    ASIOError stop() override {running=false;++stops;return ASE_OK;}
    ASIOError getChannels(long* in,long* out) override {*in=*out=2;return ASE_OK;}
    ASIOError getLatencies(long* in,long* out) override {*in=*out=2048;return ASE_OK;}
    ASIOError getBufferSize(long* min,long* max,long* preferred,long* granularity) override {
        *min=*max=*preferred=2048;*granularity=0;return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate value) override {return value==rate?ASE_OK:ASE_NoClock;}
    ASIOError getSampleRate(ASIOSampleRate* value) override {*value=rate;return ASE_OK;}
    ASIOError setSampleRate(ASIOSampleRate) override {return ASE_InvalidMode;}
    ASIOError getClockSources(ASIOClockSource*,long* count) override {*count=0;return ASE_OK;}
    ASIOError setClockSource(long) override {return ASE_InvalidMode;}
    ASIOError getSamplePosition(ASIOSamples*,ASIOTimeStamp*) override {return ASE_NotPresent;}
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {info->type=ASIOSTFloat32LSB;return ASE_OK;}
    ASIOError createBuffers(ASIOBufferInfo* buffers,long count,long frames,ASIOCallbacks* cb) override {
        if(count!=4 || frames!=2048) return ASE_InvalidParameter;
        callbacks=cb;
        for(unsigned c=0;c<4;++c) for(unsigned b=0;b<2;++b) buffers[c].buffers[b]=samples[c][b];
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {callbacks=nullptr;++disposals;return ASE_OK;}
    ASIOError controlPanel() override {return ASE_NotPresent;}
    ASIOError future(long,void*) override {return ASE_NotPresent;}
    ASIOError outputReady() override {return ASE_OK;}
    void tick() {
        for(unsigned f=0;f<2048;++f) {samples[0][0][f]=.125f;samples[1][0][f]=-.25f;}
        if(running && callbacks) callbacks->bufferSwitch(0,ASIOFalse);
    }
} fake;
struct Peer {
    SOCKET socket=INVALID_SOCKET;
    ~Peer() {if(socket!=INVALID_SOCKET) closesocket(socket);}
};
template<class Predicate> bool until(Predicate predicate) {
    const auto deadline=GetTickCount64()+2000;
    while(!predicate() && GetTickCount64()<deadline) Sleep(1);
    return predicate();
}
}
HRESULT WINAPI TestActivateDriver(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID* out) {*out=&fake;return S_OK;}
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main() {
    WSADATA ws{};CHECK(WSAStartup(MAKEWORD(2,2),&ws)==0);
    struct Cleanup {~Cleanup(){WSACleanup();}} cleanup;
    Peer peer;peer.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);CHECK(peer.socket!=INVALID_SOCKET);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(18321);
    const DWORD timeout=2000;
    CHECK(setsockopt(peer.socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))==0);
    CHECK(bind(peer.socket,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0);
    Options options;options.peer=address;options.seconds=2;
    auto client=std::make_unique<Client>();CHECK(client->open(options));
    fake.tick();CHECK(client->callbacks==1 && client->silenced==2048);
    for(unsigned p=0;p<16;++p) {
        pwar_packet_t packet{};
        CHECK(recv(peer.socket,reinterpret_cast<char*>(&packet),sizeof(packet),0)==sizeof(packet));
        CHECK(packet.seq==p && packet.num_packets==1 && packet.n_samples==128);
        for(unsigned f=0;f<128;++f) CHECK(packet.samples[0][f]==.125f && packet.samples[1][f]==-.25f);
    }
    sockaddr_in destination=address;destination.sin_port=htons(8321);
    for(unsigned p=0;p<16;++p) {
        pwar_packet_t packet{};packet.seq=p;packet.num_packets=1;packet.n_samples=128;
        for(unsigned f=0;f<128;++f) {packet.samples[0][f]=.5f;packet.samples[1][f]=-.125f;}
        CHECK(sendto(peer.socket,reinterpret_cast<const char*>(&packet),sizeof(packet),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==sizeof(packet));
    }
    CHECK(until([&]{return client->rx.available()==2048;}));
    fake.tick();
    for(unsigned f=0;f<2048;++f) CHECK(fake.samples[2][0][f]==.5f && fake.samples[3][0][f]==-.125f);
    fake.tick();
    for(unsigned f=0;f<2048;++f) CHECK(fake.samples[2][0][f]==0 && fake.samples[3][0][f]==0);
    CHECK(client->silenced==4096);
    // A stalled ASIO consumer must not replay locally queued old audio.
    pwar_packet_t old{};old.seq=16;old.num_packets=1;old.n_samples=128;
    for(unsigned f=0;f<128;++f) old.samples[0][f]=old.samples[1][f]=.75f;
    CHECK(sendto(peer.socket,reinterpret_cast<const char*>(&old),sizeof(old),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==sizeof(old));
    CHECK(until([&]{return client->rx.available()==128;}));
    Sleep(150);
    fake.tick();
    CHECK(client->rxExpired==128);
    injectedSendError=WSAEWOULDBLOCK;fake.tick();
    CHECK(until([&]{return client->txSendFailures==1;}));
    CHECK(client->txWouldBlockEvents==1 && client->lastSendError==WSAEWOULDBLOCK);
    CHECK(client->txQueueOverflowFrames==0 && client->txFramesLost==128);
    for(unsigned f=0;f<2048;++f) CHECK(fake.samples[2][0][f]==0 && fake.samples[3][0][f]==0);
    CHECK(sendto(peer.socket,"bad",3,0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==3);
    CHECK(until([&]{return client->invalid==1;}));
    CHECK(fake.callbacks->asioMessage(kAsioResetRequest,0,nullptr,nullptr)==1 && client->reset);
    client->close();CHECK(fake.stops==1 && fake.disposals==1 && fake.releases==1 && !client->worker.joinable());
    client->close();CHECK(fake.stops==1 && fake.releases==1);
    client.reset();
    fake.rate=44100;
    {auto rejected=std::make_unique<Client>();CHECK(!rejected->open(options));}
    CHECK(fake.releases==2 && fake.disposals==1);
    fake.rate=48000;fake.failStart=true;
    {auto rejected=std::make_unique<Client>();CHECK(!rejected->open(options));}
    CHECK(fake.releases==3 && fake.disposals==2);
    Peer released;released.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    destination.sin_addr.s_addr=htonl(INADDR_ANY);
    CHECK(bind(released.socket,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==0);
    std::puts("Real PWAR client: 2048/128 duplex reblocking, silence, rejection, reset and cleanup passed");
    return 0;
}
