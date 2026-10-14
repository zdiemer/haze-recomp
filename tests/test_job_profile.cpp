#define HAZE_JOB_PROFILE
#include "../port/haze_job_profile.cpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

int main(int argc,char** argv) {
    assert(argc==2);
    _putenv_s("HAZE_JOB_PROFILE_CSV",argv[1]);
    { HazeJobProfileScope wrong_thread(2,0xE94EE0,1,1);
      HazeJobProfileScope wrong_toc(1,0,1,1);
      HazeJobProfileScope wrong_job(1,0xE94EE0,512,1); }
    assert(recorded==0);
    for(unsigned i=0;i<65540;++i) {
        HazeJobProfileScope scope(1,0xE94EE0,i%512,0x1234);
        for(unsigned p=0;p<4;++p) { scope.start(p); scope.stop(p); }
    }
    { HazeJobProfileScope outer(1,0xE94EE0,10,0x5678);
      outer.start(3);
      { HazeJobProfileScope inner(1,0xE94EE0,11,0x567C); }
      outer.stop(3); }
    haze_job_profile_dump();
    std::ifstream file(argv[1]); std::string line;
    assert(std::getline(file,line)); unsigned count=0,nested=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' '); std::istringstream input(line);
        uint64_t f,b,e,job,caller,depth,t[4],n[4];
        input>>f>>b>>e>>job>>caller>>depth;
        for(auto& v:t) input>>v;
        for(auto& v:n) input>>v;
        assert(input && f>0 && e>=b && job<512);
        assert(t[0]+t[1]+t[2]+t[3]<=e-b);
        if(caller==0x1234) for(auto v:n) assert(v==1);
        if(depth) { assert(depth==1 && job==11); ++nested; }
        if(!count) assert(job==6);
        ++count;
    }
    assert(count==65536 && nested==1 && recorded==65542);
    puts("PASS: completion profiler guards, phase accounting, nesting and ring retention");
}
