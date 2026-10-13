#include "haze_menu.h"
#include "haze_graphics.h"
#include <algorithm>
#include <cstdio>

namespace {
haze_video::Values edited;
HazeGraphicsState loaded;
HazeMenuModal video_dialog;
unsigned video_revision=0,preview_revision=0;
void video_result(const std::string& error={}) {
    loaded=haze_graphics_state(); edited=loaded.configured; preview_revision=0;
    video_dialog={++video_revision,"Video settings",error.empty() ? loaded.notice : error,{"OK"},false,0};
}
void video_status() {
    const auto current=haze_graphics_state();
    if (!preview_revision) return;
    if (current.display_preview!=preview_revision) { video_result(); return; }
    video_dialog.title=current.display_restore_failed ? "Restore display" : "Keep display changes?";
    video_dialog.message=current.display_restore_failed ? current.notice :
        "Reverting in "+std::to_string(current.display_seconds)+" seconds.\n\nChoose Keep to save all changes, or Revert to restore the previous settings.";
    auto choices=current.display_restore_failed ? std::vector<std::string>{"Retry revert"} : std::vector<std::string>{"Revert","Keep"};
    if (choices!=video_dialog.choices) video_dialog.revision=++video_revision;
    video_dialog.choices=std::move(choices);
    video_dialog.cancel_choice=0;
}
void video_choose(int choice) {
    if (!preview_revision) { video_dialog={}; return; }
    const auto current=haze_graphics_state();
    if (current.display_preview!=preview_revision) { video_result(); return; }
    std::string error;
    const bool keep=choice==1 && !current.display_restore_failed;
    const bool ok=haze_graphics_confirm(preview_revision,keep,error);
    if (haze_graphics_state().display_preview) { video_status(); return; }
    video_result(ok ? std::string{} : error);
}
void video_update(bool focused) {
    if (preview_revision && !focused && haze_graphics_state().display_preview==preview_revision) video_choose(0);
    video_status();
}
HazeMenuRow choices(const char* key,const char* label,std::vector<HazeMenuChoice> values,const char* hint) {
    HazeMenuRow row{key,label,edited.at(key),hint,std::move(values)};
    if (std::string(key)=="render_resolution" || std::string(key)=="shadow_resolution") row.hint+=" Restart required.";
    if (std::string(key)=="display_mode" || std::string(key)=="monitor" || std::string(key)=="window_size") row.hint+=" Apply previews this change for 15 seconds; Keep saves it.";
    if (loaded.overrides.count(key)) row.hint+=" Your launch options currently set this to "+loaded.effective.at(key)+".";
    else if (std::find(loaded.restart.begin(),loaded.restart.end(),key)!=loaded.restart.end())
        row.hint+=" Currently using "+loaded.effective.at(key)+".";
    if (std::none_of(row.choices.begin(),row.choices.end(),[&](const auto& c){return c.value==row.value;}))
        row.choices.push_back({row.value,row.value});
    return row;
}
HazeMenuRow slider(const char* key,const char* label,double lo,double hi,double step,const char* hint) {
    auto row=choices(key,label,{},hint); row.slider=true;
    row.minimum=lo; row.maximum=hi; row.step=step; return row;
}
std::vector<HazeMenuRow> rows() {
    auto quality=HazeMenuRow{"quality","Quality preset",haze_video::preset_name(edited),
        "Presets keep your display, frame limit and brightness preferences. Shadow changes need a restart.",
        {{"Low","Low"},{"Medium","Medium"},{"High","High"},{"Custom","Custom"}}};
    std::vector<HazeMenuChoice> monitors{{"primary","Primary display"}};
    for (int i=1;i<=16;++i) monitors.push_back({std::to_string(i),"Display "+std::to_string(i)});
    return {quality,
        choices("display_mode","Display mode",{{"borderless","Borderless"},{"windowed","Windowed"},{"fullscreen","Fullscreen"}},"Choose how Haze uses your display."),
        choices("monitor","Display",std::move(monitors),"Unavailable displays are rejected when you apply."),
        choices("window_size","Window size",{{"auto","Automatic"},{"960x540","960 x 540"},{"1280x720","1280 x 720"},{"1920x1080","1920 x 1080"},{"2560x1440","2560 x 1440"}},"Used in windowed mode."),
        choices("render_resolution","Render resolution",{{"auto","Desktop resolution"},{"native","Original game resolution"},{"1280x720","1280 x 720"},{"1920x1080","1920 x 1080"},{"2560x1440","2560 x 1440"},{"3840x2160","3840 x 2160"}},"Higher resolutions use more GPU memory. The game's original aspect policy is retained."),
        choices("vsync","VSync",{{"auto","Automatic"},{"on","On"},{"off","Off"}},"Synchronize presentation with your display."),
        choices("frame_limit","Frame limit",{{"30","30 FPS"},{"60","60 FPS"},{"90","90 FPS"},{"120","120 FPS"},{"144","144 FPS"},{"165","165 FPS"},{"240","240 FPS"},{"0","Unlimited"}},"Gameplay timing above 30 FPS is experimental."),
        choices("anti_aliasing","Anti-aliasing",{{"off","Off"},{"fxaa","FXAA"}},"Smooth final-image edges. FXAA can soften text."),
        choices("anisotropic_filtering","Texture filtering",{{"1","Game default"},{"2","2x"},{"4","4x"},{"8","8x"},{"16","16x"}},"Improve filtered textures viewed at an angle."),
        choices("shadow_resolution","Shadow resolution",{{"native","Game default (2048)"},{"follow","Follow render resolution"},{"1024","1024"},{"2048","2048"},{"4096","4096"},{"8192","8192"}},"Larger shadow maps use more memory; existing shadow artifacts may remain."),
        slider("mip_lod_bias","Texture detail bias",-2,2,.1,"Negative values sharpen mip selection and may shimmer."),
        slider("sharpening","Sharpening",0,1,.05,"Increase final-image detail contrast."),
        slider("exposure","Brightness",-2,2,.1,"Adjust image brightness in exposure stops, including the HUD.")};
}
}
void haze_video_menu_register() {
    HazeMenuProvider provider;
    provider.title="VIDEO";
    provider.begin=[] {
        loaded=haze_graphics_state(); edited=loaded.configured; video_dialog={}; preview_revision=loaded.display_preview;
        if (preview_revision) { video_dialog.revision=++video_revision; video_status(); }
    };
    provider.rows=rows;
    provider.change=[](const std::string& key,const std::string& value) {
        if (video_dialog.revision) return;
        if (key=="quality") haze_video::preset(edited,value); else edited[key]=value;
    };
    provider.defaults=[] { if (!video_dialog.revision) edited=haze_video::defaults(); };
    provider.apply=[](std::string& message) {
        const bool ok=haze_graphics_preview(haze_menu_owner(),edited,true,message);
        const auto current=haze_graphics_state();
        if (current.display_preview) {
            preview_revision=current.display_preview; video_dialog.revision=++video_revision; video_status();
            if (ok) message="Review the display preview. Revert is selected by default.";
            return ok ? HazeMenuApply::Applied : HazeMenuApply::Failed;
        }
        if (!ok) return HazeMenuApply::Failed;
        loaded=haze_graphics_state(); message=loaded.notice+". Settings saved."; return HazeMenuApply::Applied;
    };
    provider.update=video_update;
    provider.modal=[] { return video_dialog; };
    provider.modal_choose=video_choose;
    haze_menu_register(HazeMenuPage::Video,std::move(provider));
}
