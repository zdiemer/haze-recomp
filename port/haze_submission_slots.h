/* Two fence-owned submission storage slots for ordinary presentation.
 * Included after the renderer definitions. Each slot owns the command
 * allocator, upload/vertex/index/constant rings, shader-visible descriptor
 * heaps and the textures retired while it recorded. Ordinary presentation
 * submits its frame and waits only for the OTHER slot, so the renderer keeps
 * draining the guest FIFO while the GPU executes. Every other ld_flush remains
 * a full queue-completion boundary; guest-visible acknowledgements, readbacks
 * and movies are unchanged. Storage is never reused before its fence. */
typedef struct {
    ID3D12CommandAllocator* alloc;
    ID3D12Resource *upload, *vb, *ib, *cb, *ps_cb;
    u8 *upload_mapped, *vb_mapped, *cb_mapped, *ps_cb_mapped;
    u32* ib_mapped;
    ID3D12DescriptorHeap* srv_heap;
    ID3D12DescriptorHeap* smp_pages[LD_SAMPLER_HEAP_PAGES];
    u32 smp_page_count;
    ID3D12DescriptorHeap* postprocess_heap;
    ID3D12Resource* retired[MAX_RETIRED_TEXTURES];
    u32 retired_count;
    u64 fence;
} ld_frame_storage;
static struct {
    ld_frame_storage slots[2];
    u32 active;
    int initialized, unavailable;
    u64 submitted, waits, wait_ticks, sync_fallbacks;
} g_ld_slots;

static void ld_slots_capture(ld_frame_storage* s)
{
    s->alloc=g.alloc; s->upload=g.upload; s->vb=g.vb; s->ib=g.ib;
    s->cb=g.cb; s->ps_cb=g.ps_cb;
    s->upload_mapped=g.upload_mapped; s->vb_mapped=g.vb_mapped;
    s->ib_mapped=g.ib_mapped; s->cb_mapped=g.cb_mapped; s->ps_cb_mapped=g.ps_cb_mapped;
    s->srv_heap=g.srv_heap; s->smp_page_count=g.smp_page_count;
    memcpy(s->smp_pages,g.smp_pages,sizeof s->smp_pages);
    /* The postprocess heap may be created or replaced lazily by settings. */
    if(s->postprocess_heap!=g_ld_postprocess.heap) {
        if(g_ld_postprocess.heap) g_ld_postprocess.heap->lpVtbl->AddRef(g_ld_postprocess.heap);
        if(s->postprocess_heap) s->postprocess_heap->lpVtbl->Release(s->postprocess_heap);
        s->postprocess_heap=g_ld_postprocess.heap;
    }
}

static int ld_slots_activate(ld_frame_storage* s)
{
    g.alloc=s->alloc; g.upload=s->upload; g.vb=s->vb; g.ib=s->ib;
    g.cb=s->cb; g.ps_cb=s->ps_cb;
    g.upload_mapped=s->upload_mapped; g.vb_mapped=s->vb_mapped;
    g.ib_mapped=s->ib_mapped; g.cb_mapped=s->cb_mapped; g.ps_cb_mapped=s->ps_cb_mapped;
    g.srv_heap=s->srv_heap; g.smp_page_count=s->smp_page_count;
    memcpy(g.smp_pages,s->smp_pages,sizeof g.smp_pages);
    /* Settings, PSO and output remain global. Only descriptors rotate;
     * restoring a whole old postprocess struct would undo settings changes. */
    if(g_ld_postprocess.heap) {
        if(!s->postprocess_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,
                (void**)&s->postprocess_heap))) return 0;
        }
        s->postprocess_heap->lpVtbl->AddRef(s->postprocess_heap);
        g_ld_postprocess.heap->lpVtbl->Release(g_ld_postprocess.heap);
        g_ld_postprocess.heap=s->postprocess_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        s->postprocess_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->postprocess_heap,&cpu);
        cpu.ptr+=g_ld_postprocess.step;
        g.dev->lpVtbl->CreateUnorderedAccessView(g.dev,g_ld_postprocess.output,NULL,NULL,cpu);
    }
    g.upload_used=g.vb_used=g.ib_used=g.cb_used=g.ps_cb_used=0;
    g.srv_ring_used=0;
    ld_sampler_pages_reset();
    return 1;
}

/* Only for never-submitted storage or after queue completion is proved. */
static void ld_slots_release(ld_frame_storage* s)
{
    for (u32 i=0;i<s->retired_count;++i) s->retired[i]->lpVtbl->Release(s->retired[i]);
    if(s->postprocess_heap) s->postprocess_heap->lpVtbl->Release(s->postprocess_heap);
#define LD_SLOT_RELEASE(field) if(s->field) s->field->lpVtbl->Release(s->field)
    LD_SLOT_RELEASE(alloc); LD_SLOT_RELEASE(upload); LD_SLOT_RELEASE(vb);
    LD_SLOT_RELEASE(ib); LD_SLOT_RELEASE(cb); LD_SLOT_RELEASE(ps_cb); LD_SLOT_RELEASE(srv_heap);
#undef LD_SLOT_RELEASE
    for(u32 i=0;i<s->smp_page_count;++i) s->smp_pages[i]->lpVtbl->Release(s->smp_pages[i]);
    memset(s,0,sizeof *s);
}

static int ld_slots_buffer(u64 bytes,ID3D12Resource** resource,void** mapped)
{
    D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d={0}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
    d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr=g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&d,
        D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)resource);
    D3D12_RANGE no_read={0,0};
    if(SUCCEEDED(hr)) hr=(*resource)->lpVtbl->Map(*resource,0,&no_read,mapped);
    return SUCCEEDED(hr);
}

static int ld_slots_initialize(void)
{
    if(g_ld_slots.initialized) return 1;
    if(g_ld_slots.unavailable) return 0;
    ld_frame_storage* s=&g_ld_slots.slots[1];
    HRESULT hr=g.dev->lpVtbl->CreateCommandAllocator(g.dev,D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,(void**)&s->alloc);
    if(FAILED(hr) ||
        !ld_slots_buffer(UPLOAD_SIZE,&s->upload,(void**)&s->upload_mapped) ||
        !ld_slots_buffer(VERT_BUFFER_SIZE,&s->vb,(void**)&s->vb_mapped) ||
        !ld_slots_buffer(INDEX_BUFFER_SIZE,&s->ib,(void**)&s->ib_mapped) ||
        !ld_slots_buffer(CB_RING_BYTES,&s->cb,(void**)&s->cb_mapped) ||
        !ld_slots_buffer(PS_CB_RING_BYTES,&s->ps_cb,(void**)&s->ps_cb_mapped)) goto fail;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=SRV_RING_TABLES*SRV_TABLE_SIZE;
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,
        (void**)&s->srv_heap))) goto fail;
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors=SMP_RING_TABLES*SMP_TABLE_SIZE;
    for(u32 i=0;i<g.smp_page_count;++i) {
        if(FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,
            (void**)&s->smp_pages[i]))) goto fail;
        ++s->smp_page_count;
    }
    ld_slots_capture(&g_ld_slots.slots[0]);
    g_ld_slots.initialized=1;
    return 1;
fail:
    ld_slots_release(s); g_ld_slots.unavailable=1;
    fprintf(stderr,"[submission-slots] allocation failed; retaining synchronous submission\n");
    return 0;
}

static int ld_slots_wait(u64 value)
{
    u64 done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(done==UINT64_MAX) goto fail;
    if(done>=value) return 1;
    LARGE_INTEGER start, end; QueryPerformanceCounter(&start);
    HRESULT hr=g.fence->lpVtbl->SetEventOnCompletion(g.fence,value,g.fence_event);
    if(FAILED(hr)) goto fail;
    DWORD wait=WaitForSingleObject(g.fence_event,INFINITE);
    done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(wait!=WAIT_OBJECT_0 || done==UINT64_MAX || done<value) goto fail;
    QueryPerformanceCounter(&end);
    ++g_ld_slots.waits; g_ld_slots.wait_ticks+=(u64)(end.QuadPart-start.QuadPart);
    return 1;
fail:
    g.ready=0;
    fprintf(stderr,"[submission-slots] completion failed; storage retained\n");
    return 0;
}

static void ld_submit_frame(void)
{
    if(!g.ready) return;
    /* Movies keep their synchronous completion contract. */
    if(g_ld_movie_mode || !ld_slots_initialize()) {
        ++g_ld_slots.sync_fallbacks; ld_flush(LD_FLUSH_PRESENT); return;
    }
    ld_frame_storage* submitted=&g_ld_slots.slots[g_ld_slots.active];
    ld_frame_storage* next=&g_ld_slots.slots[g_ld_slots.active^1u];
#if defined(YZ_PERF_PROFILE)
    /* GPU timestamps resolve only on synchronous flushes. */
    g_ld_gpu_armed=0;
#endif
    /* Capture lazy postprocess replacement and transfer all retired
     * resources to the fence that follows their last possible use. */
    ld_slots_capture(submitted);
    if(submitted->retired_count) { g.ready=0; return; }
    HRESULT hr=g.list->lpVtbl->Close(g.list);
    if(FAILED(hr)) goto fail;
    ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
    submitted->fence=++g.fence_value;
    hr=g.queue->lpVtbl->Signal(g.queue,g.fence,submitted->fence);
    if(FAILED(hr)) goto fail;
    memcpy(submitted->retired,g.retired_textures,g.n_retired_textures*sizeof(g.retired_textures[0]));
    submitted->retired_count=g.n_retired_textures; g.n_retired_textures=0;
    ++g_ld_slots.submitted;
    /* At most two submissions outstanding. Only reuse the OTHER slot after
     * its completion, never the just-submitted slot's storage. */
    if(!ld_slots_wait(next->fence)) return;
    for(u32 i=0;i<next->retired_count;++i) next->retired[i]->lpVtbl->Release(next->retired[i]);
    next->retired_count=0;
    hr=next->alloc->lpVtbl->Reset(next->alloc);
    if(FAILED(hr)) goto fail;
    hr=g.list->lpVtbl->Reset(g.list,next->alloc,NULL);
    if(FAILED(hr)) goto fail;
    if(!ld_slots_activate(next)) { g.ready=0; return; }
    g_ld_slots.active^=1u;
    ld_drain_info_queue("submission-slots");
    return;
fail:
    g.ready=0; ld_dump_dred("submission-slots",hr);
}

/* Called only after the original synchronous shutdown flush succeeds, so both
 * slots are complete. The active slot remains owned by the original shutdown
 * code; release the other. */
static void ld_slots_shutdown(void)
{
    if(g_ld_slots.initialized) {
        const u32 other=g_ld_slots.active^1u;
        ld_slots_release(&g_ld_slots.slots[other]);
        if(g_ld_slots.slots[g_ld_slots.active].postprocess_heap)
            g_ld_slots.slots[g_ld_slots.active].postprocess_heap->lpVtbl->Release(
                g_ld_slots.slots[g_ld_slots.active].postprocess_heap);
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        fprintf(stderr,"[submission-slots] submissions=%llu waits=%llu wait_ms=%.3f sync_fallbacks=%llu\n",
            g_ld_slots.submitted,g_ld_slots.waits,
            1000.0*(double)g_ld_slots.wait_ticks/(double)f.QuadPart,g_ld_slots.sync_fallbacks);
    }
    memset(&g_ld_slots,0,sizeof g_ld_slots);
}
