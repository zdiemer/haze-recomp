#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstdlib>
extern "C" __declspec(dllimport) int haze_launcher_fixture_runtime();
static std::string utf8(const wchar_t* text) {
    const int size=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    std::string result(size,'\0'); WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),size,nullptr,nullptr); result.pop_back(); return result;
}
int wmain(int argc,wchar_t** argv) {
    if (haze_launcher_fixture_runtime()!=42) return 99;
    std::printf("ARGC=%d\nNO_CONSOLE=%d\n",argc,GetConsoleWindow()==nullptr);
    for (int i=0;i<argc;++i) std::printf("ARG%d=%s\n",i,utf8(argv[i]).c_str());
    wchar_t value[32768]{}; GetCurrentDirectoryW(32768,value); std::printf("CWD=%s\n",utf8(value).c_str());
    const wchar_t* variables[]={L"PS3_VFS_ROOT",L"PS3_DEV_FLASH",L"PS3_SAVEDATA_ROOT",L"RSX_SHADER_CACHE_DIR",L"HAZE_INPUT_CONFIG",L"HAZE_GRAPHICS_CONFIG",L"HAZE_INPUT",L"RSX_RENDER_RESOLUTION",L"HAZE_LAUNCHER_PATH",L"HAZE_LAUNCHER_CONFIG",L"PS3_NET_ONLINE",L"YDKJ_NO_CRI_CTX",L"PS3_HLE_UNRESOLVED",L"PS3_VERBOSE",L"RSX_LIVE_DRAW"};
    for (const auto key:variables) { value[0]=0; GetEnvironmentVariableW(key,value,32768); std::printf("%s=%s\n",utf8(key).c_str(),utf8(value).c_str()); }
    std::fprintf(stderr,"Fixture stderr: native failure diagnostics remain available.\n");
    value[0]=0; GetEnvironmentVariableW(L"HAZE_TEST_SENTINEL",value,32768);
    if (*value) SetEvent(reinterpret_cast<HANDLE>(uintptr_t(_wcstoui64(value,nullptr,10))));
    value[0]=0; GetEnvironmentVariableW(L"HAZE_TEST_HOLD_MS",value,32768); if (*value) Sleep(wcstoul(value,nullptr,10));
    value[0]=0; GetEnvironmentVariableW(L"HAZE_TEST_EXIT",value,32768); return *value ? int(wcstoul(value,nullptr,10)) : 0;
}
