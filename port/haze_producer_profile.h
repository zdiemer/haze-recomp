#pragma once

#include <cstdint>

namespace haze_producer_profile {

// A bounded, opt-in QPC interval record. Frame scopes are accepted only for
// guest thread 1 with the audited PPU TOC. Nested scopes inherit their active
// TLS parent unless a parent is supplied explicitly. An explicit nonzero
// generation overrides inheritance; default zero inherits the parent value.
// Scope lifetimes must begin and end on the same host thread.
class scope {
public:
    scope() noexcept = default;
    ~scope() noexcept;
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    scope(scope&&) = delete;
    scope& operator=(scope&&) = delete;

    bool active() const noexcept { return active_; }
    std::uint64_t id() const noexcept { return id_; }
    std::uint64_t generation() const noexcept { return generation_; }

protected:
    void begin(unsigned kind, const scope* parent, std::uint64_t generation,
        std::uint32_t guest_ea, std::uint32_t callsite_lr,
        std::uint32_t callee_ea) noexcept;

private:
    friend void haze_producer_profile_dump() noexcept;
    bool active_ = false;
    std::uint64_t id_ = 0;
    std::uint64_t generation_ = 0;
    scope* previous_ = nullptr;
    bool qpc_begin_valid_ = false;
};

class frame_scope final : public scope {
public:
    frame_scope(unsigned guest_thread, std::uint64_t toc,
        std::uint32_t guest_ea, std::uint64_t generation = 0,
        const scope* parent = nullptr) noexcept;
};

class call_scope final : public scope {
public:
    call_scope(const scope& parent, std::uint32_t callsite_lr,
        std::uint32_t callee_ea) noexcept;
    // Attach to the active scope on this thread. If no parent is active, this
    // constructor is a no-op and does not initialize configuration or clocks.
    call_scope(std::uint32_t callsite_lr, std::uint32_t callee_ea) noexcept;
};

// Call once from the normal shutdown path. The snapshot is frozen before file
// I/O; active scopes are reported as incomplete and are not waited on.
void haze_producer_profile_dump() noexcept;

} // namespace haze_producer_profile

