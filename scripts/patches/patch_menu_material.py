"""Draw the PC popup material at Haze's native container background boundary."""
import hashlib

ENTRY = 'void func_002F1538(ppu_context* ctx) {'
DIGEST = '1ed1049fd0798f32c8824ff52cedcd2e318dbf0fed723ff7acf168857faadd78'

def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one native container background function')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    original = source[start:end]
    if hashlib.sha256(original.encode()).hexdigest() != DIGEST:
        raise ValueError('Native container drawing changed; audit the material hook')
    wrapper = '''
void func_002F1538(ppu_context* ctx) {
    const uint32_t widget = uint32_t(ctx->gpr[3]);
    haze_original_002F1538(ctx);
    DRAIN_TRAMPOLINE(ctx);
    extern void haze_native_popup_material(ppu_context*,uint32_t);
    haze_native_popup_material(ctx,widget);
}
'''
    return source[:start] + original.replace('func_002F1538(', 'haze_original_002F1538(', 1) + wrapper + source[end:]
