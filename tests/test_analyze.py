import os, struct, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import analyze


class EncodedTest(unittest.TestCase):
    def test_uleb128_datarel_is_supported(self):
        # DW_EH_PE_datarel | DW_EH_PE_uleb128, the encoding NSMBU's SDK uses
        v, size = analyze.read_encoded(bytes([0x81, 0x01]), 0, 0x31, 0x100)
        self.assertEqual((v, size), (0x181, 2))

    def test_sdata2_and_udata4(self):
        v, size = analyze.read_encoded(struct.pack('<h', -2), 0, 0x0A, 0x100)
        self.assertEqual((v, size), (-2, 2))
        v, size = analyze.read_encoded(struct.pack('<I', 0x1234), 0, 0x03, 0x100)
        self.assertEqual((v, size), (0x1234, 4))

    def test_sleb128(self):
        v, size = analyze.read_encoded(bytes([0x7e]), 0, 0x09, 0)
        self.assertEqual((v, size), (-2, 1))


if __name__ == "__main__":
    unittest.main()
