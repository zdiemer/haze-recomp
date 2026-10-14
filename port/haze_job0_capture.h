#ifndef HAZE_JOB0_CAPTURE_H
#define HAZE_JOB0_CAPTURE_H

// Diagnostic fixture files are written only when HAZE_JOB_TRACE is enabled,
// HAZE_JOB0_CAPTURE_DIR names an existing directory, and the file named by
// HAZE_JOB0_CAPTURE_GATE exists. Call input only for image 30 at PCs 0x3408,
// 0x36F0, or 0x3F48; dump is intended for shutdown after workers stop.
// Each .bin starts with a 56-byte little-endian header:
//   magic[8]="HZJ0CAP\0", version, header_bytes, pc, register_bytes,
//   local_store_bytes, host_tid, per_site_index, qpc, FNV-1a64(payload), reserved.
// The non-cryptographic hash covers raw GPR bytes followed by raw LS bytes.
// Raw host-native GPR bytes (128 * 16) and raw LS bytes (256 KiB) follow.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(HAZE_JOB_TRACE) && defined(_WIN32)
void haze_job0_capture_input(unsigned pc, const void* registers, const void* ls);
void haze_job0_capture_dump(void);
#else
#define haze_job0_capture_input(pc, registers, ls) ((void)0)
#define haze_job0_capture_dump() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
