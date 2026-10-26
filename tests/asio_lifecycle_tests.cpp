#include <winsock2.h>
#include <windows.h>
#include "pwarASIO.h"
#include "pwarASIOLog.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#define CHECK(expression) do { if (!(expression)) { std::cerr << "line " << __LINE__ << ": " #expression "\n"; return 1; } } while (false)
std::atomic<bool> holdCallback{false}, callbackEntered{false};
void BufferSwitch(long, ASIOBool) {
    callbackEntered = true;
    // Deliberately unbounded host callback, solely to test the driver's drain.
    while (holdCallback) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void RateChanged(ASIOSampleRate) {}
long Message(long, long, void*, double*) { return 0; }
ASIOTime* TimeInfo(ASIOTime* time, long, ASIOBool) { return time; }
bool PortAvailable() {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return false;
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
    const auto result = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(socket);
    return result == 0;
}
bool WaitForPort(bool available) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do {
        if (PortAvailable() == available) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
bool SendOversizedDatagram() {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char payload[4096]{};
    const auto result = sendto(socket, payload, sizeof(payload), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(socket);
    return result == sizeof(payload);
}
bool SendAudioDatagram() {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pwar_packet_t packet{}; packet.n_samples = 128; packet.num_packets = 1; packet.seq = 100;
    const auto result = sendto(socket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(socket);
    return result == sizeof(packet);
}
bool SendInvalidDatagrams() {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool sent = true;
    const auto sendAndObserve = [&](const char* data, int bytes) {
        if (sendto(socket, data, bytes, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != bytes)
            return false;
        // Avoid bursting eight datagrams into the upstream 1 KB receive queue.
        // This is an observed negative test, not a UDP delivery acknowledgement.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        return !callbackEntered.load();
    };
    for (const auto frames : {0, 1, 63, 96, 129, 65535}) {
        pwar_packet_t packet{}; packet.n_samples = static_cast<uint16_t>(frames);
        packet.num_packets = 1; packet.seq = 10;
        sent = sent && sendAndObserve(reinterpret_cast<const char*>(&packet), sizeof(packet));
    }
    // Struct-sized packets are the existing wire contract; truncated/extended
    // datagrams must not enter the audio callback even if their header is valid.
    char payload[sizeof(pwar_packet_t) + 1]{};
    pwar_packet_t valid{}; valid.n_samples = 128; valid.num_packets = 1; valid.seq = 20;
    std::memcpy(payload, &valid, sizeof(valid));
    for (const int bytes : {static_cast<int>(sizeof(valid) - 1), static_cast<int>(sizeof(valid) + 1)})
        sent = sent && sendAndObserve(payload, bytes);
    closesocket(socket);
    return sent;
}

int main() {
    // Keep upstream diagnostic datagrams on loopback during this test.
    pwarASIOLog logging("127.0.0.1", 1338);
    WSADATA winsock{};
    CHECK(WSAStartup(MAKEWORD(2,2), &winsock) == 0);
    CHECK(PortAvailable()); // Refuse to run if a real PWAR listener owns the port.
    HRESULT result = S_OK;
    const auto begin = std::chrono::steady_clock::now();
    {
        pwarASIO driver(nullptr, &result);
        CHECK(SUCCEEDED(result));
        CHECK(driver.init(nullptr));
        ASIOCallbacks callbacks{BufferSwitch, RateChanged, Message, TimeInfo};
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            ASIOBufferInfo buffers[2]{};
            buffers[0].channelNum = 0; buffers[1].channelNum = 1;
            CHECK(driver.createBuffers(buffers, 2, 128, &callbacks) == ASE_OK);
            for (auto& buffer : buffers)
                for (auto* half : buffer.buffers) std::memset(half, 0, 128 * sizeof(float));
            if (iteration == 0) {
                const auto blocker = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                CHECK(blocker != INVALID_SOCKET);
                const BOOL exclusive = TRUE;
                CHECK(setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
                sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8321);
                CHECK(bind(blocker, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
                const auto startResult = driver.start();
                closesocket(blocker);
                CHECK(startResult == ASE_HWMalfunction);
                char error[124]{}; driver.getErrorMessage(error);
                CHECK(std::strstr(error, "UDP") != nullptr);
                CHECK(PortAvailable());
            }
            CHECK(driver.start() == ASE_OK);
            CHECK(WaitForPort(false)); // This must exercise an actual bound listener.
            callbackEntered = false;
            CHECK(SendInvalidDatagrams());
            CHECK(SendOversizedDatagram());
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            CHECK(!PortAvailable()); // Recoverable datagram errors must not stop it.
            CHECK(!callbackEntered);
            CHECK(driver.start() == ASE_InvalidMode);
            CHECK(driver.setSampleRate(44100) == ASE_InvalidMode);
            CHECK(driver.createBuffers(buffers, 2, 128, &callbacks) == ASE_InvalidMode);
            std::cerr << "stopping iteration " << iteration << '\n';
            if (iteration == 0) {
                CHECK(driver.stop() == ASE_OK);
                CHECK(PortAvailable());
                CHECK(driver.disposeBuffers() == ASE_OK);
            } else if (iteration == 1) {
                holdCallback = true; callbackEntered = false;
                CHECK(SendAudioDatagram());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (!callbackEntered && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!callbackEntered) { holdCallback = false; CHECK(false); }
                std::atomic<bool> disposing{false}, disposed{false};
                ASIOError disposedResult = ASE_HWMalfunction;
                std::thread control([&] { disposing = true; disposedResult = driver.disposeBuffers(); disposed = true; });
                while (!disposing) std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                const bool returnedDuringCallback = disposed.load();
                holdCallback = false;
                control.join();
                CHECK(!returnedDuringCallback);
                CHECK(disposedResult == ASE_OK);
                CHECK(PortAvailable());
            }
            // Final iteration is destroyed while active, without explicit stop.
        }
        std::cerr << "destroying driver\n";
    }
    CHECK(PortAvailable());
    pwarASIOLog::Shutdown();
    WSACleanup();
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(4));
    std::cout << "PWAR real ASIO idle stop, active disposal drain, restart and active destruction passed\n";
}
