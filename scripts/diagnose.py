"""Analyze captures and profiles, or collect diagnostics from an owned Haze run.

Run a command with --help for its inputs and collection requirements.
"""
import argparse
import runpy
import sys


# Explicit commands keep helper modules out of the public interface. Imports
# are lazy so offline reports do not load Windows APIs or optional dependencies.
COMMANDS = {
    'browser-cpu': ('summarize_browser_cpu', 'Summarize Chromium worker CPU profiles'),
    'renderer': ('summarize_profile', 'Summarize renderer CSV frame costs'),
    'presents': ('summarize_presents', 'Summarize successful native presents'),
    'pump': ('summarize_pump', 'Summarize renderer pump intervals'),
    'waits': ('summarize_waits', 'Summarize guest wait timelines'),
    'spurs-queues': ('summarize_spurs_queues', 'Summarize SPURS queue waits'),
    'spu-jobs': ('summarize_spu_jobs', 'Summarize completed SPU job costs'),
    'spu-job-modes': ('summarize_spu_job_modes', 'Compare SPU jobs across mode windows'),
    'frame-jobs': ('summarize_frame_jobs', 'Correlate draw windows and SPU jobs'),
    'job-completion': ('summarize_job_completion', 'Summarize job completion phases'),
    'job-trace': ('analyze_job_trace', 'Analyze dispatch, wait and async job traces'),
    'job-wait-frames': ('analyze_job_wait_frames', 'Attribute job wait intervals to frames'),
    'fifo-recycles': ('analyze_fifo_recycles', 'Analyze FIFO recycle traces'),
    'producer': ('analyze_producer_profile', 'Analyze producer call profiles'),
    'slow-drains': ('analyze_slow_drains', 'Attribute slow FIFO drains'),
    'slow-draws': ('analyze_slow_draws', 'Analyze slow draw stages and resources'),
    'texture-misses': ('analyze_texture_misses', 'Analyze texture cache misses'),
    'pump-stacks': ('correlate_pump_stacks', 'Correlate pump intervals and stack samples'),
    'pump-waits': ('correlate_pump_waits', 'Correlate pump intervals and guest waits'),
    'compare-draws': ('compare_draw_frames', 'Compare submissions in two captured frames'),
    'capture': ('capture_sequence', 'Request consecutive native frame readbacks'),
    'detach-pad': ('detach_pad_file', 'Detach diagnostic PAD_FILE input with acknowledgement'),
    'native-cpu': ('profile_native_cpu', 'Sample native Windows threads and stacks'),
    'watch-frames': ('watch_frame_progress', 'Watch native frame progress and stalls'),
    'cpu-work': ('benchmark_cpu_work', 'Measure renderer CPU variants in one scene'),
    'present-modes': ('benchmark_present_modes', 'Record or report present mode comparisons'),
    'thread-priority': ('benchmark_thread_priority', 'Measure owned thread priority variants'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Commands:\n' + '\n'.join(
            f'  {name:18} {description}' for name, (_, description) in COMMANDS.items()))
    parser.add_argument('command', choices=COMMANDS, help='diagnostic to run')
    parser.add_argument('arguments', nargs=argparse.REMAINDER, help=argparse.SUPPRESS)
    args = parser.parse_args()
    sys.argv = [f'{parser.prog} {args.command}', *args.arguments]
    runpy.run_module('diagnostics.' + COMMANDS[args.command][0], run_name='__main__')


if __name__ == '__main__':
    main()
