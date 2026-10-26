#include <winsock2.h>
#include <windows.h>
#ifdef PWAR_TEST_DLL
#include "iasiodrv.h"
#include "asio_error.h"
#include "pwar_packet.h"
#include <atomic>
#else
#include "pwarASIO.h"
#include "pwarASIOLog.h"
#endif
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(expression) do { if (!(expression)) { std::cerr << "line " << __LINE__ << ": " #expression "\n"; return 1; } } while (false)
namespace {
#ifdef PWAR_TEST_DLL
struct LoadedDriver {
    HMODULE module = nullptr;
    IASIO* driver = nullptr;
    bool Load(const char* path) {
        module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return false;
        using GetClassObject = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        const auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
        constexpr CLSID clsid{0x188135e1, 0xd565, 0x11d2, {0x85,0x4f,0,0xa0,0xc9,0x9f,0x5d,0x19}};
        IClassFactory* factory = nullptr;
        if (!getClassObject || FAILED(getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) return false;
        const auto result = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&driver));
        factory->Release();
        return SUCCEEDED(result) && driver;
    }
    ~LoadedDriver() {
        if (driver) { driver->stop(); driver->disposeBuffers(); driver->Release(); }
        if (module) FreeLibrary(module);
    }
};
#endif
constexpr long Frames = 128;
std::array<ASIOBufferInfo, 4> buffers{};
long bufferCount = 0;
std::atomic<unsigned> callbacksSeen{0};
std::atomic<bool> mismatch{false};
float expectedReturn[2]{};
constexpr float incoming[2]{0.25f, -0.5f};
constexpr float outgoing[2]{0.75f, -0.125f};

void Switch(long half, ASIOBool) {
    const auto amplitude = static_cast<float>(callbacksSeen.load() + 1);
    for (long slot = 0; slot < bufferCount; ++slot) {
        auto& info = buffers[slot];
        auto* samples = static_cast<float*>(info.buffers[half]);
        for (long frame = 0; frame < Frames; ++frame) {
            if (info.isInput) {
                const auto expected = incoming[info.channelNum] * amplitude;
                if (samples[frame] != expected) mismatch = true;
            } else {
                samples[frame] = outgoing[info.channelNum] * amplitude;
            }
        }
    }
    callbacksSeen.fetch_add(1);
}
void Rate(ASIOSampleRate) {}
long Message(long, long, void*, double*) { return 0; }
ASIOTime* Time(ASIOTime* time, long half, ASIOBool direct) { Switch(half, direct); return time; }
bool PortFree() {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return false;
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
    const auto result = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(socket);
    return result == 0;
}
}

int main(int argc, char** argv) {
#ifdef PWAR_TEST_DLL
    CHECK(argc == 3);
#else
    CHECK(argc == 2);
#endif
    // CTest supplies a private USERPROFILE with the existing PWAR sender set to
    // loopback port 18321. Capture the actual outgoing datagram separately.
#ifndef PWAR_TEST_DLL
    pwarASIOLog logging("127.0.0.1", 1338);
#endif
    WSADATA winsock{};
    CHECK(WSAStartup(MAKEWORD(2,2), &winsock) == 0);
    CHECK(PortFree());
    const auto receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(receiver != INVALID_SOCKET);
    const DWORD timeout = 800;
    CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
    const BOOL exclusive = TRUE;
    CHECK(setsockopt(receiver, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
    sockaddr_in receiveAddress{}; receiveAddress.sin_family = AF_INET; receiveAddress.sin_port = htons(18321);
    receiveAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(receiver, reinterpret_cast<const sockaddr*>(&receiveAddress), sizeof(receiveAddress)) == 0);
    std::vector<long> inputs, outputs;
    const std::string mode = argv[1];
    if (mode == "stereo") { inputs = {0,1}; outputs = {0,1}; }
    else if (mode == "reversed") { inputs = {1,0}; outputs = {1,0}; }
    else if (mode == "reverse_outputs") { inputs = {0}; outputs = {1,0}; }
    else if (mode == "left") { inputs = {0}; outputs = {0}; }
    else if (mode == "right") { inputs = {1}; outputs = {1}; }
    else if (mode == "capture") { inputs = {0,1}; }
    else if (mode == "playback") { outputs = {1}; }
    else if (mode == "clock") { inputs = {0}; }
    else { CHECK(false); }
    for (auto channel : inputs) { auto& info = buffers[bufferCount++]; info.isInput = ASIOTrue; info.channelNum = channel; }
    for (auto channel : outputs) {
        auto& info = buffers[bufferCount++]; info.channelNum = channel;
        expectedReturn[channel] = outgoing[channel];
    }
    HRESULT result = S_OK;
    {
#ifdef PWAR_TEST_DLL
        LoadedDriver loaded;
        CHECK(loaded.Load(argv[2]));
        auto& driver = *loaded.driver;
#else
        pwarASIO driver(nullptr, &result);
#endif
        CHECK(SUCCEEDED(result) && driver.init(nullptr));
        ASIOCallbacks callbacks{Switch, Rate, Message, Time};
        ASIOBufferInfo invalid[3]{};
        for (auto& info : invalid) info.isInput = ASIOTrue;
        CHECK(driver.createBuffers(invalid, 3, Frames, &callbacks) == ASE_InvalidParameter);
        invalid[0].channelNum = 2;
        CHECK(driver.createBuffers(invalid, 1, Frames, &callbacks) == ASE_InvalidParameter);
        CHECK(driver.createBuffers(buffers.data(), bufferCount, Frames, &callbacks) == ASE_OK);
        for (long slot = 0; slot < bufferCount; ++slot)
            for (auto* half : buffers[slot].buffers) std::memset(half, 0, Frames * sizeof(float));
#ifdef PWAR_TEST_DLL
        // Exercise the host's error formatter with the real loaded driver.
        const auto blocker = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(blocker != INVALID_SOCKET);
        CHECK(setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
        sockaddr_in occupied{}; occupied.sin_family = AF_INET; occupied.sin_port = htons(8321);
        CHECK(bind(blocker, reinterpret_cast<const sockaddr*>(&occupied), sizeof(occupied)) == 0);
        const auto rejected = driver.start();
        const auto diagnostic = DescribeAsioFailure(driver, "start", rejected);
        closesocket(blocker);
        CHECK(rejected == ASE_HWMalfunction);
        CHECK(diagnostic.find("ASIO start failed (code ") == 0);
        CHECK(diagnostic.find("UDP listener on port 8321") != std::string::npos);
#endif
        CHECK(driver.start() == ASE_OK);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (PortFree() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!PortFree());
        const auto sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(sender != INVALID_SOCKET);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (unsigned pass = 0; pass < 2; ++pass) {
            const auto amplitude = static_cast<float>(pass + 1);
            pwar_packet_t packet{}; packet.n_samples = Frames; packet.num_packets = 1; packet.seq = 100 + pass;
            for (unsigned channel = 0; channel < 2; ++channel)
                for (long frame = 0; frame < Frames; ++frame) packet.samples[channel][frame] = incoming[channel] * amplitude;
            const auto sent = sendto(sender, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                                    reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            CHECK(sent == sizeof(packet));
            pwar_packet_t response{};
            int received = 0;
            const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            do {
                received = recv(receiver, reinterpret_cast<char*>(&response), sizeof(response), 0);
                // PWAR multiplexes its existing latency report onto this port.
                if (received != sizeof(pwar_latency_info_t)) break;
            } while (std::chrono::steady_clock::now() < responseDeadline);
            CHECK(received == sizeof(response));
            CHECK(callbacksSeen == pass + 1);
            CHECK(!mismatch);
            CHECK(response.n_samples == Frames && response.num_packets == 1 && response.packet_index == 0);
            CHECK(response.seq == packet.seq);
            for (unsigned channel = 0; channel < 2; ++channel)
                for (long frame = 0; frame < Frames; ++frame)
                    CHECK(response.samples[channel][frame] == expectedReturn[channel] * amplitude);
        }
        closesocket(sender);
        CHECK(driver.stop() == ASE_OK);
        CHECK(driver.disposeBuffers() == ASE_OK);
    }
    CHECK(PortFree());
    closesocket(receiver);
#ifndef PWAR_TEST_DLL
    pwarASIOLog::Shutdown();
#endif
    WSACleanup();
    std::cout << "PWAR real UDP channel mapping passed: " << mode << '\n';
}
