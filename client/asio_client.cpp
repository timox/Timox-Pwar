// Standalone PWAR ASIO client. No Timox engine or private transport dependency.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "iasiodrv.h"
#include "pwar_packet.h"
#include "pwar_router.h"
#include "frame_ring.h"
#include "control_protocol.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr unsigned maxFrames=4096, wireFrames=128, wireRate=48000;
struct Options { CLSID driver{}; sockaddr_in peer{}; unsigned seconds=0,maxQueueMs=100; bool stdio=false; };
bool number(const char* text,unsigned& result) {
    const auto end=text+std::strlen(text);
    const auto parsed=std::from_chars(text,end,result);
    return parsed.ec==std::errc{} && parsed.ptr==end;
}
bool parse(int argc,char** argv,Options& options) {
    bool driver=false,peer=false,queueLimit=false;
    options.peer.sin_family=AF_INET; options.peer.sin_port=htons(8321);
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"--stdio")==0) {
            if(options.stdio) return false;
            options.stdio=true;continue;
        }
        if(i+1==argc) return false;
        const std::string key=argv[i++]; const char* value=argv[i];
        if(key=="--driver-clsid" && !driver) {
            std::wstring wide(value,value+std::strlen(value));
            if(FAILED(CLSIDFromString(wide.c_str(),&options.driver))) return false;
            driver=true;
        } else if(key=="--peer" && !peer) {
            if(InetPtonA(AF_INET,value,&options.peer.sin_addr)!=1) return false;
            peer=true;
        } else if(key=="--seconds" && !options.seconds) {
            if(!number(value,options.seconds) || !options.seconds || options.seconds>86400) return false;
        } else if(key=="--max-queue-ms" && !queueLimit) {
            if(!number(value,options.maxQueueMs) || !options.maxQueueMs || options.maxQueueMs>2000) return false;
            queueLimit=true;
        } else return false;
    }
    return driver && peer && (options.stdio ? options.seconds==0 : options.seconds!=0);
}
struct Client {
    static_assert(std::atomic<unsigned long long>::is_always_lock_free);
    IASIO* driver=nullptr;
    SOCKET socket=INVALID_SOCKET;
    sockaddr_in peer{};
    CLSID requestedDriver{};
    ASIOBufferInfo buffers[4]{};
    ASIOCallbacks asioCallbacks{};
    long frames=0;
    unsigned maxQueueMs=100;
    bool buffersCreated=false,started=false;
    std::atomic<bool> stop{false},reset{false},failed{false};
    std::atomic<unsigned long long> callbacks{0},txFramesLost{0},rxFramesLost{0},silenced{0},txPackets{0},rxPackets{0},invalid{0};
    std::atomic<unsigned long long> txExpired{0},rxExpired{0};
    std::atomic<unsigned long long> udpResetEvents{0};
    std::atomic<unsigned long long> txQueueOverflowFrames{0},txSendFailures{0},txWouldBlockEvents{0};
    std::atomic<int> lastSendError{0};
    std::atomic<unsigned long long> lastCallbackMs{0},lastRxMs{0},lastSignalMs{0};
    pwar::FrameRing tx,rx;
    std::array<float,maxFrames*2> scratch{};
    std::thread worker;
    static Client* active; // Published before start; cleared after stop/dispose.
    static void process(long index,ASIOBool) {
        auto* self=active;
        if(!self || index<0 || index>1) return;
        const auto frames=static_cast<unsigned>(self->frames);
        bool signal=false;
        for(unsigned f=0;f<frames;++f)
            for(unsigned c=0;c<2;++c) {
                const auto value=static_cast<float*>(self->buffers[c].buffers[index])[f];
                self->scratch[f*2+c]=std::isfinite(value)?value:0;
                signal=signal || (std::isfinite(value) && std::abs(value)>0.000001f);
            }
        const auto now=GetTickCount64();
        if(self->tx.write(self->scratch.data(),frames,now)!=frames) {
            self->txFramesLost+=frames;self->txQueueOverflowFrames+=frames;
        }
        self->rxExpired+=self->rx.discardExpired(now,self->maxQueueMs);
        const auto got=self->rx.read(self->scratch.data(),frames);
        for(unsigned f=0;f<frames;++f)
            for(unsigned c=0;c<2;++c) {
                static_cast<float*>(self->buffers[c+2].buffers[index])[f]=f<got?self->scratch[f*2+c]:0;
                signal=signal || (f<got && std::abs(self->scratch[f*2+c])>0.000001f);
            }
        self->silenced+=frames-got; ++self->callbacks;
        self->lastCallbackMs=now;if(signal) self->lastSignalMs=now;
    }
    static ASIOTime* timed(ASIOTime* time,long index,ASIOBool direct) {process(index,direct);return time;}
    static void rateChanged(ASIOSampleRate) {if(active) active->reset=true;}
    static long message(long selector,long value,void*,double*) {
        if(selector==kAsioSelectorSupported)
            return value==kAsioResetRequest || value==kAsioResyncRequest || value==kAsioEngineVersion || value==kAsioSupportsTimeInfo;
        if(selector==kAsioEngineVersion) return 2;
        if(selector==kAsioSupportsTimeInfo) return 1;
        if(selector==kAsioResetRequest || selector==kAsioResyncRequest) {if(active) active->reset=true;return 1;}
        return 0;
    }
    bool open(const Options& options) {
        peer=options.peer;
        requestedDriver=options.driver;
        maxQueueMs=options.maxQueueMs;
        if(FAILED(CoCreateInstance(options.driver,nullptr,CLSCTX_INPROC_SERVER,options.driver,reinterpret_cast<void**>(&driver))) || !driver) {
            std::fputs("Cannot activate selected ASIO driver\n",stderr); return false;
        }
        if(!driver->init(GetDesktopWindow())) return false;
        double rate=0; long inputs=0,outputs=0,min=0,max=0,granularity=0;
        if(driver->getSampleRate(&rate)!=ASE_OK || rate!=wireRate ||
           driver->getChannels(&inputs,&outputs)!=ASE_OK || inputs<2 || outputs<2 ||
           driver->getBufferSize(&min,&max,&frames,&granularity)!=ASE_OK || frames<1 || frames>maxFrames) {
            std::fputs("Requires 48 kHz, two inputs/outputs and preferred block 1..4096; driver settings unchanged\n",stderr); return false;
        }
        if(maxQueueMs<(static_cast<unsigned>(frames)*1000u+wireRate-1)/wireRate) {
            std::fputs("Local queue age limit must cover at least one ASIO block\n",stderr);return false;
        }
        for(long i=0;i<4;++i) {
            ASIOChannelInfo info{}; info.channel=i%2; info.isInput=i<2?ASIOTrue:ASIOFalse;
            if(driver->getChannelInfo(&info)!=ASE_OK || info.type!=ASIOSTFloat32LSB) {
                std::fputs("Requires float32 little-endian ASIO channels\n",stderr); return false;
            }
            buffers[i].channelNum=info.channel; buffers[i].isInput=info.isInput;
        }
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(socket==INVALID_SOCKET) return false;
        const BOOL exclusive=TRUE;
        const int receiveBytes=1024*1024;
        if(setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)) ||
           setsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receiveBytes),sizeof(receiveBytes))) return false;
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(8321);
        if(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))) {
            std::fputs("Cannot bind PWAR UDP port 8321\n",stderr);return false;
        }
        u_long nonblocking=1;if(ioctlsocket(socket,FIONBIO,&nonblocking)) return false;
        asioCallbacks={process,rateChanged,message,timed};
        active=this;
        if(driver->createBuffers(buffers,4,frames,&asioCallbacks)!=ASE_OK) return false;
        buffersCreated=true;
        for(const auto& buffer:buffers) if(!buffer.buffers[0] || !buffer.buffers[1]) return false;
        for(unsigned c=2;c<4;++c) for(unsigned b=0;b<2;++b) std::memset(buffers[c].buffers[b],0,frames*sizeof(float));
        worker=std::thread([this]{network();});
        if(driver->start()!=ASE_OK) return false;
        started=true;
        return true;
    }
    void network() noexcept {
        pwar_router_t router{};pwar_router_init(&router,2);
        std::array<float,PWAR_ROUTER_MAX_BUFFER_SIZE*2> planar{},stereo{};
        std::uint64_t sequence=0;
        while(!stop.load()) {
            txExpired+=tx.discardExpired(GetTickCount64(),maxQueueMs);
            for(unsigned burst=0;burst<64 && tx.available()>=wireFrames;++burst) {
                if(tx.read(stereo.data(),wireFrames)!=wireFrames) break;
                pwar_packet_t packet{};packet.seq=sequence++;packet.n_samples=wireFrames;packet.num_packets=1;
                packet.timestamp=packet.seq_timestamp=GetTickCount64()*1000000ull;
                for(unsigned f=0;f<wireFrames;++f) for(unsigned c=0;c<2;++c) packet.samples[c][f]=stereo[f*2+c];
                const auto sent=sendto(socket,reinterpret_cast<const char*>(&packet),sizeof(packet),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer));
                if(sent==sizeof(packet)) ++txPackets;
                else {
                    const auto error=sent==SOCKET_ERROR?WSAGetLastError():0;
                    lastSendError=error;++txSendFailures;txFramesLost+=wireFrames;
                    if(error==WSAEWOULDBLOCK) ++txWouldBlockEvents;
                }
            }
            fd_set readable;FD_ZERO(&readable);FD_SET(socket,&readable);timeval timeout{0,1000};
            if(select(0,&readable,nullptr,nullptr,&timeout)==SOCKET_ERROR) {failed=true;break;}
            for(unsigned burst=0;burst<64;++burst) {
                pwar_packet_t packet{};sockaddr_in sender{};int length=sizeof(sender);
                const auto bytes=recvfrom(socket,reinterpret_cast<char*>(&packet),sizeof(packet),0,reinterpret_cast<sockaddr*>(&sender),&length);
                if(bytes==SOCKET_ERROR) {
                    const auto error=WSAGetLastError();
                    if(error==WSAEWOULDBLOCK) break;
                    if(error==WSAEMSGSIZE) {++invalid;continue;}
                    if(error==WSAECONNRESET) {++udpResetEvents;continue;}
                    failed=true;return;
                }
                if(sender.sin_family!=AF_INET || sender.sin_addr.s_addr!=peer.sin_addr.s_addr) {++invalid;continue;}
                if(bytes==sizeof(pwar_latency_info_t)) continue;
                if(bytes!=sizeof(packet)) {++invalid;continue;}
                const auto count=pwar_router_process_packet(&router,&packet,planar.data(),PWAR_ROUTER_MAX_BUFFER_SIZE,2);
                if(count<0) {++invalid;continue;}
                if(!count) continue;
                ++rxPackets;
                lastRxMs=GetTickCount64();
                for(int f=0;f<count;++f) for(unsigned c=0;c<2;++c) {
                    const auto value=planar[c*count+f];stereo[f*2+c]=std::isfinite(value)?value:0;
                }
                if(rx.write(stereo.data(),count,GetTickCount64())!=static_cast<unsigned>(count)) rxFramesLost+=count;
            }
        }
    }
    pwar::Json status() const {
        const auto now=GetTickCount64(),callbackMs=lastCallbackMs.load(),rxMs=lastRxMs.load(),signalMs=lastSignalMs.load();
        const bool clockActive=callbackMs && now>=callbackMs && now-callbackMs<=1000;
        const char* state=failed || reset?"error":stop?"stopped":clockActive?"running":"waiting_clock";
        char address[INET_ADDRSTRLEN]{};
        InetNtopA(AF_INET,const_cast<IN_ADDR*>(&peer.sin_addr),address,sizeof(address));
        wchar_t guid[40]{};StringFromGUID2(requestedDriver,guid,40);
        const std::wstring wideGuid(guid);
        return {{"state",state},{"pid",GetCurrentProcessId()},{"driver_clsid",std::string(wideGuid.begin(),wideGuid.end())},
            {"sample_rate",wireRate},{"channels",2},{"block_frames",frames},
            {"peer_address",address},{"peer_port",ntohs(peer.sin_port)},{"local_port",8321},{"max_queue_ms",maxQueueMs},
            {"callbacks",callbacks.load()},{"tx_packets",txPackets.load()},{"rx_blocks",rxPackets.load()},
            {"tx_dropped_frames",txFramesLost.load()},{"rx_dropped_frames",rxFramesLost.load()},
            {"silenced_frames",silenced.load()},{"invalid_packets",invalid.load()},
            {"udp_reset_events",udpResetEvents.load()},
            {"tx_queue_overflow_frames",txQueueOverflowFrames.load()},{"tx_send_failures",txSendFailures.load()},
            {"tx_would_block_events",txWouldBlockEvents.load()},{"last_send_error",lastSendError.load()},
            {"tx_expired_frames",txExpired.load()},{"rx_expired_frames",rxExpired.load()},
            {"reset_requested",reset.load()},{"network_error",failed.load()},
            {"network_active",!stop && rxMs && now>=rxMs && now-rxMs<=1000},
            {"audio_active",!stop && clockActive && signalMs && now>=signalMs && now-signalMs<=1000},
            {"last_callback_age_ms",callbackMs && now>=callbackMs?pwar::Json(now-callbackMs):pwar::Json(nullptr)},
            {"latency_ms",nullptr},{"drift_ppm",nullptr}};
    }
    void close() noexcept {
        if(started) {driver->stop();started=false;}
        if(buffersCreated) {driver->disposeBuffers();buffersCreated=false;}
        active=nullptr;
        stop=true;if(worker.joinable()) worker.join();
        if(driver) {driver->Release();driver=nullptr;}
        if(socket!=INVALID_SOCKET) {closesocket(socket);socket=INVALID_SOCKET;}
    }
    ~Client() {close();}
};
Client* Client::active=nullptr;
bool emit(const pwar::Json& value) {
    const auto line=value.dump();
    return std::fwrite(line.data(),1,line.size(),stdout)==line.size() && std::fputc('\n',stdout)!=EOF && std::fflush(stdout)==0;
}
bool supervise(Client& client) {
    const auto input=GetStdHandle(STD_INPUT_HANDLE);
    std::string pending;pending.reserve(pwar::maxControlBytes);
    bool oversized=false,requestedStop=false;
    std::string lastState=client.status()["state"];
    while(!client.failed && !client.reset) {
        DWORD available=0;
        if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr))
            return GetLastError()==ERROR_BROKEN_PIPE;
        if(available) {
            char bytes[4096];DWORD read=0;
            if(!ReadFile(input,bytes,std::min<DWORD>(available,sizeof(bytes)),&read,nullptr) || !read)
                return GetLastError()==ERROR_BROKEN_PIPE;
            for(DWORD i=0;i<read;++i) {
                if(bytes[i]=='\n') {
                    if(!oversized) {
                        if(!emit(pwar::controlCommand(pending,client.status(),requestedStop))) return false;
                        if(requestedStop) return true;
                    }
                    pending.clear();oversized=false;
                } else if(!oversized) {
                    if(pending.size()==pwar::maxControlBytes) {
                        if(!emit(pwar::controlError(nullptr,"MESSAGE_TOO_LARGE"))) return false;
                        pending.clear();oversized=true;
                    } else pending+=bytes[i];
                }
            }
        }
        const auto status=client.status();
        const auto state=status["state"].get<std::string>();
        if(state!=lastState) {
            if(!emit({{"version",1},{"event","state_changed"},{"status",status}})) return false;
            lastState=state;
        }
        Sleep(20);
    }
    return false;
}
}
int main(int argc,char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--help")==0) {
        std::puts("PWAR ASIO client (qualification): --driver-clsid GUID --peer IPv4 (--seconds 1..86400 | --stdio) [--max-queue-ms 1..2000]\nSupervised mode requires inherited stdin/stdout pipes; JSON v1 status/stop, one object per line. Uses existing ASIO rate/block, channels 1/2 in both directions. Currently 48 kHz float32 only. UDP 8321. Local queue age limit defaults to 100 ms; not an end-to-end latency guarantee. No routing changes. Drift compensation and automatic restart are not yet qualified.");return 0;
    }
    Options options;
    if(!parse(argc,argv,options)) {std::fputs("Invalid arguments; use --help\n",stderr);return 2;}
    if(options.stdio && (GetFileType(GetStdHandle(STD_INPUT_HANDLE))!=FILE_TYPE_PIPE ||
                        GetFileType(GetStdHandle(STD_OUTPUT_HANDLE))!=FILE_TYPE_PIPE)) {
        std::fputs("--stdio requires inherited stdin and stdout pipes\n",stderr);return 2;
    }
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 1;
    WSADATA ws{};if(WSAStartup(MAKEWORD(2,2),&ws)) {CoUninitialize();return 1;}
    int result=1;
    try {
        auto client=std::make_unique<Client>();
        if(client->open(options)) {
            const bool announced=emit({{"version",1},{"event","asio_start_accepted"},{"status",client->status()}});
            bool controlledStop=false;
            if(announced && options.stdio) controlledStop=supervise(*client);
            else if(announced) {
                const auto deadline=GetTickCount64()+options.seconds*1000ull;
                while(GetTickCount64()<deadline && !client->reset && !client->failed) Sleep(20);
            }
            client->close();
            if(!options.stdio && !client->callbacks) std::fputs("No ASIO audio callbacks observed after start was accepted; verify the host audio clock. No audio transport was qualified.\n",stderr);
            emit({{"version",1},{"event","stopped"},{"status",client->status()}});
            result=announced && (options.stdio?controlledStop:client->callbacks!=0) && !client->reset && !client->failed?0:1;
        } else emit({{"version",1},{"event","error"},{"error",{{"code","START_FAILED"}}}});
    } catch(const std::exception& error) {std::fprintf(stderr,"PWAR client: %s\n",error.what());}
    WSACleanup();CoUninitialize();return result;
}
