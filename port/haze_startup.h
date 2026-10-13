#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

// Drive the guest skip path only while a recognized stream owns playback.
// Opening branding and the idle attract demo have independent policies. Story
// movies never acquire ownership. No guest initialization/save state is patched.
class HazeStartupSkip {
    std::mutex mutex;
    bool active = false, attract = false, draining = false;
    bool auto_logos = false, auto_attract = false;
    uint32_t owner = 0;
    uint64_t started = 0;
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
        if (active && (owner == player || playing)) {
            active=false;
            draining=true;
        }
        if (!playing || !known_video(uri)) return false;
        owner=player; started=now; active=true; attract=!opening_logo(uri);
        return true;
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
        const bool automatic=attract ? auto_attract : auto_logos;
        // Keep the original start-completion grace period. Slow guest frames
        // get repeated edges until STOP/CLOSE/destroy, with no timeout that
        // could strand a still-owned stream after slow loading.
        const bool pulse=elapsed>=300 && (elapsed-300)%700<400;
        const bool manual=focused && (buttons & confirm) && elapsed>=300;
        return (automatic && pulse) || manual ? confirm : 0;
    }
};
