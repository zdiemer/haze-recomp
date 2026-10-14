"""Guard the generated main-producer call timing transform."""

from collections import Counter
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import patch_guest_pacer
import patch_producer_profile as producer_profile
import patch_job_wait
import patch_vp_upload
import patch_job_dispatch_trace
import patch_slot_copy
import patch_save_status
import patch_audio_tick
import patch_movie_poll


class ProducerProfilePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_path = ROOT / "generated/ppu/ppu_recomp_003.cpp"
        cls.memmove_path = ROOT / "generated/ppu/ppu_recomp_005.cpp"
        if not cls.source_path.exists() or not cls.memmove_path.exists():
            raise unittest.SkipTest("Requires the locally generated BLUS30094 PPU lift")
        cls.source = cls.source_path.read_text(encoding="cp1252")

    def test_pinned_original_and_guest_pacer_pipeline_are_accepted(self):
        original = producer_profile.patch(self.source)
        self.assertEqual(producer_profile.strip_profile(original), self.source)

        # Replay the actual generated-source pipeline through slot-copy, which
        # is the build stage immediately before this diagnostic.
        paced = patch_guest_pacer.patch(self.source)
        waited = patch_job_wait.patch(paced)
        vp_patched = patch_vp_upload.patch(waited)
        traced = patch_job_dispatch_trace.patch(vp_patched)
        slot_copy = patch_slot_copy.patch(traced, self.memmove_path.read_text(encoding="cp1252"))
        instrumented = producer_profile.patch(slot_copy)
        self.assertEqual(producer_profile.strip_profile(instrumented), slot_copy)
        self.assertIn('#include "haze_producer_profile.h"\n', instrumented)

        # Main now composes save/audio/movie hooks after the performance chain.
        # Removing only profiler scopes must yield the identical hooked game.
        final = patch_movie_poll.patch(patch_audio_tick.patch(patch_save_status.patch(instrumented)))
        reference = patch_movie_poll.patch(patch_audio_tick.patch(patch_save_status.patch(slot_copy)))
        self.assertEqual(producer_profile.strip_profile(final), reference)

    def test_every_audited_direct_call_is_covered_or_explicitly_excluded(self):
        source = patch_guest_pacer.patch(self.source)
        result = producer_profile.patch(source)
        body = producer_profile.function_body(result)
        sites = producer_profile.call_sites(body)
        self.assertEqual(len(sites), 74)
        scopes = [line for line in body.splitlines()
                  if "haze_producer_profile::call_scope haze_producer_call_" in line]
        self.assertEqual(len(scopes), 59)
        scope_sites = []
        for line in scopes:
            tokens = line.split("haze_producer_frame, ", 1)[1].split(");", 1)[0]
            lr, callee = [int(part.strip().rstrip("u"), 16) for part in tokens.split(",")]
            scope_sites.append((lr, f"{callee:08X}"[-8:]))
        expected = Counter(
            (int(match.group("lr"), 16), match.group("callee").upper())
            for match in sites if match.group("callee").upper() not in producer_profile.EXCLUDED_CALLEES
        )
        self.assertEqual(Counter(scope_sites), expected)

        excluded_calls = [match.group("callee").upper() for match in sites
                          if match.group("callee").upper() in producer_profile.EXCLUDED_CALLEES]
        self.assertEqual(len(excluded_calls), 15)
        self.assertEqual(set(excluded_calls), producer_profile.EXCLUDED_CALLEES)
        self.assertTrue(all(
            f"func_{callee}(ctx); DRAIN_TRAMPOLINE(ctx);" in body
            for callee in ("00680D18", "0068EC78")))
        self.assertIn("func_0068ECD8(ctx); DRAIN_TRAMPOLINE(ctx);", body)
        self.assertIn("func_006DD638(ctx); DRAIN_TRAMPOLINE(ctx);", body)

    def test_changed_or_repeated_function_is_rejected(self):
        changed = self.source.replace("func_00680D18(ctx);", "func_00680D19(ctx);", 1)
        with self.assertRaisesRegex(ValueError, "changed"):
            producer_profile.patch(changed)
        with self.assertRaisesRegex(ValueError, "changed"):
            producer_profile.patch(producer_profile.patch(self.source))

    def test_dispatcher_and_dynamic_targets_preserve_calls(self):
        result = producer_profile.patch(self.source)
        self.assertEqual(producer_profile.strip_profile(result), self.source)
        main = producer_profile.function_body(result)
        nested = producer_profile.function_body(result, producer_profile.DISPATCH_FUNCTION)
        self.assertEqual(len(producer_profile.call_sites(nested)), 14)
        self.assertEqual(nested.count("haze_producer_nested_"), 14)
        self.assertEqual(nested.count("haze_producer_indirect_"), 4)
        self.assertEqual(main.count("haze_producer_indirect_"), 2)
        for body, returns in ((main, producer_profile.MAIN_INDIRECT_RETURNS),
                              (nested, producer_profile.DISPATCH_INDIRECT_RETURNS)):
            for lr in returns:
                self.assertIn(f"0x{lr}u, static_cast<std::uint32_t>(ctx->ctr));", body)
        original_body = producer_profile.function_body(self.source, producer_profile.DISPATCH_FUNCTION)
        changed = self.source.replace(original_body, original_body.replace("func_0068EC40(ctx)", "func_0068EC41(ctx)"))
        with self.assertRaisesRegex(ValueError, "dispatcher changed"):
            producer_profile.patch(changed)


if __name__ == "__main__":
    unittest.main()
