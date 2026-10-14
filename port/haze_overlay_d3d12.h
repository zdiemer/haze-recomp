/* Included inside the audited renderer translation unit. One draw on the final
 * backbuffer, after game postprocessing and movie composition, before Present.
 * No input surface, uploads, per-frame allocations, queries, waits or file I/O. */
#include "haze_overlay.h"
#include <psapi.h>
static unsigned haze_overlay_mode,haze_overlay_corner;
static ID3D12RootSignature* haze_overlay_root;
static ID3D12PipelineState* haze_overlay_pipeline;
static int64_t haze_overlay_refresh;
static char haze_overlay_text[240];
static unsigned haze_overlay_columns=6,haze_overlay_display_height;

static void haze_overlay_destroy(void) {
    if (haze_overlay_pipeline) haze_overlay_pipeline->lpVtbl->Release(haze_overlay_pipeline);
    if (haze_overlay_root) haze_overlay_root->lpVtbl->Release(haze_overlay_root);
    haze_overlay_pipeline=NULL; haze_overlay_root=NULL;
}
static int haze_overlay_init(void) {
    /* Original 4x7 uppercase bitmap alphabet, encoded one nibble per row.
     * Root constants carry 40 columns x 6 lines; no mutable GPU resources. */
    static const char shader[]=
        "cbuffer Data:register(b0) { float4 area; uint4 text[15]; };\n"
        "float4 vs(uint id:SV_VertexID):SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }\n"
        "uint glyph(uint c) { switch(c) {\n"
        "case 48:return 0x6999996; case 49:return 0x2622227; case 50:return 0x691248F;\n"
        "case 51:return 0xE11611E; case 52:return 0x226AF22; case 53:return 0xF88E11E;\n"
        "case 54:return 0x688E996; case 55:return 0xF112244; case 56:return 0x6996996; case 57:return 0x6997116;\n"
        "case 65:return 0x699F999; case 66:return 0xE99E99E; case 67:return 0x6988896; case 68:return 0xE99999E;\n"
        "case 69:return 0xF88E88F; case 70:return 0xF88E888; case 71:return 0x698B996; case 72:return 0x999F999;\n"
        "case 73:return 0x7222227; case 74:return 0x1111196; case 75:return 0x99ACE99; case 76:return 0x888888F;\n"
        "case 77:return 0x9FF9999; case 78:return 0x9DDBB99; case 79:return 0x6999996; case 80:return 0xE99E888;\n"
        "case 81:return 0x6999B65; case 82:return 0xE99ECA9; case 83:return 0x788611E; case 84:return 0xF222222;\n"
        "case 85:return 0x9999996; case 86:return 0x9999966; case 87:return 0x9999FF9; case 88:return 0x9966699;\n"
        "case 89:return 0x9996222; case 90:return 0xF11248F; case 46:return 0x0000002; case 58:return 0x0020020;\n"
        "case 47:return 0x1122488; case 45:return 0x000F000; case 40:return 0x2444442; case 41:return 0x4222224;\n"
        "default:return 0; }}\n"
        "bool inkAt(int2 p) {\n"
        " if(p.x<0 || p.y<0 || p.x>=240 || p.y>=int(area.w)*9) return false;\n"
        " uint index=(p.y/9)*40+p.x/6; uint word=index/4; uint c=(text[word/4][word%4]>>((index%4)*8))&255;\n"
        " uint x=p.x%6,y=p.y%9; return x<4 && y<7 && ((glyph(c)>>((6-y)*4+3-min(x,3)))&1); }\n"
        "float4 ps(float4 position:SV_Position):SV_Target {\n"
        " bool compact=area.w==1; int2 p=int2(floor((position.xy-area.xy)/area.z))-(compact?1:6);\n"
        " if(inkAt(p)) return compact ? float4(.7,1,.45,1) : float4(.9,1,.9,1);\n"
        " if(compact) { if(inkAt(p-int2(1,1))) return float4(0,0,0,1); discard; }\n"
        " return float4(.015,.02,.025,1); }\n";
    ID3DBlob *vs=NULL,*ps=NULL,*signature=NULL;
    HRESULT hr=D3DCompile(shader,sizeof(shader)-1,"overlay",NULL,NULL,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,NULL);
    if (SUCCEEDED(hr)) hr=D3DCompile(shader,sizeof(shader)-1,"overlay",NULL,NULL,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,NULL);
    D3D12_ROOT_PARAMETER param={0}; param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues=64;
    D3D12_ROOT_SIGNATURE_DESC root={0}; root.NumParameters=1; root.pParameters=&param;
    if (SUCCEEDED(hr)) hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&signature,NULL);
    if (SUCCEEDED(hr)) hr=g.dev->lpVtbl->CreateRootSignature(g.dev,0,signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature),&IID_ID3D12RootSignature,(void**)&haze_overlay_root);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p={0}; p.pRootSignature=haze_overlay_root;
        p.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs); p.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
        p.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps); p.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
        p.SampleMask=UINT_MAX; p.SampleDesc.Count=1;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        hr=g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev,&p,&IID_ID3D12PipelineState,(void**)&haze_overlay_pipeline);
    }
    if (vs) vs->lpVtbl->Release(vs);
    if (ps) ps->lpVtbl->Release(ps);
    if (signature) signature->lpVtbl->Release(signature);
    if (FAILED(hr)) { haze_overlay_destroy(); return 0; }
    return 1;
}
static void haze_overlay_set(unsigned mode,unsigned corner) {
    haze_overlay_mode=mode; haze_overlay_corner=corner; haze_overlay_refresh=0;
}
static void haze_overlay_start(void) {
    const char* mode=getenv("HAZE_PERFORMANCE_OVERLAY");
    const char* corner=getenv("HAZE_OVERLAY_POSITION");
    haze_overlay_set(mode && !strcmp(mode,"expanded") ? 2 : mode && !strcmp(mode,"compact") ? 1 : 0,
        (corner && strstr(corner,"right") ? 1 : 0) | (corner && strstr(corner,"bottom") ? 2 : 0));
    /* Prepare once at renderer startup even when hidden: enabling never compiles
     * shaders in a gameplay frame. Failure is reported by Video Settings. */
    if (!haze_overlay_init()) fprintf(stderr,"[graphics] Performance overlay initialization failed.\n");
}
static haze_overlay_sample haze_overlay_read_present(uint64_t id,void* context) {
    (void)context;
    const ld_present_sample* s=&g_ld_present_ring[(id-1)&(LD_PRESENT_RING_CAP-1)];
    const haze_overlay_sample result={s->present_id,s->qpc,s->guest_frame}; return result;
}
static void haze_overlay_line(unsigned row,const char* line) {
    size_t length=strlen(line); if (length>40) length=40;
    memcpy(haze_overlay_text+row*40,line,length);
}
static void haze_overlay_draw(uint32_t backbuffer) {
    if (!haze_overlay_mode || !haze_overlay_pipeline) return;
    const int64_t now=g_ld_present_total ? haze_overlay_read_present(g_ld_present_total,NULL).ticks : 0;
    if (!haze_overlay_refresh || now-haze_overlay_refresh>=g_ld_qpc_frequency/4) {
        haze_overlay_refresh=now;
        const haze_overlay_stats stats=haze_overlay_measure(g_ld_present_total,LD_PRESENT_RING_CAP,
            g_ld_qpc_frequency,haze_overlay_read_present,NULL);
        char line[80]; memset(haze_overlay_text,' ',sizeof haze_overlay_text);
        RECT client;
        if (GetClientRect(g_ld_display_hwnd,&client) && client.bottom>0)
            haze_overlay_display_height=(unsigned)client.bottom;
        if (stats.ready) {
            snprintf(line,sizeof line,haze_overlay_mode==1 ? "%.0f FPS" : "PRESENT: %.1f FPS",stats.fps);
            haze_overlay_columns=(unsigned)strlen(line); haze_overlay_line(0,line);
            snprintf(line,sizeof line,"INTERVAL: %.2f MS",stats.interval_ms); haze_overlay_line(1,line);
            snprintf(line,sizeof line,"GUEST FLIPS: %.1f/S (NOT SIM)",stats.guest_flips); haze_overlay_line(2,line);
        } else {
            haze_overlay_line(0,haze_overlay_mode==1 ? "-- FPS" : "PRESENT: SAMPLING");
            haze_overlay_columns=6; haze_overlay_line(1,"INTERVAL: N/A");
        }
        if (haze_overlay_mode==2) {
            snprintf(line,sizeof line,"DRAWS: %u  TEX CACHE: %u",g_ld_last_frame_draws,g.n_textures); haze_overlay_line(3,line);
            PROCESS_MEMORY_COUNTERS memory={0}; memory.cb=sizeof memory;
            if (K32GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof memory)) {
                snprintf(line,sizeof line,"PROCESS RAM (WS): %.0f MIB",(double)memory.WorkingSetSize/(1024*1024)); haze_overlay_line(4,line);
            } else haze_overlay_line(4,"PROCESS RAM (WS): N/A");
            haze_overlay_line(5,"CPU / GPU TIME: N/A");
        }
    }
    const haze_overlay_rect rect=haze_overlay_layout(g_ld_resolution.host_w,g_ld_resolution.host_h,
        haze_overlay_display_height,haze_overlay_mode,haze_overlay_corner,haze_overlay_columns);
    struct { float area[4]; char text[240]; } constants={{rect.x,rect.y,rect.pixel,haze_overlay_mode==2 ? 6.0f : 1.0f},{0}};
    memcpy(constants.text,haze_overlay_text,sizeof constants.text);
    D3D12_RESOURCE_BARRIER barrier={0}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=g.backbuf[backbuffer]; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv=rtv_handle(backbuffer);
    g.list->lpVtbl->OMSetRenderTargets(g.list,1,&rtv,FALSE,NULL);
    const D3D12_VIEWPORT viewport={rect.x,rect.y,rect.width,rect.height,0,1};
    const D3D12_RECT scissor={(LONG)rect.x,(LONG)rect.y,(LONG)(rect.x+rect.width),(LONG)(rect.y+rect.height)};
    g.list->lpVtbl->RSSetViewports(g.list,1,&viewport); g.list->lpVtbl->RSSetScissorRects(g.list,1,&scissor);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list,haze_overlay_root);
    g.list->lpVtbl->SetPipelineState(g.list,haze_overlay_pipeline);
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list,0,64,&constants,0);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list,3,1,0,0);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier);
    /* The following ld_flush resets the command list and all cached bindings. */
}
