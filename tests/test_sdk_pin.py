"""Exercise pin validation with real isolated Git histories, without network."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from check_sdk_pin import check, git, HAZE_REF, SDK_URL


class SdkPin(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "external/ps3recomp"
        self.sdk.mkdir(parents=True)
        for repo in (self.root, self.sdk):
            git(repo, "init", "-q")
            git(repo, "config", "user.name", "Pin test")
            git(repo, "config", "user.email", "pin@example.invalid")
            git(repo, "config", "commit.gpgsign", "false")
        git(self.sdk, "commit", "--allow-empty", "-qm", "base")
        self.pin = git(self.sdk, "rev-parse", "HEAD")
        git(self.sdk, "update-ref", HAZE_REF, self.pin)
        (self.root / ".gitmodules").write_text(
            '[submodule "external/ps3recomp"]\n'
            f' path = external/ps3recomp\n url = {SDK_URL}\n branch = haze\n')
        git(self.root, "add", ".gitmodules")
        git(self.root, "update-index", "--add", "--cacheinfo",
            f"160000,{self.pin},external/ps3recomp")
        git(self.root, "commit", "-qm", "pin")

    def test_detached_pin_can_lag_haze(self):
        git(self.sdk, "commit", "--allow-empty", "-qm", "later")
        git(self.sdk, "update-ref", HAZE_REF, "HEAD")
        git(self.sdk, "checkout", "--detach", self.pin)
        self.assertEqual(check(self.root), self.pin)

    def test_pin_on_feature_branch_only_is_rejected(self):
        git(self.sdk, "commit", "--allow-empty", "-qm", "feature")
        pin = git(self.sdk, "rev-parse", "HEAD")
        git(self.root, "update-index", "--cacheinfo", f"160000,{pin},external/ps3recomp")
        git(self.root, "commit", "-qm", "bad pin")
        with self.assertRaisesRegex(ValueError, "not on published haze"):
            check(self.root)

    def test_wrong_checkout(self):
        git(self.sdk, "commit", "--allow-empty", "-qm", "different")
        with self.assertRaisesRegex(ValueError, "match the committed gitlink"):
            check(self.root)

    def test_dirty_sdk(self):
        (self.sdk / "uncommitted.txt").write_text("work")
        with self.assertRaisesRegex(ValueError, "local modifications"):
            check(self.root)

    def test_missing_haze(self):
        git(self.sdk, "update-ref", "-d", HAZE_REF)
        with self.assertRaisesRegex(ValueError, "--fetch"):
            check(self.root)

    def test_fetch_refreshes_haze_from_canonical_url(self):
        remote = self.root / "fork.git"
        git(self.root, "init", "--bare", "-q", str(remote))
        git(self.sdk, "push", str(remote), "HEAD:refs/heads/haze")
        git(self.sdk, "update-ref", "-d", HAZE_REF)
        # Redirect only this fixture's canonical URL to its local bare fork.
        git(self.sdk, "config", f"url.{remote.as_uri()}.insteadOf", SDK_URL)
        self.assertEqual(check(self.root, fetch=True), self.pin)
        self.assertEqual(git(self.sdk, "rev-parse", HAZE_REF), self.pin)

    def test_wrong_branch_config(self):
        git(self.root, "config", "-f", ".gitmodules", "submodule.external/ps3recomp.branch", "master")
        git(self.root, "commit", "-am", "wrong branch")
        with self.assertRaisesRegex(ValueError, "branch must be"):
            check(self.root)

    def test_wrong_fork(self):
        git(self.root, "config", "-f", ".gitmodules", "submodule.external/ps3recomp.url",
            "https://github.com/sp00nznet/ps3recomp.git")
        git(self.root, "commit", "-am", "wrong fork")
        with self.assertRaisesRegex(ValueError, "url must be"):
            check(self.root)


if __name__ == "__main__":
    unittest.main()
