#include "haze_audio_settings.h"
#include "haze_menu.h"
#include <charconv>

namespace {
HazeAudioState loaded;
HazeAudioValues edited;
const char* keys[]={"music","effects","dialogue","subtitles"};
HazeMenuApply result(std::string& message) {
    const auto state=haze_audio_state();
    message=state.message;
    if (state.phase==HazeAudioPhase::Queued || state.phase==HazeAudioPhase::Saving) return HazeMenuApply::Pending;
    if (state.phase==HazeAudioPhase::Applied) { loaded=state; edited=state.values; return HazeMenuApply::Applied; }
    return HazeMenuApply::Failed;
}
}
void haze_audio_menu_register() {
    HazeMenuProvider page;
    page.title="AUDIO";
    page.begin=[] { loaded=haze_audio_state(); edited=loaded.values; };
    page.defaults=[] { edited=loaded.defaults; };
    page.rows=[] {
        const char* labels[]={"Music","Sound effects","Dialogue"};
        std::vector<HazeMenuRow> rows;
        for (int i=0;i<3;++i)
            rows.push_back({keys[i],labels[i],std::to_string(edited[i]),
                "Apply saves to your game profile. Back discards edits since the last Apply.",{},true,0,100,5});
        rows.push_back({"subtitles","Subtitles",std::to_string(edited[3]),
            "Show subtitles during dialogue.",{{"0","Off"},{"1","On"}}});
        return rows;
    };
    page.change=[](const std::string& key,const std::string& value) {
        int amount=0; const auto parsed=std::from_chars(value.data(),value.data()+value.size(),amount);
        if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || amount<0 || amount>100) return;
        for (int i=0;i<4;++i) if (key==keys[i] && (i<3 || amount<=1)) edited[i]=amount;
    };
    page.apply=[](std::string& message) {
        return haze_audio_apply(edited,loaded.generation,message) ? HazeMenuApply::Pending : HazeMenuApply::Failed;
    };
    page.poll=result;
    haze_menu_register(HazeMenuPage::Audio,std::move(page));
}
