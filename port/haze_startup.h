#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

// Drive the guest skip path only while a stream owns playback.
// Opening branding and the idle attract demo have independent policies. Story
// movies never acquire automatic ownership. Static startup slides use the same
// input gate, but advance their presentation timers without synthetic buttons.
class HazeStartupSkip {
    std::mutex mutex;
    bool active = false, logo = false, attract = false, draining = false;
    bool auto_logos = false, auto_attract = false;
    uint32_t owner = 0;
    uint64_t started = 0;
    bool static_splash = false, manual_splash = false;
    int slide = -1;
public:
    static const char* video_file(const char* uri) {
        const char* prefix = "x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/";
        return uri && !std::strncmp(uri,prefix,std::strlen(prefix)) ? uri+std::strlen(prefix) : nullptr;
    }
    static bool opening_logo(const char* uri) {
        const char* file=video_file(uri);
        return file && (!std::strcmp(file,"ubi.pam") || !std::strcmp(file,"ubi43.pam") ||
                        !std::strcmp(file,"frd.pam") || !std::strcmp(file,"frd43.pam"));
    }
    static bool known_video(const char* uri) {
        const char* file=video_file(uri);
        return opening_logo(uri) || (file && !std::strcmp(file,"attract.pam"));
    }
    void configure(bool logos, bool demo) {
        std::lock_guard lock(mutex);
        auto_logos=logos; auto_attract=demo;
    }
    bool playback(uint32_t player, const char* uri, bool playing, uint64_t now) {
        std::lock_guard lock(mutex);
        // A new stream (even on another player) supersedes logo ownership.
        // An unrelated STOP must not cancel the stream currently being skipped.
        if (active && ((!static_splash && owner == player) || playing)) {
            active=false;
            draining=true;
        }
        if (!playing || !uri || !*uri) return false;
        owner=player; started=now; active=true; logo=opening_logo(uri);
        attract=known_video(uri) && !logo;
        static_splash=false;
        return true;
    }
    bool splash(uint32_t scene, int screen, uint64_t now) {
        std::lock_guard lock(mutex);
        if (!active || !static_splash || owner!=scene) {
            owner=scene; started=now; active=true; static_splash=true;
            slide=screen; manual_splash=false;
        } else if (slide!=screen) {
            slide=screen; manual_splash=false;
        }
        return auto_logos || manual_splash;
    }
    void splash_end(uint32_t scene) {
        std::lock_guard lock(mutex);
        if (active && static_splash && owner==scene) {
            active=false; draining=true;
        }
    }
    bool owns_input() {
        std::lock_guard lock(mutex);
        return active || draining;
    }
    bool showing() {
        std::lock_guard lock(mutex);
        return active;
    }
    // Supply merged physical mapped buttons, before adding synthetic input.
    // Consume all buttons during playback and until a focused neutral report
    // after its end, including a key bound to both Cross and Start.
    uint16_t filter(uint64_t now, uint16_t buttons, bool focused, uint16_t confirm) {
        std::lock_guard lock(mutex);
        if (!active) {
            if (!draining) return buttons;
            if (focused && !buttons) draining=false;
            return 0;
        }
        const uint64_t elapsed=now>=started ? now-started : 0;
        if (static_splash) {
            manual_splash |= focused && (buttons & confirm);
            return 0;
        }
        // Other streams accept manual confirm, subject to the guest's own
        // skippable flag, but never inherit a branding/idle-demo auto policy.
        const bool automatic=logo ? auto_logos : attract && auto_attract;
        // Keep the original start-completion grace period. Slow guest frames
        // get repeated edges until STOP/CLOSE/destroy, with no timeout that
        // could strand a still-owned stream after slow loading.
        const bool pulse=elapsed>=300 && (elapsed-300)%700<400;
        const bool manual=focused && (buttons & confirm) && elapsed>=300;
        return (automatic && pulse) || manual ? confirm : 0;
    }
};
