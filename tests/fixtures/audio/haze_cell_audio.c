/* Narrow audio-thread/output seam for the production host-stop extension. */
#include <windows.h>
#include <stdio.h>
static int s_mix_thread_running=1,s_audio_initialized=1;
static HANDLE s_mix_thread=(HANDLE)1;
static unsigned stopped,closed,timeout;
static DWORD result=WAIT_OBJECT_0;
static DWORD wait_audio(HANDLE,DWORD value) { timeout=value; return result; }
static BOOL close_audio(HANDLE) { ++closed; return TRUE; }
static void audio_backend_shutdown() { ++stopped; }
#define WaitForSingleObject wait_audio
#define CloseHandle close_audio
