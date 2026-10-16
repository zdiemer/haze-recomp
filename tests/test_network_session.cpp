#include "session.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <array>

using namespace haze::net;
using namespace std::chrono_literals;
void require(bool condition,const char* error) { if(!condition) throw std::runtime_error(error); }
int main(int argc,char** argv) try {
    require(argc>=2,"Usage: test_network_session ws://127.0.0.1:port/v1 [--relay]");
    Options options{argv[1],{},false,true};
    bool expect_failure=false;
    for(int i=2;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--relay") options.force_relay=true;
        else if(arg=="--expect-failure") expect_failure=true;
        else if(arg.starts_with("--ca=")) options.ca_file=arg.substr(5);
        else throw std::runtime_error("Unknown test option");
    }
    std::array<Session,4> sessions;
    if(expect_failure) {
        sessions[0].start(options);
        const auto deadline=std::chrono::steady_clock::now()+25s;
        while(std::chrono::steady_clock::now()<deadline) {
            sessions[0].poll();
            const auto state=sessions[0].status().state;
            require(state!=State::Waiting && state!=State::Connected,"Untrusted TLS connection accepted");
            if(state==State::Failed) { std::cout<<"Untrusted TLS connection rejected.\n"; return 0; }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("TLS rejection timed out");
    }
    const unsigned count=options.force_relay ? 2 : 4;
    auto wait=[&](auto condition,const char* error) {
        const auto deadline=std::chrono::steady_clock::now()+50s;
        do {
            for(auto& s:sessions) s.poll();
            if(condition()) return;
            for(auto& s:sessions) if(s.status().state==State::Failed) throw std::runtime_error(s.status().error);
            std::this_thread::sleep_for(10ms);
        } while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error(error);
    };
    require(normalize_code("abcd-efgh-jk23")=="ABCDEFGHJK23","Code normalization failed");
    require(normalize_code("ABCD!EFGHJK23").empty(),"Invalid character accepted");
    for(const std::string url:{"ws://127.0.0.1.evil.test:8765/v1", "ws://127.0.0.1:8765@evil.test/v1", "ws://example.com/v1"}) {
        Options unsafe=options; unsafe.endpoint=url;
        sessions[0].start(unsafe);
        require(sessions[0].status().state==State::Failed,"Non-loopback plaintext endpoint accepted");
    }
    sessions[0].start(options);
    wait([&] { return !sessions[0].status().code.empty(); },"Create timed out");
    const auto code=sessions[0].status().code;
    for(unsigned i=1;i<count;++i) {
        sessions[i].start(options,code);
        wait([&] { return sessions[i].status().id==i+1; },"Join timed out");
    }
    wait([&] { for(unsigned i=0;i<count;++i) if(sessions[i].status().peers!=count-1) return false; return true; },"Full mesh timed out");
    if(options.force_relay) wait([&] {
        // libjuice may report a peer-reflexive pair at the receiving end of a
        // TURN allocation. Confirm a selected relay for every link, using both
        // ends' candidate reports, rather than requiring both to name it.
        for(unsigned a=0;a<count;++a) for(unsigned b=a+1;b<count;++b)
            if(!sessions[a].status().relay_peers[b+1] && !sessions[b].status().relay_peers[a+1]) return false;
        return true;
    },"Relay test did not confirm all routes use TURN");
    const std::array<uint8_t,7> payload={0,1,0,255,127,0,42};
    std::array<bool,4> received{}; received[0]=true;
    wait([&] {
        sessions[0].send(0,payload);
        for(unsigned i=1;i<count;++i) for(auto& packet:sessions[i].receive()) {
            require(packet.peer==1,"Wrong sender");
            require(std::vector<uint8_t>(payload.begin(),payload.end())==packet.bytes,"Datagram changed");
            received[i]=true;
        }
        for(unsigned i=1;i<count;++i) if(!received[i]) return false;
        return true;
    },"Broadcast failed");
    for(unsigned i=1;i<count;++i) {
        bool echoed=false;
        wait([&] {
            sessions[i].send(1,payload);
            for(auto& packet:sessions[0].receive()) if(packet.peer==i+1) echoed=packet.bytes==std::vector<uint8_t>(payload.begin(),payload.end());
            return echoed;
        },"Client datagram failed");
    }
    sessions[1].stop();
    wait([&] { return sessions[0].status().peers==count-2; },"Departure failed");
    sessions[1].start(options,code);
    wait([&] { for(unsigned i=0;i<count;++i) if(sessions[i].status().peers!=count-1) return false; return true; },"Rejoin failed");
    sessions[0].stop();
    const auto deadline=std::chrono::steady_clock::now()+5s;
    bool closed=false;
    while(std::chrono::steady_clock::now()<deadline) {
        closed=true;
        for(unsigned i=1;i<count;++i) { sessions[i].poll(); closed &= sessions[i].status().state==State::Failed; }
        if(closed) break;
        std::this_thread::sleep_for(10ms);
    }
    require(closed,"Host closure did not end room");
    std::cout << count << "-peer connection, binary datagrams, departure/rejoin and host closure passed.\n";
    return 0;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
