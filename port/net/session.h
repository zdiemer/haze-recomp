#pragma once
#include <cstdint>
#include <atomic>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace haze::net {
enum class State { Offline, Connecting, Waiting, Connected, Failed };
struct Status {
    State state=State::Offline;
    std::string code, error;
    unsigned id=0, peers=0, relayed=0;
    // Selected candidates can identify a relay from only one end of a pair.
    std::array<bool,5> relay_peers{};
};
struct Packet { unsigned peer; std::vector<uint8_t> bytes; };
struct Options {
    std::string endpoint, ca_file;
    bool force_relay=false;
    bool local_test=false; // permits ws://127.0.0.1 only; production requires TLS
};
class Session {
public:
    Session();
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    // Window-thread lifecycle and polling. Code empty creates a room.
    void start(const Options&, const std::string& code={});
    void stop();
    void poll();
    Status status() const;
    // Guest networking thread: peer 0 broadcasts to connected room members.
    bool send(unsigned peer, std::span<const uint8_t> bytes);
    std::vector<Packet> receive();
private:
    struct Impl;
    std::atomic<std::shared_ptr<Impl>> impl_;
};
std::string normalize_code(std::string code);
}
