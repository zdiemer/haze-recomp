/* Shadow every eligible native math operation against the actual original,
 * then retain original state for the strict full-batch captured oracle. */
#define main original_replay_main
#include "replay_job13_capture.c"
#undef main
void haze_job13_native_math_report(void);
uint64_t haze_job13_native_math_failure_count(void);
int main(int argc, char** argv) {
    int result = original_replay_main(argc, argv);
    haze_job13_native_math_report();
    return result ? result : haze_job13_native_math_failure_count() ? 1 : 0;
}
