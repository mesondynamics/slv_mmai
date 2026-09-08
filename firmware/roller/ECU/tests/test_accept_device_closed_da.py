"""Static host-side gates for the new-board CLOSED DA acceptance."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import accept_device_closed_da as accept


class ClosedDaAcceptanceTests(unittest.TestCase):
    def test_nonsecure_segments_are_contiguous_exact_slot(self):
        cursor = 0
        total = 0
        for _, address, offset, size in accept.NONSECURE:
            self.assertEqual(offset, cursor)
            self.assertEqual(address, 0x08100000 + offset)
            self.assertGreater(size, 0)
            cursor += size
            total += size
        self.assertEqual(total, 0x50000)
        self.assertEqual(accept.NONSECURE,
                         (("nonsecure-primary", 0x08100000, 0, 0x50000),))

    def test_service_has_no_lifecycle_obkey_or_regression_writer(self):
        source = Path(accept.__file__).read_text()
        self.assertNotIn('"-sdp"', source)
        self.assertNotIn('"PRODUCT_STATE=', source)
        self.assertNotIn('full-regression-to-open', source)
        self.assertNotIn('per=f', source)
        self.assertIn('open-app-debug', source)
        self.assertIn('close-debug', source)

    def test_halt_and_upload_share_one_connection_then_resume(self):
        source = Path(accept.__file__).read_text()
        self.assertIn('["-halt", "-u", hex(address), hex(size), str(path)]', source)
        self.assertIn('["-run"]', source)
        self.assertLess(source.index('["-halt", "-u"'), source.index('["-run"]'))

    def test_ordinary_denial_reads_only_small_secure_application_sample(self):
        source = Path(accept.__file__).read_text()
        self.assertIn('"0x0C030400", "0x100"', source)
        self.assertNotIn('"0x08000000", "0x200000"', source)

    def test_secure_region_is_hdpl3_primary(self):
        self.assertEqual(accept.SECURE, ("secure-primary", 0x0C030000,
                                        0x30000, 0x30000))

    def test_evidence_reports_closed_not_pretransition_target_state(self):
        source = Path(accept.__file__).read_text()
        self.assertIn('dict(m.TARGET, product_state="0x72 CLOSED")', source)


if __name__ == "__main__":
    unittest.main()
