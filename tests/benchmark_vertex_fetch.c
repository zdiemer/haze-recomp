/* Synthetic throughput diagnostic for the actual compact vertex batch API.
 * The scalar decoder/reference batch is an independent correctness oracle.
 * CPU cycles exclude descheduling; this benchmark does not predict game FPS. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

enum { VERTICES=1024, SOURCE_STRIDE=16, ATTRIBUTES=8, REPEATS=2000 };
static u8 source[ATTRIBUTES*(VERTICES*SOURCE_STRIDE+16)];
static u8 actual[VERTICES*ATTRIBUTES*16], expected[sizeof(actual)];
static rsx_vertex_ref refs[VERTICES];

int main(int argc,char** argv)
{
    unsigned modes=RSX_VERTEX_BATCH_FLOAT_BYTE | RSX_VERTEX_BATCH_SINT16;
    assert(argc<=2);
    if (argc==2) {
        assert(argv[1][0]>='0' && argv[1][0]<='3' && !argv[1][1]);
        modes=(unsigned)(argv[1][0]-'0');
    }
    for (u32 i=0;i<sizeof(source);++i) source[i]=(u8)(i*73u+(i>>2));
    for (u32 i=0;i<VERTICES;++i) refs[i].vertex_id=(i*67u)&(VERTICES-1u);
    puts("round,modes,size,attributes,vertices,cycles_per_vertex");
    for (u32 round=0;round<3;++round)
    for (u32 size=1;size<=4;++size)
    for (u32 attributes=1;attributes<=ATTRIBUTES;attributes*=2)
    for (u32 count=64;count<=VERTICES;count*=4) {
        rsx_vertex_fetch_plan plan={0};
        rsx_vertex_layout_plan_init(&plan.layout,(1u<<attributes)-1u);
        for (u32 slot=0;slot<attributes;++slot) {
            rsx_vertex_fetch_attr* f=&plan.attr[slot];
            f->desc.type=RSX_VTX_TYPE_SINT16; f->desc.size=size;
            f->stride=SOURCE_STRIDE; f->elem_size=size*2;
            f->stable_base=source+slot*(VERTICES*SOURCE_STRIDE+16)+slot;
            f->stable_last=VERTICES-1u;
        }
        memset(actual,0xA5,sizeof(actual)); memset(expected,0xA5,sizeof(expected));
        assert(rsx_vertex_fetch_many_ex(&plan,refs,count,expected,0));
        assert(rsx_vertex_fetch_many_mode(&plan,refs,count,actual,modes));
        assert(!memcmp(actual,expected,sizeof(actual)));
        ULONG64 begin,end;
        assert(QueryThreadCycleTime(GetCurrentThread(),&begin));
        for (u32 repeat=0;repeat<REPEATS;++repeat)
            assert(rsx_vertex_fetch_many_mode(&plan,refs,count,actual,modes));
        assert(QueryThreadCycleTime(GetCurrentThread(),&end));
        assert(!memcmp(actual,expected,sizeof(actual)));
        printf("%u,%u,%u,%u,%u,%.6f\n",round,modes,size,attributes,count,
               (double)(end-begin)/REPEATS/count);
    }
    return 0;
}
