#include "../port/haze_benchmark_clock_logic.h"
#include <cassert>
#include <cstring>
#include <vector>

using namespace haze_benchmark;

int main() {
    // Frozen within a frame until the escape interval, then real time beyond it.
    assert(clock_value(1000,1000,0,100000)==1000);
    assert(clock_value(1000,1000,99999,100000)==1000);
    assert(clock_value(1000,1000,100250,100000)==1250);
    assert(clock_value(1000,5000,0,100000)==5000); // never hands out less than before
    // Each boundary advances exactly one step, unless escaped time passed it.
    assert(next_base(1000,1000,6944)==7944);
    assert(next_base(1000,9000,6944)==9000);

    assert(microseconds_to_timebase(1000000)==timebase_hz);
    assert(microseconds_to_timebase(10)==798);
    // The split multiply stays exact beyond the range of a naive us*79.8e6.
    assert(microseconds_to_timebase(300000000000ull)==300000ull*timebase_hz);

    // 1 us per read within a frame, restarting at the next frame's base...
    ThreadTimebase timebase;
    assert(timebase.read(100)==microseconds_to_timebase(101));
    assert(timebase.read(100)==microseconds_to_timebase(102));
    assert(timebase.read(200)==microseconds_to_timebase(201));
    // ...but a thread that read past the next base never steps backwards.
    for (int i=0;i<200;++i) timebase.read(200);
    const uint64_t before=timebase.last_us;
    assert(before==401);
    assert(timebase.read(300)==microseconds_to_timebase(before+1));

    auto off=sweep_at(99,100,10,6944), first=sweep_at(100,100,10,6944);
    auto later=sweep_at(105,100,10,6944), done=sweep_at(110,100,10,6944);
    assert(off.token==0 && first.token==1 && first.seconds==0.0);
    assert(later.token==1 && later.seconds>0.0347 && later.seconds<0.0348);
    assert(done.token==0);

    std::vector<Command> commands;
    assert(parse_input("10:0x8 1000;250:0x4000 100;250:detach",commands));
    assert(commands.size()==3 && commands[0].boundary==10 && !std::strcmp(commands[0].text,"0x8 1000"));
    assert(commands[2].boundary==250 && !std::strcmp(commands[2].text,"detach"));
    commands.clear();
    assert(!parse_input("10:0x8;5:0x4000",commands) && commands.size()==1); // out of order
    commands.clear();
    assert(!parse_input("x:0x8",commands) && commands.empty());
    return 0;
}
