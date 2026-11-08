/* The pinned SDK save path, patched exactly as the game build patches it, with
 * guest callbacks over real files: a multi-file save interrupted by the guest
 * keeps the previous checkpoint, and a truncated save loads from its backup. */
#include "cellSaveData.c" /* scripts/patch.py savedata output */
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; abort(); }
ps3_guest_caller_fn g_ps3_guest_caller;
int haze_save_is_settings(void) { return 0; }
#define DATA_SIZE 50000u
#define EXTRA_SIZE 1000u
static unsigned step;
static int loading, interrupt;

static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == 0x800) {
        if (!loading) {
            strcpy((char*)vm_base + get + 64, "Backup test");
            vm_write32(set, (u32)get + 64);
        }
        return;
    }
    if (step == 2 || (step == 1 && interrupt)) {
        vm_write32(cb, step == 2 ? CELL_SAVEDATA_CBRESULT_OK_LAST : (u32)CELL_SAVEDATA_CBRESULT_ERR_FAILURE);
        return;
    }
    vm_write32(set, loading ? CELL_SAVEDATA_FILEOP_READ : CELL_SAVEDATA_FILEOP_WRITE);
    vm_write32(set + 8, step ? CELL_SAVEDATA_FILETYPE_NORMALFILE : CELL_SAVEDATA_FILETYPE_SECUREFILE);
    vm_write32(set + 28, step ? 0x420 : 0x400);
    vm_write32(set + 36, step ? EXTRA_SIZE : DATA_SIZE);
    vm_write32(set + 40, step ? EXTRA_SIZE : DATA_SIZE);
    vm_write32(set + 44, (step ? 0x30000 : 0x10000) + (loading ? 0x40000 : 0));
    ++step;
}

static int run(int saving)
{
    step = 0; loading = !saving;
    return saving ? cellSaveDataAutoSave2(0, (void*)0x300, 0, (void*)0x200, (void*)0x800, (void*)0x900, 0, NULL)
                  : cellSaveDataAutoLoad2(0, (void*)0x300, 0, (void*)0x200, (void*)0x800, (void*)0x900, 0, NULL);
}

static long file_size(const char* directory, const char* name, int* first)
{
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    *first = fgetc(fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    return size;
}

static void fill(char data, char extra)
{
    memset(vm_base + 0x10000, data, DATA_SIZE);
    memset(vm_base + 0x30000, extra, EXTRA_SIZE);
}

static void expect(const char* save, char data, char extra)
{
    int first = 0;
    assert(file_size(save, "DATA.BIN", &first) == DATA_SIZE && first == data);
    assert(file_size(save, "EXTRA.BIN", &first) == EXTRA_SIZE && first == extra);
    assert(file_size(save, "PARAM.SFO", &first) > 0 && file_size(save, "PARAM.PFD", &first) > 0);
}

static void rmtree(const char* path)
{
    char search[1400], child[1400];
    snprintf(search, sizeof(search), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(search, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(child);
            else assert(DeleteFileA(child));
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    assert(RemoveDirectoryA(path));
}

int main(void)
{
    char temp[MAX_PATH], root[1200], save[1300], path[1400];
    assert(GetTempPathA(sizeof(temp), temp));
    snprintf(root, sizeof(root), "%shaze-savedata-backup-%lu", temp, GetCurrentProcessId());
    assert(_mkdir(root) == 0);
    assert(cellSaveData_set_root_path(root) == CELL_OK);
    snprintf(save, sizeof(save), "%s\\TEST00000", root);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    strcpy((char*)vm_base + 0x300, "TEST00000");
    strcpy((char*)vm_base + 0x400, "DATA.BIN");
    strcpy((char*)vm_base + 0x420, "EXTRA.BIN");
    vm_write32(0x204, 4);
    g_ps3_guest_caller = callback;
    cellSaveData_set_strict_load_errors(1);

    fill('A', 'a'); assert(run(1) == CELL_OK); expect(save, 'A', 'a');
    /* The backup directory is invisible to the guest's directory list. */
    assert(enumerate_save_dirs("", NULL, 0) == 1);
    snprintf(path, sizeof(path), "%s\\.haze-saves\\TEST00000\\backup.1", root);
    expect(path, 'A', 'a');
    /* The guest fails after the first of two payload files: nothing changes. */
    fill('B', 'b'); interrupt = 1;
    assert(run(1) == CELL_SAVEDATA_ERROR_CBRESULT); expect(save, 'A', 'a');
    interrupt = 0; assert(run(1) == CELL_OK); expect(save, 'B', 'b');
    /* Truncate the committed payload; the next load reads the verified copy. */
    snprintf(path, sizeof(path), "%s\\DATA.BIN", save);
    FILE* fp = fopen(path, "wb"); assert(fp); fputs("BB", fp); fclose(fp);
    memset(vm_base + 0x50000, 0, DATA_SIZE); memset(vm_base + 0x70000, 0, EXTRA_SIZE);
    assert(run(0) == CELL_OK); expect(save, 'B', 'b');
    assert(!memcmp(vm_base + 0x50000, vm_base + 0x10000, DATA_SIZE));
    assert(!memcmp(vm_base + 0x70000, vm_base + 0x30000, EXTRA_SIZE));
    snprintf(path, sizeof(path), "%s\\.haze-saves\\TEST00000\\damaged.1", root);
    int first = 0; assert(file_size(path, "DATA.BIN", &first) == 2);
    /* A missing save directory comes back from its backup on boot recovery. */
    rmtree(save); haze_backup_recover_all(root); expect(save, 'B', 'b');
    rmtree(root); free(vm_base);
    puts("Patched SDK saves: multi-file interruption, guest-invisible backups, truncated load and boot recovery passed.");
}
