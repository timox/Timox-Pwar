#include <winsock2.h>
#include <windows.h>
#include "asiolist.h"
#include "iasiodrv.h"
#include "physical_asio_host.h"
#include "pwar_packet.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <cmath>

// Only the registry/activation adapter is replaced. PhysicalAsioHost and its
// sample conversions below are the production sources, and IASIO is the DLL.
namespace {
HMODULE module = nullptr;
constexpr CLSID DriverId{0x188135e1,0xd565,0x11d2,{0x85,0x4f,0,0xa0,0xc9,0x9f,0x5d,0x19}};
constexpr wchar_t DriverText[] = L"{188135E1-D565-11D2-854F-00A0C99F5D19}";
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    bool Bind(unsigned short port) {
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const BOOL exclusive = TRUE;
        const DWORD timeout = 1000;
        if (value == INVALID_SOCKET || setsockopt(value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
            setsockopt(value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) return false;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
        // Match PWAR's wildcard bind when reserving its receive port. A
        // loopback-only reservation need not exclude a later wildcard bind.
        address.sin_addr.s_addr = htonl(port == 8321 ? INADDR_ANY : INADDR_LOOPBACK);
        return bind(value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
};
struct Environment {
    bool winsock = false;
    ~Environment() { if (module) FreeLibrary(module); if (winsock) WSACleanup(); }
};
struct CallbackState {
    bool selected[2]{true,true};
    std::atomic<unsigned> calls{0};
    std::atomic<bool> mismatch{false};
    bool live = false;
    float stimulusGain = 1.0f;
    std::atomic<unsigned> signalBlocks{0};
    std::atomic<bool> holdFirst{false}, enteredFirst{false};
};
void Process(const float* const* inputs, float* const* outputs, std::uint32_t frames, void* context) noexcept {
    auto& state = *static_cast<CallbackState*>(context);
    if(state.holdFirst.load() && state.calls.load()==0) {
        state.enteredFirst=true;
        const auto deadline=GetTickCount64()+1000;
        while(state.holdFirst.load() && GetTickCount64()<deadline) Sleep(1);
    }
    if (state.live) {
        bool signal = false;
        if (frames != 128 || !inputs[0] || !outputs[0] || !outputs[1]) { state.mismatch=true; return; }
        for (unsigned f=0; f<frames; ++f) {
            const float sample=inputs[0][f];
            if (!std::isfinite(sample) || std::abs(sample)>1.0f) { state.mismatch=true; outputs[0][f]=outputs[1][f]=0; continue; }
            if (std::abs(sample)>0.001f) signal=true;
            outputs[0][f]=sample*0.5f;
            outputs[1][f]=-sample*0.5f;
        }
        if(signal) ++state.signalBlocks;
        ++state.calls;
        return;
    }
    const float scale = static_cast<float>(state.calls.load() + 1) * state.stimulusGain;
    if (frames != 128) { state.mismatch = true; return; }
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (!!inputs[channel] != state.selected[channel] || !!outputs[channel] != state.selected[channel]) {
            state.mismatch = true; continue;
        }
        if (!state.selected[channel]) continue;
        for (unsigned frame = 0; frame < frames; ++frame) {
            const float expected = (channel ? -0.25f : 0.125f) * scale;
            if (inputs[channel][frame] != expected) state.mismatch = true;
            outputs[channel][frame] = inputs[channel][frame] * 2;
        }
    }
    ++state.calls;
}
}

AsioDriverList::AsioDriverList() : lpdrvlist(new ASIODRVSTRUCT{}), numdrv(1) {}
AsioDriverList::~AsioDriverList() { asioCloseDriver(0); delete lpdrvlist; }
LONG AsioDriverList::asioGetNumDev() { return numdrv; }
LONG AsioDriverList::asioGetDriverName(int index, char* name, int size) {
    if (index != 0 || size < 10) return DRVERR_INVALID_PARAM;
    std::memcpy(name, "PWAR ASIO", 10); return 0;
}
LONG AsioDriverList::asioGetDriverCLSID(int index, CLSID* id) {
    if (index != 0 || !id) return DRVERR_INVALID_PARAM;
    *id = DriverId; return 0;
}
LONG AsioDriverList::asioOpenDriver(int index, void** instance) {
    if (index != 0 || !instance || lpdrvlist->asiodrv) return DRVERR_INVALID_PARAM;
    using Factory = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    const auto factoryFn = reinterpret_cast<Factory>(GetProcAddress(module, "DllGetClassObject"));
    IClassFactory* factory = nullptr;
    if (!factoryFn || FAILED(factoryFn(DriverId, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) return DRVERR;
    const auto result = factory->CreateInstance(nullptr, DriverId, &lpdrvlist->asiodrv);
    factory->Release();
    *instance = lpdrvlist->asiodrv;
    return SUCCEEDED(result) ? 0 : DRVERR;
}
LONG AsioDriverList::asioCloseDriver(int index) {
    if (index != 0) return DRVERR_INVALID_PARAM;
    if (lpdrvlist->asiodrv) { static_cast<IASIO*>(lpdrvlist->asiodrv)->Release(); lpdrvlist->asiodrv = nullptr; }
    return 0;
}

#define CHECK(condition) do { if (!(condition)) { std::cerr << "Line " << __LINE__ << ": " #condition << " " << error << '\n'; return 1; } } while (false)
int main(int argc, char** argv) {
    std::string error;
    CHECK(argc == 3);
    Environment environment;
    WSADATA data{}; CHECK(WSAStartup(MAKEWORD(2,2), &data) == 0); environment.winsock = true;
    module = LoadLibraryExA(argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    CHECK(module);
    Socket receiver; CHECK(receiver.Bind(18321));
    CallbackState state;
    const std::string mode = argv[1];
    if (mode == "burst") state.stimulusGain = 1.0f / 16.0f;
    std::vector<std::uint32_t> channels{0,1};
    if (mode == "right") { channels = {1}; state.selected[0] = false; }
    else if (mode == "clock") { channels.clear(); state.selected[0] = state.selected[1] = false; }
    else CHECK(mode == "stereo" || mode == "sequence64" || mode == "live" || mode == "burst");
    state.live = mode == "live";
    state.holdFirst = mode == "burst";
    PhysicalAsioHost host;
    CHECK(host.enumerate().size() == 1);
    PhysicalAsioCapabilities capabilities;
    CHECK(host.open(DriverText, nullptr, capabilities, error));
    CHECK(capabilities.inputChannels == 2 && capabilities.outputChannels == 2);
    CHECK(host.setSampleRate(48000, error));
    {
        Socket occupied; CHECK(occupied.Bind(8321));
        CHECK(!host.start(128, channels, channels, Process, &state, error));
        CHECK(error.find("UDP listener on port 8321") != std::string::npos);
    }
    CHECK(host.start(128, channels, channels, Process, &state, error));
    if (state.live) {
        std::cout << "LIVE_READY: 60 seconds, mono input returned as L=0.5*x R=-0.5*x\n" << std::flush;
        for(unsigned second=0;second<60;++second) {
            Sleep(1000);
            if(second%5==4) std::cout << "callbacks=" << state.calls << " signal_blocks=" << state.signalBlocks << " invalid=" << state.mismatch << '\n' << std::flush;
        }
        host.stop(); host.close();
        CHECK(state.calls>=100 && state.signalBlocks>=100 && !state.mismatch);
        return 0;
    }
    sockaddr_in destination{}; destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK); destination.sin_port = htons(8321);
    const unsigned passes=mode=="burst"?16:2;
    if(mode=="burst") {
        for(unsigned pass=0;pass<passes;++pass) {
            pwar_packet_t packet{}; packet.n_samples=128; packet.num_packets=1; packet.seq=100+pass;
            for(unsigned c=0;c<2;++c)
                for(unsigned f=0;f<128;++f) packet.samples[c][f]=(c?-.25f:.125f)*(pass+1)*state.stimulusGain;
            CHECK(sendto(receiver.value,reinterpret_cast<const char*>(&packet),sizeof(packet),0,
                reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==sizeof(packet));
            if(pass==0) {
                const auto deadline=GetTickCount64()+500;
                while(!state.enteredFirst.load() && GetTickCount64()<deadline) Sleep(1);
                CHECK(state.enteredFirst.load());
            }
        }
        state.holdFirst=false;
    }
    for (unsigned pass = 0; pass < passes; ++pass) {
        pwar_packet_t packet{}; packet.n_samples = 128; packet.num_packets = 1; packet.seq = 100 + pass;
        if (mode == "sequence64") packet.seq = uint64_t{UINT32_MAX} + pass;
        for (unsigned channel = 0; channel < 2; ++channel)
            for (unsigned frame = 0; frame < 128; ++frame)
                packet.samples[channel][frame] = (channel ? -0.25f : 0.125f) * (pass + 1) * state.stimulusGain;
        if(mode!="burst") CHECK(sendto(receiver.value, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == sizeof(packet));
        pwar_packet_t response{};
        int bytes = 0;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            bytes = recv(receiver.value, reinterpret_cast<char*>(&response), sizeof(response), 0);
            if (bytes != sizeof(pwar_latency_info_t)) break;
        }
        CHECK(bytes == sizeof(response) && response.seq == packet.seq && response.n_samples == 128);
        CHECK(state.calls >= pass + 1 && !state.mismatch);
        for (unsigned channel = 0; channel < 2; ++channel)
            for (unsigned frame = 0; frame < 128; ++frame)
                CHECK(response.samples[channel][frame] == (state.selected[channel] ? packet.samples[channel][frame] * 2 : 0));
    }
    CHECK(host.consumeTimingCounters().callbacks == passes);
    host.stop();
    host.close();
    Socket released; CHECK(released.Bind(8321));
    std::cout << "Timox production ASIO host + real PWAR DLL passed: " << mode << '\n';
}
