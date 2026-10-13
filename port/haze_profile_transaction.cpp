#include "haze_profile_transaction.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <string>
#include <atomic>
#include <cstdio>
#include <memory>

namespace {
namespace fs=std::filesystem;
constexpr int32_t access_error=int32_t(0x8002b402u);
constexpr uint32_t limit=16*1024*1024;
std::atomic<unsigned> sequence{0};
bool read(const fs::path& path,std::string& bytes) {
    const auto attributes=GetFileAttributesW(path.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; DWORD got=0;
    bool ok=GetFileSizeEx(file,&size) && size.QuadPart>=0 && size.QuadPart<=limit;
    try {
        if (ok) { bytes.resize(size_t(size.QuadPart)); ok=ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr) && got==bytes.size(); }
    } catch (...) { CloseHandle(file); throw; }
    if (!CloseHandle(file)) ok=false;
    return ok;
}
struct Transaction {
    fs::path directory,temporary,profile;
    std::string before,metadata,temporary_utf8;
    HANDLE lock=INVALID_HANDLE_VALUE;
    bool staged=false,metadata_requested=false,failed=false;
    ~Transaction() {
        std::error_code ignored;
        if (!temporary.empty()) {
            fs::remove(temporary/L"_PROFILE.DAT",ignored);
            fs::remove(temporary/L"PARAM.SFO",ignored);
            fs::remove(temporary,ignored);
        }
        if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    }
};
int32_t failure(const char* reason) {
    std::fprintf(stderr,"[audio-profile] %s Previous profile retained.\n",reason); return access_error;
}
}
extern "C" void* haze_profile_begin(const char* directory) {
    try {
        if (!directory || !*directory) return nullptr;
        auto tx=std::make_unique<Transaction>(); tx->directory=fs::u8path(directory);
        tx->profile=tx->directory/L"_PROFILE.DAT";
        const auto lock_path=tx->directory.parent_path()/(L".haze-audio-"+tx->directory.filename().wstring()+L".lock");
        tx->lock=CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if (tx->lock==INVALID_HANDLE_VALUE || !read(tx->profile,tx->before) || !read(tx->directory/L"PARAM.SFO",tx->metadata)) {
            failure("Cannot lock or read the existing audio profile."); return nullptr;
        }
        for (int attempt=0;attempt<16;++attempt) {
            auto path=tx->directory.parent_path()/(L".haze-audio-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence));
            if (CreateDirectoryW(path.c_str(),nullptr)) { tx->temporary=std::move(path); break; }
            if (GetLastError()!=ERROR_ALREADY_EXISTS) break;
        }
        if (tx->temporary.empty()) { failure("Cannot stage audio settings."); return nullptr; }
        const auto utf8=tx->temporary.u8string(); tx->temporary_utf8.assign(utf8.begin(),utf8.end());
        return tx.release();
    } catch (...) { failure("Cannot prepare audio settings storage."); return nullptr; }
}
extern "C" const char* haze_profile_metadata_directory(void* transaction) {
    auto& tx=*static_cast<Transaction*>(transaction); tx.metadata_requested=true;
    return tx.temporary_utf8.c_str();
}
extern "C" int32_t haze_profile_stage(void* transaction,const void* bytes,uint32_t size) {
    auto& tx=*static_cast<Transaction*>(transaction);
    if (tx.staged || tx.failed || !bytes || !size || size>limit) {
        tx.failed=true; return failure("Unexpected audio profile write.");
    }
    try {
        const auto path=tx.temporary/L"_PROFILE.DAT";
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        DWORD written=0;
        bool ok=file!=INVALID_HANDLE_VALUE;
        if (ok) {
            ok=WriteFile(file,bytes,size,&written,nullptr) && written==size && FlushFileBuffers(file);
            if (!CloseHandle(file)) ok=false;
        }
        if (!ok) { tx.failed=true; return failure("Could not write and flush staged audio settings."); }
        tx.staged=true; return int32_t(size);
    } catch (...) { tx.failed=true; return failure("Could not stage audio settings."); }
}
extern "C" int32_t haze_profile_finish(void* transaction,int32_t callback_result) {
    std::unique_ptr<Transaction> tx(static_cast<Transaction*>(transaction));
    if (callback_result) return callback_result;
    try {
        if (!tx->staged || tx->failed) return failure("The audio save did not produce a complete profile.");
        std::string current;
        if (tx->metadata_requested && (!read(tx->temporary/L"PARAM.SFO",current) || current!=tx->metadata))
            return failure("Audio save requested an unsupported metadata change.");
        if (!read(tx->profile,current) || current!=tx->before ||
            !read(tx->directory/L"PARAM.SFO",current) || current!=tx->metadata)
            return failure("The profile changed or became unavailable during saving.");
        if (!MoveFileExW((tx->temporary/L"_PROFILE.DAT").c_str(),tx->profile.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            return failure("Could not replace the audio profile.");
        std::fprintf(stderr,"[audio-profile] Profile committed after successful save callbacks.\n");
        return 0;
    } catch (...) { return failure("Could not commit the audio profile."); }
}
