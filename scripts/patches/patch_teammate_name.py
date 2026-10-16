"""Use the resolved network display name for the campaign teammate label."""
import hashlib

ENTRY = 'void func_0062A280(ppu_context* ctx) {'
DIGEST = '40203ca1ce255b12d58ecec5f5bc762c6f20591ea7c6c638f229b130cf13fdab'
SITE = 'loc_0062A8A4:'
# The retail label uses the viewed character's stale NAME property. r28 is that
# character; r29 is the network manager. Match the character's full entity
# handle (+44) to a player's entity handle (+160, also read by 004D1BC8).
# The lookup immediately above this site concerns the LOCAL voice indicator;
# its r3 result must not supply the teammate name. The manager has 32 slots
# (+c0, 004D11F0); each player's display name is at +16a (004D81E0).
# The label's +35 buffer holds 64 bytes (00379EA8). Preserve registers,
# identity keys, character properties and the original non-player fallback.
COPY = '''
        {
            const uint32_t haze_label = vm_read32(ctx->gpr[31] + 0x154);
            const uint32_t haze_actor = uint32_t(ctx->gpr[28]);
            const uint32_t haze_manager = uint32_t(ctx->gpr[29]);
            const uint32_t haze_entity = haze_actor ? vm_read32(haze_actor + 0x44) : 0;
            uint32_t haze_name = 0;
            if (haze_label && haze_entity && haze_manager) {
                for (unsigned haze_slot = 0; haze_slot < 32; ++haze_slot) {
                    const uint32_t haze_player = vm_read32(haze_manager + 0xc0 + haze_slot * 4);
                    if (haze_player && vm_read32(haze_player + 0x160) == haze_entity) {
                        haze_name = haze_player + 0x16a;
                        break;
                    }
                }
            }
            if (haze_name && vm_read8(haze_name)) {
                unsigned haze_i = 0;
                for (; haze_i < 63; ++haze_i) {
                    const auto haze_c = vm_read8(haze_name + haze_i);
                    vm_write8(haze_label + 0x35 + haze_i, haze_c);
                    if (!haze_c) break;
                }
                vm_write8(haze_label + 0x35 + haze_i, 0);
            }
        }'''


def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one campaign teammate label update')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    body = source[start:end]
    if hashlib.sha256(body.encode()).hexdigest() != DIGEST or body.count(SITE) != 1:
        raise ValueError('Teammate label update changed; audit before patching')
    return source[:start] + body.replace(SITE, SITE + COPY, 1) + source[end:]
