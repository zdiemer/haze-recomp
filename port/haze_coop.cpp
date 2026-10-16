// Bridge the retail LAN datagrams to an invite-only WebRTC room. Keeping a real
// loopback socket underneath preserves the SDK's select/poll and timeout ABI.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include "ppu_recomp.h"
#include "haze_coop.h"
#include "haze_coop_settings.h"
#include "../external/ps3recomp/libs/network/sysNet.h"
#include "ps3emu/nid.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>

extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void(*)(ppu_context*));
namespace {
haze::net::Session session;
std::atomic<bool> enabled=false;
std::mutex bridge_mutex;
std::map<int,uint16_t> endpoints;
int active_socket=-1;
struct Endpoint { SOCKET socket=INVALID_SOCKET; uint16_t port=0; };
std::array<Endpoint,5> peers;
constexpr uint32_t virtual_network=0x0a2e0000;
template<class T=void> T* guest(uint64_t address) { return reinterpret_cast<T*>(uintptr_t(uint32_t(address))); }
void result(ppu_context* ctx,int32_t value) { ctx->gpr[3]=uint64_t(int64_t(value)); }
void error(ppu_context* ctx,int code) {
    const auto location=sys_net_errno_loc(); if(location) vm_write32(location,code);
    result(ctx,int32_t(0x80010200u|unsigned(code)));
}
struct Address {
    uint32_t address; uint64_t saved[3];
    Address(ppu_context* ctx,uint32_t source):address(uint32_t(ctx->gpr[1])-64) {
        const uint64_t copy[]={vm_read64(source),vm_read64(source+8)};
        for(unsigned i=0;i<3;++i) saved[i]=vm_read64(address+i*8);
        for(unsigned i=0;i<2;++i) vm_write64(address+i*8,copy[i]);
    }
    ~Address() { for(unsigned i=0;i<3;++i) vm_write64(address+i*8,saved[i]); }
};
bool address_valid(uint32_t address) { return address>=0x10000 && address<=0xfffffff0u; }
bool game_address(uint32_t address) { return address_valid(address) && vm_read16(address+2)==30000; }
void bind_socket(ppu_context* ctx) {
    const auto socket=int32_t(ctx->gpr[3]);
    const auto ptr=uint32_t(ctx->gpr[4]), size=uint32_t(ctx->gpr[5]);
    if(!enabled || !game_address(ptr)) {
        result(ctx,sys_net_bnet_bind(socket,guest<sys_net_sockaddr>(ptr),size)); return;
    }
    Address copy(ctx,ptr);
    vm_write16(copy.address+2,0); vm_write32(copy.address+4,0x7f000001);
    const auto rc=sys_net_bnet_bind(socket,guest<sys_net_sockaddr>(copy.address),size);
    if(!rc) {
        vm_write32(copy.address+16,16);
        if(!sys_net_bnet_getsockname(socket,guest<sys_net_sockaddr>(copy.address),guest<uint32_t>(copy.address+16))) {
            std::lock_guard lock(bridge_mutex);
            endpoints[socket]=vm_read16(copy.address+2); active_socket=socket;
        }
    }
    result(ctx,rc);
}
void send_packet(ppu_context* ctx) {
    const auto socket=int32_t(ctx->gpr[3]);
    const auto ptr=uint32_t(ctx->gpr[7]), size=uint32_t(ctx->gpr[5]);
    bool bridged=false;
    { std::lock_guard lock(bridge_mutex); bridged=endpoints.contains(socket); }
    if(!bridged) {
        result(ctx,sys_net_bnet_sendto(socket,guest(ctx->gpr[4]),size,int32_t(ctx->gpr[6]),
            guest<sys_net_sockaddr>(ptr),uint32_t(ctx->gpr[8]))); return;
    }
    if(!game_address(ptr)) { error(ctx,SYS_NET_ENETUNREACH); return; }
    if(size>65507) { error(ctx,SYS_NET_EMSGSIZE); return; }
    const auto data=uint32_t(ctx->gpr[4]);
    if(!data || uint64_t(data)+size>0x100000000ull) { error(ctx,SYS_NET_EINVAL); return; }
    const auto destination=vm_read32(ptr+4);
    unsigned peer=0;
    if(destination>=virtual_network+1 && destination<=virtual_network+4) peer=destination-virtual_network;
    else if(destination!=0xffffffff && (destination&0xff)!=0xff) { error(ctx,SYS_NET_ENETUNREACH); return; }
    if(enabled) {
        { std::lock_guard lock(bridge_mutex); active_socket=socket; }
        std::vector<uint8_t> payload(size);
        for(uint32_t i=0;i<size;++i) payload[i]=vm_read8(data+i);
        session.send(peer,payload);
    }
    // UDP sends remain successful when a peer is absent or a channel is busy.
    // The game already retries discovery and handles loss and peer timeouts.
    result(ctx,int32_t(size));
}
void receive_packet(ppu_context* ctx) {
    const auto socket=int32_t(ctx->gpr[3]);
    const auto ptr=uint32_t(ctx->gpr[7]);
    const auto rc=sys_net_bnet_recvfrom(socket,guest(ctx->gpr[4]),uint32_t(ctx->gpr[5]),int32_t(ctx->gpr[6]),
        guest<sys_net_sockaddr>(ptr),guest<uint32_t>(ctx->gpr[8]));
    if(rc>=0 && address_valid(ptr)) {
        std::lock_guard lock(bridge_mutex);
        if(endpoints.contains(socket)) {
            unsigned peer=0;
            if(enabled && vm_read32(ptr+4)==0x7f000001) for(unsigned i=1;i<peers.size();++i)
                if(peers[i].port && peers[i].port==vm_read16(ptr+2)) { peer=i; break; }
            if(!peer) { error(ctx,SYS_NET_EWOULDBLOCK); return; }
            vm_write32(ptr+4,virtual_network+peer); vm_write16(ptr+2,30000);
        }
    }
    result(ctx,rc);
}
void close_socket(ppu_context* ctx) {
    const auto socket=int32_t(ctx->gpr[3]);
    { std::lock_guard lock(bridge_mutex); endpoints.erase(socket); if(active_socket==socket) active_socket=-1; }
    result(ctx,sys_net_bnet_close(socket));
}
bool bridge_socket(Endpoint& peer) {
    if(peer.socket!=INVALID_SOCKET) return true;
    peer.socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(peer.socket==INVALID_SOCKET) return false;
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::bind(peer.socket,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==SOCKET_ERROR) {
        closesocket(peer.socket); peer={}; return false;
    }
    int size=sizeof(addr);
    if(::getsockname(peer.socket,reinterpret_cast<sockaddr*>(&addr),&size)==SOCKET_ERROR) {
        closesocket(peer.socket); peer={}; return false;
    }
    u_long nonblocking=1; ioctlsocket(peer.socket,FIONBIO,&nonblocking);
    BOOL reset=FALSE; DWORD returned=0;
    WSAIoctl(peer.socket,SIO_UDP_CONNRESET,&reset,sizeof(reset),nullptr,0,&returned,nullptr,nullptr);
    peer.port=ntohs(addr.sin_port); return true;
}
void start(const std::string& code) {
    haze_coop_leave();
    haze::net::Options options;
    options.endpoint=haze_coop_server();
    if(const auto value=std::getenv("HAZE_COOP_CA_FILE")) options.ca_file=value;
    if(const auto value=std::getenv("HAZE_COOP_LOCAL_TEST")) options.local_test=std::string_view(value)=="1";
    enabled=true;
    session.start(options,code);
}
}
void haze_coop_host() { start({}); }
void haze_coop_join(const std::string& code) { start(code); }
void haze_coop_leave() {
    enabled=false; session.stop();
    std::lock_guard lock(bridge_mutex);
    for(auto& peer:peers) { if(peer.socket!=INVALID_SOCKET) closesocket(peer.socket); peer={}; }
}
haze::net::Status haze_coop_status() { return session.status(); }
void haze_coop_update() {
    session.poll();
    auto packets=session.receive();
    std::lock_guard lock(bridge_mutex);
    const auto endpoint=endpoints.find(active_socket);
    if(!enabled || endpoint==endpoints.end()) return;
    for(const auto& packet:packets) {
        if(packet.peer<1 || packet.peer>=peers.size() || !bridge_socket(peers[packet.peer])) continue;
        sockaddr_in destination{}; destination.sin_family=AF_INET;
        destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK); destination.sin_port=htons(endpoint->second);
        ::sendto(peers[packet.peer].socket,reinterpret_cast<const char*>(packet.bytes.data()),int(packet.bytes.size()),0,
            reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
    }
}
void haze_coop_register() {
    // Native LAN and the invite bridge share the SDK's real socket ABI. Keep
    // an explicit offline override; PSN authentication remains disconnected.
    if(!std::getenv("PS3_NET_ONLINE")) _putenv_s("PS3_NET_ONLINE","1");
    ps3_hle_register_ctx(ps3_compute_nid("bind"),"Haze co-op bind",bind_socket);
    ps3_hle_register_ctx(ps3_compute_nid("sendto"),"Haze co-op sendto",send_packet);
    ps3_hle_register_ctx(ps3_compute_nid("recvfrom"),"Haze co-op recvfrom",receive_packet);
    ps3_hle_register_ctx(ps3_compute_nid("socketclose"),"Haze co-op close",close_socket);
}
