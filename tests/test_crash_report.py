import os, sys, tempfile, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import crash_report


LOG = """\
*** signal 11 accessing 0x1
    host pc 120535bd8 (f_7000178b00+38)
    host lr 118c76d7c (f_7000178b00)
guest: sp=72120c86b0 lr=700017b38c
  x0  00000072cb20c358  x1  0000000000000001
  x3  00000072e79e2800  x29 00000072120c89e0
  x30 000000700017b38c
host backtrace (guest functions are f_<addr>):
  0   tlltdrecomp  0x1 f
  1   tlltdgame    0x2 f_7000178b00 + 27872
"""
SRC = """\
void f_7000178b00(Cpu *c);
void f_7000178b00(Cpu *CPU_RESTRICT c) {
    FN_ENTER(0x7000178b00ull);
}
"""


class CrashReportTest(unittest.TestCase):
    def test_parse_crash(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(LOG); path = f.name
        try:
            c = crash_report.parse_crash(path)
        finally:
            os.unlink(path)
        self.assertEqual(c["pc"], 0x7000178b00)
        self.assertEqual(c["access"], "0x1")
        self.assertEqual(c["regs"]["x3"], "00000072e79e2800")

    def test_source_definition_not_prototype(self):
        with tempfile.TemporaryDirectory() as d:
            gen = os.path.join(d, "generated", "main")
            os.makedirs(gen)
            with open(os.path.join(gen, "main_1.c"), "w") as f:
                f.write(SRC)
            got = crash_report.find_source(d, 0x7000178b00)
            self.assertIsNotNone(got)
            self.assertEqual(got[1], 2)


if __name__ == "__main__":
    unittest.main()
