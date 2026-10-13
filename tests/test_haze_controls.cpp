#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <cstdio>
static bool held[256]{};
static SHORT key_state(int key) { return held[key] ? SHORT(0x8000) : 0; }
#define GetAsyncKeyState key_state
#include "../port/haze_controls_settings.cpp"
#include "../port/haze_controls_menu.cpp"
static HazeMenuProvider registered;
static std::string path;
static unsigned reloads;
static HazeBindingCapture gamepad_capture;
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider) { assert(page==HazeMenuPage::Controls); registered=std::move(provider); }
void haze_menu_retail() {}
const char* haze_input_config_path() { return path.c_str(); }
void haze_input_reload() { ++reloads; }
unsigned haze_input_menu_button(int) { return 0; }
HazeInputFamily haze_input_family() { return HazeInputFamily::Keyboard; }
HazeControllerInfo haze_gamepad_info() { return {"Fixture controller",true,true}; }
void haze_gamepad_capture_begin(bool) { gamepad_capture={HazeBindingCapturePhase::Release,{},"Release"}; }
void haze_gamepad_capture_cancel() { gamepad_capture={HazeBindingCapturePhase::Cancelled,{},"Cancelled"}; }
HazeBindingCapture haze_gamepad_capture() { return gamepad_capture; }
static std::string bytes() { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
static std::string value(const std::string& key) {
    for (const auto& row:registered.rows()) if (row.key==key) return row.value;
    assert(false); return {};
}
int main() {
    path=std::filesystem::absolute("local/controls-test-"+std::to_string(GetCurrentProcessId())+".ini").string();
    { std::ofstream file(path); file<<"; Keep this comment\n[Bindings]\nForward=W\n[Startup]\nAutoSkipLogos=0\n[Prompts]\nDirectory=../my glyphs\n[Extra]\nValue=keep me\n"; }
    const auto original=bytes();
    HazeControlDocument initial; std::string message;
    assert(haze_control_load(path,initial,message));
    auto invalid=initial.values; invalid["Mouse.Sensitivity"]="nan"; assert(!haze_control_save(initial,invalid,message) && bytes()==original);
    invalid=initial.values; invalid["Controller.MoveX"]="raw:b0"; assert(!haze_control_validate(invalid,message));
    invalid=initial.values; invalid["Bindings.Cross"]="F1"; assert(!haze_control_validate(invalid,message));
    invalid=initial.values; invalid["ControllerBindings.Cross"]="a";
    assert(haze_control_conflicts(invalid,"ControllerBindings.Circle","south").size()==1);
    haze_controls_menu_register(); registered.begin();
    held['W']=true; registered.activate("Bindings.Forward"); registered.update(true); registered.modal_key('P');
    assert(registered.modal().capturing && value("Bindings.Forward")=="W");
    held['W']=false; registered.update(true); registered.modal_key('S');
    assert(!registered.modal().capturing); registered.modal_choose(0);
    assert(registered.modal().title=="Binding conflict"); registered.modal_choose(1);
    assert(value("Bindings.Forward")=="S" && value("Bindings.Backward")=="Unbound" && bytes()==original);
    registered.activate("Bindings.Forward"); registered.update(true); registered.update(false);
    assert(!registered.modal().revision && value("Bindings.Forward")=="S");
    registered.activate("ControllerBindings.Cross");
    gamepad_capture={HazeBindingCapturePhase::Captured,"raw:h0.4","Captured"}; registered.update(true);
    registered.modal_choose(0); assert(value("ControllerBindings.Cross")=="Hat 1 Down");
    const auto bad_mapping=path+".mapping.txt";
    { std::ofstream file(bad_mapping); file<<"This is not an SDL mapping\n"; }
    registered.change("Controller.MappingFile",bad_mapping);
    assert(registered.apply(message)==HazeMenuApply::Failed && reloads==0 && bytes()==original);
    registered.activate("Controller.MappingFile"); registered.modal_choose(1);
    assert(edited.at("Controller.MappingFile").empty()); std::filesystem::remove(bad_mapping);
    registered.activate("ControllerBindings.Circle");
    gamepad_capture={HazeBindingCapturePhase::Cancelled,{},"Disconnected"}; registered.update(true);
    assert(!registered.modal().revision && value("ControllerBindings.Circle")=="East face button");
    HANDLE lock=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); assert(lock!=INVALID_HANDLE_VALUE);
    assert(registered.apply(message)==HazeMenuApply::Failed && reloads==0 && bytes()==original); CloseHandle(lock);
    assert(registered.apply(message)==HazeMenuApply::Applied && reloads==1);
    HazeControlDocument saved; assert(haze_control_load(path,saved,message));
    assert(saved.values.at("Bindings.Forward")=="S" && saved.values.at("ControllerBindings.Cross")=="raw:h0.4");
    auto contents=bytes(); assert(contents.find("AutoSkipLogos=0")!=std::string::npos && contents.find("Directory=../my glyphs")!=std::string::npos && contents.find("Value=keep me")!=std::string::npos);
    registered.defaults(); assert(value("Bindings.Forward")=="W"); registered.begin(); assert(value("Bindings.Forward")=="S");
    { std::ofstream file(path,std::ios::app); file<<"; external edit\n"; }
    const auto external=bytes(); assert(registered.apply(message)==HazeMenuApply::Failed && bytes()==external && reloads==1);
    { std::ofstream file(path); file<<"[Bindings]\nForward=A\nforward=B\n"; }
    assert(!haze_control_load(path,saved,message));
    std::filesystem::remove(path);
    std::puts("Controls: capture, conflict resolution, focus/disconnect cancellation, staged defaults, atomic persistence and external edit rejection passed.");
}
