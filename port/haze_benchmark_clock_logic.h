#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

// Pure rules of the diagnostic benchmark clock, kept separate for tests.
namespace haze_benchmark {

constexpr uint64_t timebase_hz=79800000u;

// Guest time handed out between boundaries: frozen at the frame's base until
// no boundary has arrived for escape_us, then real time beyond that point.
constexpr uint64_t clock_value(uint64_t base,uint64_t last,uint64_t real_since_boundary,uint64_t escape_us) {
    const uint64_t over=real_since_boundary>escape_us ? real_since_boundary-escape_us : 0;
    return base+over<last ? last : base+over;
}

// A boundary advances exactly one step unless escaped time already passed it.
constexpr uint64_t next_base(uint64_t base,uint64_t last,uint64_t step) {
    return base+step<last ? last : base+step;
}

constexpr uint64_t microseconds_to_timebase(uint64_t us) {
    return us/1000000u*timebase_hz+us%1000000u*timebase_hz/1000000u;
}

// mftb for one thread: the frame's virtual time plus 1 us per read since that
// thread last saw a new frame, never stepping backwards for the thread.
struct ThreadTimebase {
    uint64_t seen=~uint64_t(0), reads=0, last_us=0;
    uint64_t read(uint64_t base_us) {
        if (base_us!=seen) { seen=base_us; reads=0; }
        uint64_t us=base_us+ ++reads;
        if (us<=last_us) us=last_us+1;
        last_us=us;
        return microseconds_to_timebase(us);
    }
};

struct Sweep { unsigned token; double seconds; };
inline Sweep sweep_at(long long gameplay_frame,long long settle,long long sweep,uint64_t step_us) {
    const bool on=gameplay_frame>=settle && gameplay_frame<settle+sweep;
    return {on ? 1u : 0u,on ? double(gameplay_frame-settle)*double(step_us)/1e6 : 0.0};
}

struct Command { uint64_t boundary; char text[64]; };
// "<boundary>:<PAD_FILE command>;..." in nondecreasing boundary order. Stops at
// the first malformed entry and reports whether the whole text was accepted.
inline bool parse_input(const char* text,std::vector<Command>& commands) {
    uint64_t previous=0;
    for (const char* at=text;*at;) {
        Command command{};
        unsigned long long boundary=0;
        int used=0;
        if (std::sscanf(at,"%llu:%63[^;]%n",&boundary,command.text,&used)!=2 || boundary<previous) return false;
        command.boundary=previous=boundary;
        commands.push_back(command);
        at+=used;
        if (*at==';') ++at;
        else if (*at) return false;
    }
    return true;
}

}
