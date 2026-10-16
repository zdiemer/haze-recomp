#include "haze_coop_settings.h"
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <regex>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
std::mutex settings_mutex;
std::filesystem::path settings_file;
std::string server_address, settings_error;
bool local_test_enabled() {
    const auto value=std::getenv("HAZE_COOP_LOCAL_TEST");
    return value && std::string_view(value)=="1";
}
}
bool haze_coop_server_normalize(const std::string& input,bool local_test,std::string& endpoint,std::string& error) {
    auto value=input;
    const auto first=value.find_first_not_of(" \t\r\n"), last=value.find_last_not_of(" \t\r\n");
    value=first==std::string::npos ? "":value.substr(first,last-first+1);
    endpoint.clear();
    if(value.empty()) { error="Enter a server address first."; return false; }
    if(value.size()>240) { error="Use a server address of at most 240 characters."; return false; }
    if(value.find("://")==std::string::npos) value="wss://"+value;
    // Accept a hostname/IP with an optional port and reverse-proxy path.
    // Credentials, fragments, whitespace and query strings are not addresses.
    static const std::regex address(R"((wss|ws)://([A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?|\[[A-Fa-f0-9:]+\])(?::([0-9]{1,5}))?(/[A-Za-z0-9/_.~-]*)?)");
    std::smatch match;
    if(!std::regex_match(value,match,address)) {
        error="Use a hostname, hostname:port, or wss://hostname/v1."; return false;
    }
    if(match[3].matched && (std::stoul(match[3])==0 || std::stoul(match[3])>65535)) {
        error="The server port must be between 1 and 65535."; return false;
    }
    const bool insecure=match[1]=="ws";
    if(!match[4].matched || match[4]=="/") {
        if(match[4].matched) value.pop_back();
        value+="/v1";
    }
    if(insecure && !(local_test && std::regex_match(value,std::regex(R"(ws://127\.0\.0\.1:[0-9]{1,5}/[A-Za-z0-9/_-]*)")))) {
        error="Use a secure server address (wss://)."; return false;
    }
    if(value.size()>240) { error="Use a server address of at most 240 characters."; return false; }
    endpoint=std::move(value); error.clear(); return true;
}
void haze_coop_settings_initialize(const std::filesystem::path& save_root) {
    std::lock_guard lock(settings_mutex);
    settings_file=save_root/"co-op-server.txt"; server_address.clear(); settings_error.clear();
    std::error_code ec;
    if(!std::filesystem::exists(settings_file,ec) && !ec) return;
    const auto size=std::filesystem::file_size(settings_file,ec);
    if(ec || size>242) { settings_error="Cannot read the saved server. Enter its address again."; return; }
    std::ifstream file(settings_file,std::ios::binary);
    std::string value(size_t(size),'\0');
    if(!file.read(value.data(),std::streamsize(size))) { settings_error="Cannot read the saved server. Enter its address again."; return; }
    haze_coop_server_normalize(value,local_test_enabled(),server_address,settings_error);
}
std::string haze_coop_server() { std::lock_guard lock(settings_mutex); return server_address; }
std::string haze_coop_settings_error() { std::lock_guard lock(settings_mutex); return settings_error; }
bool haze_coop_server_save(const std::string& input,std::string& error) {
    std::string endpoint;
    if(!haze_coop_server_normalize(input,local_test_enabled(),endpoint,error)) return false;
    std::lock_guard lock(settings_mutex);
    if(settings_file.empty()) { error="Server settings are not ready."; return false; }
    std::error_code ec;
    std::filesystem::create_directories(settings_file.parent_path(),ec);
    if(ec) { error="Cannot create the settings folder. Server unchanged."; return false; }
    const auto temporary=settings_file.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) { error="Cannot write the server settings. Server unchanged."; return false; }
    const auto bytes=endpoint+"\n"; DWORD written=0;
    bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(file);
    if(!CloseHandle(file)) ok=false;
    if(ok) ok=MoveFileExW(temporary.c_str(),settings_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) { DeleteFileW(temporary.c_str()); error="Cannot save the server settings. Server unchanged."; return false; }
    server_address=std::move(endpoint); settings_error.clear(); error.clear(); return true;
}
