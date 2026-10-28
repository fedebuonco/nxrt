import os, sys, tempfile, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import onboard


class OnboardTest(unittest.TestCase):
    def test_parse_makefile(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "Makefile"), "w") as f:
                f.write("GAME_ID := demo\nGAME_EXE := demorecomp\n"
                        "GAME_MODULES := main:0x7000000000 sdk:0x7006000000\n")
            m = onboard.parse_makefile(d)
            self.assertEqual(m["id"], "demo")
            self.assertEqual(m["exe"], "demorecomp")
            self.assertEqual(m["modules"], [("main", 0x7000000000), ("sdk", 0x7006000000)])

    def test_check_reports_missing_inputs(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "Makefile"), "w") as f:
                f.write("GAME_ID := demo\nGAME_EXE := demorecomp\n")
            r = onboard.check(d)
            by = {x["check"]: x["ok"] for x in r["checks"]}
            self.assertFalse(by["exefs/main"])
            self.assertFalse(by["generated"])
            self.assertFalse(by["executable"])


if __name__ == "__main__":
    unittest.main()
