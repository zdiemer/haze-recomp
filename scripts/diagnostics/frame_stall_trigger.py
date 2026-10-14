"""Bounded diagnostic trigger driven by observed frame-counter progress."""
import math


class FrameStallTrigger:
    def __init__(self, frequency, threshold_ms, samples_per_stall):
        if frequency <= 0 or not math.isfinite(threshold_ms) or threshold_ms <= 0:
            raise ValueError('Invalid stall clock/threshold')
        if samples_per_stall < 1:
            raise ValueError('Invalid stall sample budget')
        self.threshold = math.ceil(frequency * threshold_ms / 1000)
        self.budget = samples_per_stall
        self.value = None
        self.changed_qpc = None
        self.previous_qpc = None
        self.polls = self.progress = self.episodes = self.samples = 0
        self.episode_samples = self.max_poll_gap = 0

    def observe(self, value, now):
        if self.previous_qpc is not None:
            if now < self.previous_qpc:
                raise ValueError('Counter observations went backward')
            self.max_poll_gap = max(self.max_poll_gap, now - self.previous_qpc)
        self.previous_qpc = now
        self.polls += 1
        if self.value is None or value != self.value:
            if self.value is not None:
                self.progress += 1
            self.value, self.changed_qpc = value, now
            self.episode_samples = 0
            return None
        # Arm only after seeing actual progress. An initially frozen process is
        # not enough to establish a gameplay frame, and counter wrap is progress.
        if not self.progress or now - self.changed_qpc < self.threshold:
            return None
        if self.episode_samples >= self.budget:
            return None
        if not self.episode_samples:
            self.episodes += 1
        self.episode_samples += 1
        self.samples += 1
        return dict(episode=self.episodes, sample_in_episode=self.episode_samples,
                    frame_counter=value, progress_observed_qpc=self.changed_qpc,
                    trigger_qpc=now)

    def summary(self):
        return dict(polls=self.polls, observed_progress=self.progress,
                    episodes=self.episodes, sample_rounds=self.samples,
                    max_poll_gap_ticks=self.max_poll_gap,
                    threshold_ticks=self.threshold, samples_per_stall=self.budget)
