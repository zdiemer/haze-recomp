#include "haze_job13_native_update.h"

#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

#include "spu_helpers.h"
#include "haze_job13_native_math.h"

namespace {

constexpr uint32_t kImage = 32;
constexpr uint32_t kLoopEntry = 0x9A00;
constexpr uint32_t kLoopExit = 0x9BC4;
constexpr uint32_t kObject = 0x108C0;
constexpr uint32_t kParticles = 0x10D00;
constexpr uint32_t kCapacityMax = 4096;
constexpr uint32_t kObjectBytes = 0x440;
constexpr uint32_t kParticleBytes = 0x28;
constexpr uint32_t kStackCount = 0x15C0;
constexpr uint32_t kStackCursor = 0x15D0;
constexpr uint32_t kStackAux = 0xF0;
constexpr uint32_t kStackAuxCount = 0x1610;
constexpr uint32_t kDtAddress = 0x108B0;
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static bool span(uint32_t ea, uint32_t size) {
    return ea <= SPU_LS_SIZE && size <= SPU_LS_SIZE - ea;
}
static uint32_t ls32(const spu_context* ctx, uint32_t ea) {
    return be32(ctx->ls + ea);
}
static u128 splat(uint32_t x) { u128 r; for (unsigned i=0;i<4;++i) r._u32[i]=x; return r; }
static uint32_t add(uint32_t a, uint32_t b) { return spu_fa(splat(a),splat(b))._u32[0]; }
static uint32_t sub(uint32_t a, uint32_t b) { return spu_fs(splat(a),splat(b))._u32[0]; }
static uint32_t mul(uint32_t a, uint32_t b) { return spu_fm(splat(a),splat(b))._u32[0]; }
static uint32_t madd(uint32_t a, uint32_t b, uint32_t c) { return spu_fma(splat(a),splat(b),splat(c))._u32[0]; }
static uint32_t nms(uint32_t a, uint32_t b, uint32_t c) {
    return spu_fnms(splat(a),splat(b),splat(c))._u32[0];
}
static bool gt(uint32_t a, uint32_t b) { return spu_float_gt(a,b) != 0; }
static uint32_t from_i8(uint8_t b) {
    const int32_t v=(int8_t)b;
    const uint32_t magnitude=(uint32_t)(v<0 ? -v : v);
    return spu_float_pack(magnitude,0,v<0 ? 0x80000000u : 0);
}
static uint32_t reciprocal(uint32_t bits) {
    u128 a=spu_zero(); a._u32[0]=bits;
    u128 estimate=spu_frest(a);
    return spu_fi(a,estimate)._u32[0];
}

static uint32_t clamp01(uint32_t u) {
    const uint32_t zero=0, one=0x3f800000u;
    if (!gt(u,zero)) return zero;
    if (gt(u,one)) return one;
    return u;
}

static uint32_t curve(const uint8_t* object, uint32_t value_bits, bool variation) {
    const unsigned times[5] = { variation?0x30u:0x04u, variation?0x38u:0x0cu,
                                variation?0x40u:0x14u, variation?0x48u:0x1cu,
                                variation?0x50u:0x24u };
    const unsigned values[5] = { times[0]+4u,times[1]+4u,times[2]+4u,
                                 times[3]+4u,times[4]+4u };
    uint32_t t[5], v[5], line[4];
    for (unsigned i=0;i<5;++i) { t[i]=be32(object+times[i]); v[i]=be32(object+values[i]); }
    for (unsigned i=0;i<4;++i) {
        const uint32_t fraction=mul(sub(value_bits,t[i]),reciprocal(sub(t[i+1],t[i])));
        line[i]=madd(fraction,sub(v[i+1],v[i]),v[i]);
    }
    const bool a[4]={gt(0xc0000000u,value_bits),gt(t[0],value_bits),
                     gt(t[1],value_bits),gt(t[2],value_bits)};
    const bool b[4]={gt(t[3],value_bits),gt(t[4],value_bits),
                     gt(0x40000000u,value_bits),gt(0x40000000u,value_bits)};
    const bool lm[4]={a[0],a[1]&&!a[0],a[2]&&!a[1],a[3]&&!a[2]};
    const bool rm[4]={b[0]&&!a[3],b[1]&&!b[0],b[2]&&!b[1],false};
    const uint32_t lv[4]={v[0],v[0],line[0],line[1]};
    const uint32_t rv[4]={line[2],line[3],v[4],0};
    uint32_t w[4];
    for (unsigned i=0;i<4;++i)
        w[i]=add(lm[i]?lv[i]:0,rm[i]?rv[i]:0);
    return add(add(w[0],w[2]),add(w[3],w[1]));
}

static bool dynamic_aux_is_zero_type5(const spu_context* ctx, uint32_t sp,
                                      uint32_t count) {
    if (count>4 || !span(sp+kStackAux,16) || !span(sp+kStackAuxCount,16)) return false;
    for (uint32_t i=0;i<count;++i) {
        const uint32_t ea=ls32(ctx,sp+kStackAux+i*4);
        if (!span(ea,196) || ctx->ls[ea+0xc1]!=5 ||
            be32(ctx->ls+ea+0x00)!=0 || be32(ctx->ls+ea+0x04)!=0 ||
            be32(ctx->ls+ea+0x08)!=0) return false;
    }
    return true;
}

static bool gate(const spu_context* ctx, uint32_t& object_ea, uint32_t& capacity,
                 uint32_t& live, uint32_t& cursor, uint32_t& sp,
                 uint32_t& dt, uint32_t& aux_count) {
    if (!ctx || ctx->image_id!=kImage) { return false; }
    sp=ctx->gpr[1]._u32[0];
    if ((sp&15u) || !span(sp,0x1660)) { return false; }
    if (ctx->gpr[89]._u32[0]!=0 || ctx->gpr[89]._u32[1] ||
        ctx->gpr[89]._u32[2] || ctx->gpr[89]._u32[3]) { return false; }
    const uint32_t capacity_ptr=ctx->gpr[107]._u32[0];
    if (capacity_ptr<0x414 || capacity_ptr>SPU_LS_SIZE) { return false; }
    object_ea=capacity_ptr-0x414;
    if (object_ea!=kObject || !span(object_ea,kObjectBytes)) { return false; }
    capacity=be32(ctx->ls+object_ea+0x414);
    live=ls32(ctx,sp+kStackCount);
    cursor=ls32(ctx,sp+kStackCursor);
    dt=ls32(ctx,kDtAddress);
    aux_count=ls32(ctx,sp+kStackAuxCount);
    if (capacity>kCapacityMax || live>capacity || aux_count>4) { return false; }
    if (capacity==0) { if (live || cursor) { return false; } }
    else if (cursor>=capacity) { return false; }
    if ((uint64_t)capacity*kParticleBytes > SPU_LS_SIZE-kParticles ||
        !span(kParticles,capacity*kParticleBytes)) { return false; }
    if (!dynamic_aux_is_zero_type5(ctx,sp,aux_count)) { return false; }
    const uint8_t* o=ctx->ls+object_ea;
    if (be32(o+0x348)&0x40u || (be32(o+0x438)&8u)) { return false; }
    if (be32(o+0x2f0) || be32(o+0x2f4) || be32(o+0x2f8)) { return false; }
    return true;
}

static bool drag(const spu_context* ctx, uint32_t& vx, uint32_t& vy, uint32_t& vz, uint32_t dt,
                 uint32_t drag_bits, uint8_t random, uint32_t sp) {
    if (!gt(drag_bits,0)) return true;
    const uint32_t speed_sq=madd(vz,vz,madd(vy,vy,mul(vx,vx)));
    if (!gt(speed_sq,0)) return true;
    u128 s=spu_zero(); s._u32[0]=speed_sq;
    u128 d=spu_fesd(s);
    const uint64_t input_bits=((uint64_t)d._u32[0]<<32)|d._u32[1];
    double root=0.0;
    if (sp<336 || !span(sp-72u,8) || !haze_job13_positive_sqrt(spu__dget(d,0),&root)) return false;
    uint64_t upper=0;
    /* The original DA08 status-only upper lane is sourced from its stack. The
     * audited caller's own stack is stable at this boundary; preserve it. */
    for (unsigned i=0;i<8;++i) upper=(upper<<8)|ctx->ls[sp-72u+i];
    haze_job13_native_upper_sqrt_effects(input_bits,upper);
    u128 sqrt_double=spu_zero(); spu__dset(&sqrt_double,0,root);
    const uint32_t speed=spu_frds(sqrt_double)._u32[0];
    if (!gt(speed,0)) return false;
    const uint32_t jitter=madd(from_i8(random),0x3ace699fu,0x3f4ccccc);
    const uint32_t loss=mul(mul(dt,drag_bits),jitter);
    uint32_t new_speed=nms(loss,mul(speed,speed),speed);
    if (gt(0,new_speed)) new_speed=0;
    const uint32_t r=reciprocal(speed);
    const uint32_t t=mul(new_speed,r);
    const uint32_t scale=madd(nms(t,speed,new_speed),r,t);
    vx=mul(vx,scale); vy=mul(vy,scale); vz=mul(vz,scale);
    return true;
}

} // namespace

extern "C" bool haze_job13_native_update_try(spu_context* ctx) { 
    const unsigned original_mxcsr=_mm_getcsr();
    uint32_t object_ea=0,capacity=0,live=0,cursor=0,sp=0,dt=0,aux_count=0;
    if (!gate(ctx,object_ea,capacity,live,cursor,sp,dt,aux_count)) return false;

    /* Full object and capacity payload are shadowed first. No LS write occurs
     * until every particle and every supported helper completes. */
    uint8_t object[kObjectBytes];
    std::memcpy(object,ctx->ls+object_ea,sizeof object);
    /* Per-worker shadow sized for the gate's capacity limit; no allocation. */
    static thread_local uint8_t particles[kCapacityMax*kParticleBytes];
    const size_t particle_bytes=(size_t)capacity*kParticleBytes;
    std::memcpy(particles,ctx->ls+kParticles,particle_bytes);
    uint32_t cursor_out=be32(object+0x424),live_out=be32(object+0x418);
    if ((capacity && cursor_out>=capacity) || live_out!=live || cursor_out!=cursor) { 
        _mm_setcsr(original_mxcsr); return false;
    }
    uint32_t min[3]={be32(object+0x3c0),be32(object+0x3c4),be32(object+0x3c8)};
    uint32_t max[3]={be32(object+0x3cc),be32(object+0x3d0),be32(object+0x3d4)};
    uint32_t last_retired=UINT32_MAX;
    const uint32_t force[3]={ctx->gpr[111]._u32[0],ctx->gpr[112]._u32[0],ctx->gpr[113]._u32[0]};
    const uint32_t retire_a=add(be32(object+0x310),be32(object+0x314));
    const uint32_t retire_b=add(be32(object+0x324),be32(object+0x328));
    const uint32_t retire_limit=gt(retire_b,retire_a)?retire_b:retire_a;
    const bool paired=be32(object+0x34c)==3;
    for (uint32_t i=0;i<live;++i) {
        uint32_t physical=cursor+i;
        if (physical>=capacity) physical-=capacity;
        if (physical>=capacity) { _mm_setcsr(original_mxcsr); return false; }
        uint8_t* p=particles+physical*kParticleBytes;
        uint32_t flags=p[0x24];
        bool update=true,age_on_skip=true;
        const uint32_t age=be32(p+0x1c);
        if (gt(age,0)) {
            const uint32_t next_age=add(age,dt);
            const bool special=(flags&4u)!=0;
            const uint32_t base=be32(object+(special?0x310:0x324));
            const uint32_t variation=be32(object+(special?0x314:0x328));
            const uint32_t lifetime=madd(mul(variation,0x3c010204u),from_i8(p[0x20]),base);
            if (gt(next_age,lifetime)) {
                update=false;
                if (!(flags&1u)) {
                    if (special) {
                        flags&=~4u;
                        uint32_t n=be32(object+0x420); put_be32(object+0x420,n-1u);
                    }
                    flags|=1u; p[0x24]=(uint8_t)flags;
                }
            }
            if (gt(next_age,retire_limit)) {
                uint32_t next=be32(object+0x424)+1u;
                if (next>=capacity) next-=capacity;
                put_be32(object+0x424,next);
                put_be32(object+0x418,be32(object+0x418)-1u);
                if (flags&2u) object[0x43e]=(uint8_t)(object[0x43e]-1u);
                update=false; age_on_skip=false; last_retired=i;
            }
        }
        if (paired && (i&1u)) {
            if (last_retired==i-1u) {
                uint32_t next=be32(object+0x424)+1u;
                if (next>=capacity) next-=capacity;
                put_be32(object+0x424,next);
                put_be32(object+0x418,be32(object+0x418)-1u);
                if (flags&2u) object[0x43e]=(uint8_t)(object[0x43e]-1u);
                age_on_skip=false; last_retired=i;
                update=false;
            } else if (gt(be32(object+0x33c),age)) {
                if (flags&8u) { flags&=~8u; p[0x24]=(uint8_t)flags; age_on_skip=false; }
                update=false;
            }
        }
        if (update) {
            if (flags&8u) {
                flags&=~8u; p[0x24]=(uint8_t)flags;
            } else {
                uint32_t blend=(flags&4u)?be32(object+0x320):be32(object+0x340);
                if (!gt(blend,0)) blend=0;
                else if (gt(blend,0x3f800000u)) blend=0x3f800000u;
                uint32_t pos[3],vel[3];
                for (unsigned c=0;c<3;++c) {
                    pos[c]=be32(p+c*4u); vel[c]=be32(p+0x0c+c*4u);
                    put_be32(p+c*4u,madd(dt,vel[c],pos[c]));
                }
                uint32_t scalar=be32(p+0x18);
                uint32_t gravity=be32(object+0x308);
                vel[1]=nms(blend,mul(dt,gravity),vel[1]);
                const uint32_t rate=madd(mul(be32(object+0x338),0x3c010204u),
                                         from_i8(p[0x21]),be32(object+0x334));
                scalar=madd(mul(0x40c90fdau,dt),rate,scalar);
                const uint32_t factor=mul(sub(0x3f800000u,blend),dt);
                if (!drag(ctx,vel[0],vel[1],vel[2],dt,be32(object+0x30c),p[0x22],sp)) { 
                    _mm_setcsr(original_mxcsr); return false;
                }
                /* A2D0 applies accumulated force after drag; preserve order. */
                for (unsigned c=0;c<3;++c) vel[c]=madd(force[c],factor,vel[c]);
                for (unsigned c=0;c<3;++c) put_be32(p+0x0c+c*4u,vel[c]);
                put_be32(p+0x18,scalar);
                put_be32(p+0x1c,add(age,dt));
            }
            const uint32_t q0=from_i8(p[0x20]);
            const bool special=(flags&4u)!=0;
            const uint32_t duration=madd(mul(be32(object+(special?0x314:0x328)),0x3c010204u),q0,
                                         be32(object+(special?0x310:0x324)));
            uint32_t u=0;
            if (gt(duration,0)) {
                const uint32_t r=reciprocal(duration);
                const uint32_t t=mul(be32(p+0x1c),r);
                u= madd(nms(t,duration,be32(p+0x1c)),r,t);
                u=clamp01(u);
            }
            if (gt(u,0) && (be32(object+0x348)&2u)) {
                const uint32_t v=sub(0x3f800000u,u);
                u=nms(v,mul(v,v),0x3f800000u);
            }
            const uint32_t radius=madd(mul(curve(object,u,true),0x3c010204u),q0,
                                       curve(object,u,false));
            for (unsigned c=0;c<3;++c) {
                const uint32_t position=be32(p+c*4u);
                const uint32_t low=sub(position,radius),high=add(radius,position);
                if (gt(min[c],low)) min[c]=low;
                if (gt(high,max[c])) max[c]=high;
            }
        } else if (age_on_skip) {
            put_be32(p+0x1c,add(age,dt));
        }
    }
    for (unsigned c=0;c<3;++c) { put_be32(object+0x3c0+c*4u,min[c]); put_be32(object+0x3cc+c*4u,max[c]); }
    /* Commit only the two original DMA output ranges. GPRs and dead loop
     * scratch remain untouched; the wrapper resumes the original state gate. */
    std::memcpy(ctx->ls+object_ea,object,sizeof object);
    std::memcpy(ctx->ls+kParticles,particles,particle_bytes);
    return true;
}
