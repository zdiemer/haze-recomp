/* Asset-free real-device shader/readback test and isolated draw-cost probe.
 * The production overlay has no GPU queries or readbacks. */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
static struct {
    ID3D12Device* dev;
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* backbuf[1];
    unsigned n_textures;
} g;
static struct { unsigned host_w,host_h; } g_ld_resolution;
static unsigned g_ld_last_frame_draws=1234;
static HWND g_ld_display_hwnd;
#define LD_PRESENT_RING_CAP 256u
typedef struct { uint64_t present_id; int64_t qpc; unsigned guest_frame; } ld_present_sample;
static ld_present_sample g_ld_present_ring[LD_PRESENT_RING_CAP];
static uint64_t g_ld_present_total=120;
static int64_t g_ld_qpc_frequency=60000;
static D3D12_CPU_DESCRIPTOR_HANDLE target_handle;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(unsigned index) { (void)index; return target_handle; }
#include "../port/haze_overlay_d3d12.h"
static ID3D12CommandQueue* queue;
static ID3D12CommandAllocator* allocator;
static ID3D12Fence* fence;
static HANDLE event;
static UINT64 serial;
static void submit(void) {
    assert(SUCCEEDED(g.list->lpVtbl->Close(g.list)));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    assert(SUCCEEDED(queue->lpVtbl->Signal(queue,fence,++serial)));
    assert(SUCCEEDED(fence->lpVtbl->SetEventOnCompletion(fence,serial,event)));
    assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
    assert(SUCCEEDED(allocator->lpVtbl->Reset(allocator)));
    assert(SUCCEEDED(g.list->lpVtbl->Reset(g.list,allocator,NULL)));
}
int main(void) {
    assert(SUCCEEDED(D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&g.dev)));
    D3D12_COMMAND_QUEUE_DESC q={0};
    assert(SUCCEEDED(g.dev->lpVtbl->CreateCommandQueue(g.dev,&q,&IID_ID3D12CommandQueue,(void**)&queue)));
    assert(SUCCEEDED(g.dev->lpVtbl->CreateCommandAllocator(g.dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&allocator)));
    assert(SUCCEEDED(g.dev->lpVtbl->CreateCommandList(g.dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&g.list)));
    assert(SUCCEEDED(g.dev->lpVtbl->CreateFence(g.dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fence)));
    event=CreateEvent(NULL,FALSE,FALSE,NULL); assert(event);
    ID3D12DescriptorHeap* heap=NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    assert(SUCCEEDED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&heap)));
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&target_handle);
    assert(haze_overlay_init());
    for (unsigned i=0;i<120;++i) {
        g_ld_present_ring[i].present_id=i+1; g_ld_present_ring[i].qpc=(i+1)*2000;
        g_ld_present_ring[i].guest_frame=i+1;
    }
    ID3D12QueryHeap* queries=NULL;
    D3D12_QUERY_HEAP_DESC qh={0}; qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count=2;
    assert(SUCCEEDED(g.dev->lpVtbl->CreateQueryHeap(g.dev,&qh,&IID_ID3D12QueryHeap,(void**)&queries)));
    UINT64 gpu_frequency=0; assert(SUCCEEDED(queue->lpVtbl->GetTimestampFrequency(queue,&gpu_frequency)));
    LARGE_INTEGER cpu_frequency; QueryPerformanceFrequency(&cpu_frequency);
    const unsigned dimensions[][2]={{640,480},{1280,720},{2560,1080},{3840,2160}};
    for (unsigned size=0;size<4;++size) {
        const unsigned width=dimensions[size][0],height=dimensions[size][1];
        g_ld_resolution.host_w=width; g_ld_resolution.host_h=height;
        D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc={0}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width; desc.Height=height; desc.DepthOrArraySize=desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        assert(SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PRESENT,NULL,&IID_ID3D12Resource,(void**)&g.backbuf[0])));
        g.dev->lpVtbl->CreateRenderTargetView(g.dev,g.backbuf[0],NULL,target_handle);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; UINT64 total;
        g.dev->lpVtbl->GetCopyableFootprints(g.dev,&desc,0,1,0,&footprint,NULL,NULL,&total);
        hp.Type=D3D12_HEAP_TYPE_READBACK; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=total+256; desc.Height=1; desc.Format=DXGI_FORMAT_UNKNOWN;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* readback=NULL;
        assert(SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback)));
        unsigned char* bytes=NULL; D3D12_RANGE range={0,(SIZE_T)total+16};
        assert(SUCCEEDED(readback->lpVtbl->Map(readback,0,&range,(void**)&bytes)));
        for (unsigned mode=0;mode<=2;++mode) {
            double gpu_ms=0,cpu_us=0;
            for (unsigned iteration=0;iteration<100;++iteration) {
                const unsigned corner=iteration%4; haze_overlay_set(mode,corner);
                D3D12_RESOURCE_BARRIER b={0}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource=g.backbuf[0]; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
                g.list->lpVtbl->ResourceBarrier(g.list,1,&b);
                const float blue[4]={0,0,1,1}; g.list->lpVtbl->ClearRenderTargetView(g.list,target_handle,blue,0,NULL);
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
                g.list->lpVtbl->ResourceBarrier(g.list,1,&b);
                g.list->lpVtbl->EndQuery(g.list,queries,D3D12_QUERY_TYPE_TIMESTAMP,0);
                LARGE_INTEGER begin,end; QueryPerformanceCounter(&begin);
                haze_overlay_draw(0); QueryPerformanceCounter(&end);
                g.list->lpVtbl->EndQuery(g.list,queries,D3D12_QUERY_TYPE_TIMESTAMP,1);
                g.list->lpVtbl->ResolveQueryData(g.list,queries,D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback,total);
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                g.list->lpVtbl->ResourceBarrier(g.list,1,&b);
                D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
                dst.pResource=readback; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
                src.pResource=g.backbuf[0]; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
                g.list->lpVtbl->ResourceBarrier(g.list,1,&b); submit();
                assert(SUCCEEDED(g.dev->lpVtbl->GetDeviceRemovedReason(g.dev)));
                if (iteration>=4) {
                    const UINT64* stamps=(const UINT64*)(bytes+total);
                    gpu_ms+=(double)(stamps[1]-stamps[0])*1000/gpu_frequency;
                    cpu_us+=(double)(end.QuadPart-begin.QuadPart)*1000000/cpu_frequency.QuadPart;
                }
                const haze_overlay_rect r=haze_overlay_layout(width,height,height,mode,corner,haze_overlay_columns);
                unsigned ink=0,background=0;
                for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                    const unsigned char* pixel=bytes+y*footprint.Footprint.RowPitch+x*4;
                    if (pixel[2]==255 && pixel[0]==0) continue;
                    assert(mode && x>=r.x-1 && x<=r.x+r.width+1 && y>=r.y-1 && y<=r.y+r.height+1);
                    if (pixel[1]>200) ++ink; else ++background;
                }
                if (mode) assert(ink>50 && background>20);
                else assert(ink==0 && background==0);
            }
            printf("%ux%u mode=%u CPU %.3f us (includes refresh), GPU %.4f ms\n",width,height,mode,cpu_us/96,gpu_ms/96);
        }
        readback->lpVtbl->Unmap(readback,0,NULL); readback->lpVtbl->Release(readback);
        g.backbuf[0]->lpVtbl->Release(g.backbuf[0]);
    }
    haze_overlay_destroy();
    puts("Real D3D12 overlay: shader, corners, bounded pixels and disabled no-op passed.");
    return 0;
}
