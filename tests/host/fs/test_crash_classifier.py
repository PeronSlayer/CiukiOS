"""Keep new crash classifications narrower than damaged ownership or EOF."""
import unittest
from classify_crashes import classify

PREFIX='FAT12 mode=0 cut=95\noracle lost=0 divergent=0 dirty=0 crosslinks=0 corrupt=0\n'
TAIL=('/Crash A/Crash file long.bin\n'
      '  File size is 12588 bytes, cluster chain length is > 16384 bytes.\n'
      '  Truncating file to 12588 bytes.\n')
LFN=('Wrong checksum for long file name "R.bin".\n'
     '  (Short name RENAME~1.BIN may have changed without updating the long name)\n'
     '  Not auto-correcting this.\n')

class ClassifierTests(unittest.TestCase):
    def test_known_surplus_and_interrupted_lfn(self):
        cases,counts=classify(PREFIX+TAIL+LFN)
        self.assertEqual(cases,1)
        self.assertEqual(counts['owned_excess_tail'],1)
        self.assertEqual(counts['lfn_checksum_disagreement'],1)

    def test_never_accept_shortening_published_data(self):
        for tail in (TAIL.replace('is > 16384','is 8192'),
                     TAIL.replace('to 12588','to 8192'),
                     TAIL.replace('> 16384','> 8192')):
            with self.subTest(tail=tail),self.assertRaises(AssertionError):
                classify(PREFIX+tail)

    def test_ownership_failure_is_fatal(self):
        for field in ('corrupt','crosslinks'):
            with self.subTest(field=field),self.assertRaises(AssertionError):
                classify(PREFIX.replace(field+'=0',field+'=1')+TAIL)

    def test_unknown_names_and_missing_context_fail(self):
        for text in (LFN.replace('R.bin','OTHER.bin'),LFN.splitlines()[1]+'\n',
                     LFN.replace('RENAME~1.BIN','OTHER.BIN'),TAIL.splitlines()[0]+'\n'):
            with self.subTest(text=text),self.assertRaises(AssertionError):
                classify(PREFIX+text)

if __name__=='__main__':
    unittest.main()
