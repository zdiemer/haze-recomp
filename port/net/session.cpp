#include "session.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#endif

namespace haze::net {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
constexpr size_t max_packet=65507, max_queue=256;
constexpr auto channel_name="haze-udp-v1";
namespace {
std::string trust_roots() {
    std::string pem;
#ifdef _WIN32
    const auto store=CertOpenSystemStoreW(0,L"ROOT");
    if(!store) throw std::runtime_error("Cannot open the Windows certificate store.");
    PCCERT_CONTEXT certificate=nullptr;
    while((certificate=CertEnumCertificatesInStore(store,certificate))) {
        DWORD size=0;
        if(!CryptBinaryToStringA(certificate->pbCertEncoded,certificate->cbCertEncoded,CRYPT_STRING_BASE64HEADER,nullptr,&size)) continue;
        std::string text(size,'\0');
        if(CryptBinaryToStringA(certificate->pbCertEncoded,certificate->cbCertEncoded,CRYPT_STRING_BASE64HEADER,text.data(),&size)) {
            text.resize(size); pem+=text;
        }
    }
    CertCloseStore(store,0);
#endif
    return pem;
}
std::string error_text(const std::string& code) {
    if(code=="invalid_code" || code=="not_found") return "That invite code is invalid or has expired.";
    if(code=="full") return "That co-op room is full.";
    if(code=="version") return "The co-op service requires a compatible game build.";
    if(code=="rate_limit" || code=="busy") return "The co-op service is busy. Try again shortly.";
    return "The co-op connection could not be established.";
}
}
struct Session::Impl : std::enable_shared_from_this<Impl> {
    struct Peer {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc;
        Clock::time_point created=Clock::now();
    };
    mutable std::mutex mutex;
    Status view;
    std::deque<std::function<void(Impl&)>> events;
    std::vector<Packet> packets;
    std::map<unsigned,std::shared_ptr<rtc::DataChannel>> channels;
    std::map<unsigned,Peer> peers; // owner thread only
    std::shared_ptr<rtc::WebSocket> ws;
    rtc::Configuration config;
    bool active=true, overflow=false;
    Clock::time_point started=Clock::now();
    void enqueue(std::function<void(Impl&)> event) {
        std::lock_guard lock(mutex);
        if(!active) return;
        if(events.size()>=max_queue) { overflow=true; return; }
        events.push_back(std::move(event));
    }
    void fail(std::string reason) {
        { std::lock_guard lock(mutex); view.state=State::Failed; view.error=std::move(reason); }
        close();
    }
    void close() {
        { std::lock_guard lock(mutex); active=false; events.clear(); packets.clear(); channels.clear(); view.peers=0; }
        if(ws) { ws->resetCallbacks(); ws->close(); ws.reset(); }
        for(auto& [id,p]:peers) {
            if(p.dc) p.dc->resetCallbacks();
            p.pc->resetCallbacks(); p.pc->close();
        }
        peers.clear();
    }
    void signal(Json message) {
        if(ws && ws->isOpen()) ws->send(message.dump());
    }
    void attach(unsigned id,std::shared_ptr<rtc::DataChannel> dc) {
        const auto found=peers.find(id);
        if(found==peers.end() || found->second.dc || dc->label()!=channel_name || dc->protocol()!="haze/1" ||
           !dc->reliability().unordered || dc->reliability().maxRetransmits!=0) {
            dc->close(); return;
        }
        found->second.dc=dc;
        auto weak=weak_from_this();
        std::weak_ptr<rtc::DataChannel> expected=dc;
        dc->onMessage([weak,id,expected](rtc::message_variant message) {
            auto self=weak.lock(); if(!self) return;
            auto binary=std::get_if<rtc::binary>(&message);
            if(!binary || binary->empty() || binary->size()>max_packet) return;
            std::lock_guard lock(self->mutex);
            const auto current=self->channels.find(id);
            if(!self->active || current==self->channels.end() || current->second!=expected.lock() || self->packets.size()>=max_queue) return;
            Packet packet{id,{}};
            packet.bytes.resize(binary->size());
            std::memcpy(packet.bytes.data(),binary->data(),binary->size());
            self->packets.push_back(std::move(packet));
        });
        dc->onOpen([weak,id,expected] { if(auto self=weak.lock()) self->enqueue([id,expected](Impl& s) {
            const auto p=s.peers.find(id); if(p!=s.peers.end() && p->second.dc==expected.lock()) s.opened(id);
        }); });
        dc->onClosed([weak,id,expected] { if(auto self=weak.lock()) self->enqueue([id,expected](Impl& s) {
            const auto p=s.peers.find(id); if(p!=s.peers.end() && p->second.dc==expected.lock()) s.remove(id);
        }); });
        if(dc->isOpen()) opened(id);
    }
    void opened(unsigned id) {
        auto found=peers.find(id); if(found==peers.end() || !found->second.dc) return;
        std::lock_guard lock(mutex); channels[id]=found->second.dc;
        view.peers=unsigned(channels.size()); view.state=State::Connected;
    }
    void remove(unsigned id) {
        auto found=peers.find(id);
        if(found!=peers.end()) {
            if(found->second.dc) found->second.dc->resetCallbacks();
            found->second.pc->resetCallbacks(); found->second.pc->close(); peers.erase(found);
        }
        std::lock_guard lock(mutex); channels.erase(id);
        std::erase_if(packets,[id](const Packet& p) { return p.peer==id; });
        view.peers=unsigned(channels.size());
        if(view.state!=State::Failed) view.state=channels.empty() ? State::Waiting : State::Connected;
    }
    void add(unsigned id) {
        if(id<1 || id>4 || id==view.id || peers.contains(id)) return;
        auto pc=std::make_shared<rtc::PeerConnection>(config);
        peers.emplace(id,Peer{pc,{}});
        auto weak=weak_from_this();
        std::weak_ptr<rtc::PeerConnection> expected=pc;
        pc->onLocalDescription([weak,id,expected](rtc::Description d) {
            if(auto self=weak.lock()) self->enqueue([id,expected,d=std::move(d)](Impl& s) {
                const auto p=s.peers.find(id); if(p==s.peers.end() || p->second.pc!=expected.lock()) return;
                s.signal({{"op","signal"},{"to",id},{"description",{{"type",d.typeString()},{"sdp",std::string(d)}}}});
            });
        });
        pc->onLocalCandidate([weak,id,expected](rtc::Candidate c) {
            if(auto self=weak.lock()) self->enqueue([id,expected,c=std::move(c)](Impl& s) {
                const auto p=s.peers.find(id); if(p==s.peers.end() || p->second.pc!=expected.lock()) return;
                s.signal({{"op","signal"},{"to",id},{"candidate",{{"candidate",std::string(c)},{"mid",c.mid()}}}});
            });
        });
        pc->onDataChannel([weak,id,expected](std::shared_ptr<rtc::DataChannel> dc) {
            if(auto self=weak.lock()) self->enqueue([id,expected,dc](Impl& s) {
                const auto p=s.peers.find(id); if(p!=s.peers.end() && p->second.pc==expected.lock()) s.attach(id,dc);
            });
        });
        pc->onStateChange([weak,id,expected](rtc::PeerConnection::State state) {
            if(state==rtc::PeerConnection::State::Failed) if(auto self=weak.lock()) self->enqueue([id,expected](Impl& s) {
                const auto p=s.peers.find(id); if(p==s.peers.end() || p->second.pc!=expected.lock()) return;
                if(s.view.id!=1 && id==1) s.fail("Could not reach the host. Check the connection and try the invite again.");
                else s.remove(id);
            });
        });
        if(view.id<id) {
            rtc::DataChannelInit init;
            init.protocol="haze/1"; init.reliability.unordered=true; init.reliability.maxRetransmits=0;
            attach(id,pc->createDataChannel(channel_name,init));
        }
    }
    void message(const std::string& wire) {
        const auto j=Json::parse(wire);
        const auto op=j.at("op").get<std::string>();
        if(op=="joined") {
            if(view.id || j.at("v")!=1) throw std::runtime_error("Invalid room response");
            const auto id=j.at("id").get<unsigned>();
            const auto code=normalize_code(j.at("code").get<std::string>());
            if(id<1 || id>4 || code.empty() || j.at("peers").size()>3 || j.at("ice").size()>8) throw std::runtime_error("Invalid room response");
            for(const auto& server:j.at("ice")) {
                rtc::IceServer ice(server.at("urls").get<std::string>());
                if(server.contains("username")) ice.username=server.at("username").get<std::string>();
                if(server.contains("credential")) ice.password=server.at("credential").get<std::string>();
                config.iceServers.push_back(std::move(ice));
            }
            { std::lock_guard lock(mutex); view.id=id; view.code=code; view.state=State::Waiting; }
            for(const auto& peer:j.at("peers")) add(peer.get<unsigned>());
        } else if(op=="peer") add(j.at("id").get<unsigned>());
        else if(op=="left") remove(j.at("id").get<unsigned>());
        else if(op=="closed") fail("The host closed the co-op session.");
        else if(op=="error") {
            const auto code=j.at("code").get<std::string>();
            if(code!="peer_gone") fail(error_text(code));
        } else if(op=="signal") {
            const auto id=j.at("from").get<unsigned>();
            const auto found=peers.find(id); if(found==peers.end()) return;
            if(j.contains("description")) {
                const auto& d=j.at("description");
                found->second.pc->setRemoteDescription(rtc::Description(d.at("sdp").get<std::string>(),d.at("type").get<std::string>()));
            } else {
                const auto& c=j.at("candidate");
                found->second.pc->addRemoteCandidate(rtc::Candidate(c.at("candidate").get<std::string>(),c.at("mid").get<std::string>()));
            }
        }
    }
};
Session::Session():impl_(std::make_shared<Impl>()) {}
Session::~Session() { stop(); }
void Session::stop() { auto s=impl_.load(); s->close(); { std::lock_guard lock(s->mutex); s->view={}; } }
void Session::start(const Options& options,const std::string& invite) {
    stop(); auto current=std::make_shared<Impl>(); impl_.store(current); auto& s=*current;
    const auto code=normalize_code(invite);
    if(options.endpoint.empty()) {
        s.fail("Enter a server address on the Host Co-op or Join Co-op screen."); return;
    }
    const bool loopback=std::regex_match(options.endpoint,std::regex(R"(ws://127\.0\.0\.1:[0-9]{1,5}/[A-Za-z0-9/_-]*)"));
    if(!options.endpoint.starts_with("wss://") && !(options.local_test && loopback)) {
        s.fail("A secure co-op service address is required."); return;
    }
    if(!invite.empty() && code.empty()) { s.fail(error_text("invalid_code")); return; }
    try {
        { std::lock_guard lock(s.mutex); s.view.state=State::Connecting; }
        s.config.maxMessageSize=max_packet;
        if(options.force_relay) s.config.iceTransportPolicy=rtc::TransportPolicy::Relay;
        rtc::WebSocketConfiguration config;
        config.maxMessageSize=20000;
        config.connectionTimeout=std::chrono::seconds(15);
        config.pingInterval=std::chrono::seconds(20);
        config.maxOutstandingPings=2;
        if(!options.ca_file.empty()) config.caCertificatePemFile=options.ca_file;
        else if(!loopback) {
            const auto roots=trust_roots();
            if(roots.empty()) throw std::runtime_error("No trusted certificates available");
            config.caCertificatePemFile=roots;
        }
        s.ws=std::make_shared<rtc::WebSocket>(config);
        std::weak_ptr<Impl> weak=current;
        s.ws->onOpen([weak,code] { if(auto self=weak.lock()) self->enqueue([code](Impl& v) {
            Json request={{"v",1},{"game","haze-blus30094-v1"},{"op",code.empty() ? "create" : "join"}};
            if(!code.empty()) request["code"]=code;
            v.signal(std::move(request));
        }); });
        s.ws->onMessage([weak](rtc::message_variant message) {
            if(auto text=std::get_if<std::string>(&message)) if(auto self=weak.lock()) self->enqueue([wire=*text](Impl& v) { v.message(wire); });
        });
        s.ws->onError([weak](std::string) { if(auto self=weak.lock()) self->enqueue([](Impl& v) { v.fail("Unable to connect to the co-op service."); }); });
        s.ws->onClosed([weak] { if(auto self=weak.lock()) self->enqueue([](Impl& v) { v.fail("The co-op service connection was lost."); }); });
        s.ws->open(options.endpoint);
    } catch(const std::exception&) { s.fail("Unable to initialize the co-op connection."); }
}
void Session::poll() {
    auto current=impl_.load(); auto& s=*current;
    std::deque<std::function<void(Impl&)>> events;
    bool overflow;
    { std::lock_guard lock(s.mutex); events.swap(s.events); overflow=s.overflow; }
    if(overflow) { s.fail("Too many co-op signaling messages."); return; }
    try {
        for(auto& event:events) { if(!s.active) break; event(s); }
        if(s.view.state==State::Connecting && Clock::now()-s.started>std::chrono::seconds(20)) s.fail("The co-op service did not respond.");
        std::vector<unsigned> expired;
        for(auto& [id,peer]:s.peers) if((!peer.dc || !peer.dc->isOpen()) && Clock::now()-peer.created>std::chrono::seconds(45)) expired.push_back(id);
        for(auto id:expired) {
            if(id==1) { s.fail("Could not reach the host. Try the invite again."); break; }
            s.remove(id);
        }
        unsigned relayed=0;
        std::array<bool,5> relay_peers{};
        for(auto& [id,peer]:s.peers) {
            rtc::Candidate local,remote;
            if(peer.pc->getSelectedCandidatePair(&local,&remote) && (local.type()==rtc::Candidate::Type::Relayed || remote.type()==rtc::Candidate::Type::Relayed)) {
                ++relayed; relay_peers[id]=true;
            }
        }
        { std::lock_guard lock(s.mutex); s.view.relayed=relayed; s.view.relay_peers=relay_peers; }
    } catch(const std::exception&) { s.fail("The co-op service sent an incompatible response."); }
}
Status Session::status() const { auto s=impl_.load(); std::lock_guard lock(s->mutex); return s->view; }
bool Session::send(unsigned peer,std::span<const uint8_t> bytes) {
    if(bytes.empty() || bytes.size()>max_packet) return false;
    std::vector<std::shared_ptr<rtc::DataChannel>> targets;
    auto s=impl_.load();
    { std::lock_guard lock(s->mutex); for(const auto& [id,dc]:s->channels) if(!peer || peer==id) targets.push_back(dc); }
    bool sent=false;
    for(const auto& dc:targets) try {
        if(dc->isOpen() && dc->bufferedAmount()<256*1024) { dc->send(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()); sent=true; }
    } catch(const std::exception&) {}
    return sent;
}
std::vector<Packet> Session::receive() {
    auto s=impl_.load(); std::vector<Packet> out; std::lock_guard lock(s->mutex); out.swap(s->packets); return out;
}
}
