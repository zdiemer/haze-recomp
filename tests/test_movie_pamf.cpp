#include "haze_pamf.h"
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace haze_movie;
static void word(std::vector<uint8_t>& b,size_t offset,uint32_t value) {
    for(int i=0;i<4;++i) b[offset+i]=uint8_t(value>>(24-i*8));
}
int main() {
    const auto path=std::filesystem::temp_directory_path()/"haze-movie-pamf-test.pam";
    std::vector<uint8_t> data(4096);
    memcpy(data.data(),"PAMF0041",8); word(data,8,1); word(data,12,1);
    word(data,0x58,90000); word(data,0x5e,180000);
    data[0x6f]=data[0x81]=data[0x87]=1; data[0x88]=2; data[0x8c]=0xe0;
    // A PES containing one elementary-stream byte and an explicit 90k PTS.
    const uint8_t pes[]={0,0,1,0xe0,0,9,0x80,0x80,5,0x21,0,5,0xbf,0x21,0xaa,0,0,1,0xb9};
    memcpy(data.data()+2048,pes,sizeof(pes));
    auto write=[&] { std::ofstream f(path,std::ios::binary); f.write((const char*)data.data(),data.size()); };
    auto rejected=[&] {
        write(); bool threw=false;
        try { Pamf p(path.string().c_str()); Packet packet; while(p.next(packet)) {} }
        catch(const std::runtime_error&) { threw=true; }
        assert(threw);
    };
    write();
    {
        Pamf p(path.string().c_str()); Packet packet;
        assert(p.start==90000 && p.end==180000 && p.audio_id==-1);
        assert(p.next(packet) && packet.pts==90000 && packet.dts==90000);
        assert(packet.data.size()==1 && packet.data[0]==0xaa);
        assert(!p.next(packet) && !p.next(packet));
    }
    auto original=data;
    data[0]='X'; rejected(); data=original;
    word(data,8,0xffffffff); rejected(); data=original;
    word(data,12,2); rejected(); data=original;
    data[0x87]=255; rejected(); data=original;
    data[2048+8]=255; rejected(); data=original;
    data[2048+9]&=0xfe; rejected(); data=original;
    data[2048+4]=0xff; rejected(); data=original;
    data[2048]=1; rejected(); data=original;
    data.resize(2049); rejected();
    std::filesystem::remove(path);
    puts("PAMF bounded header/PES parsing, timestamps and malformed/truncated input passed.");
}
