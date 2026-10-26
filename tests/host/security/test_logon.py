"""H10 amendment-1 service invariants; ordinary logon UI remains user-03 work."""
import unittest
from test_accounts import AccountTests as _Accounts

class LogonTests(unittest.TestCase):
    setUpClass = classmethod(_Accounts.setUpClass.__func__)
    run_case = _Accounts.run_case

    def test_blank_retirement_and_reboot(self):
        self.run_case("retirement")

    def test_consumed_attempt_and_generations(self):
        self.run_case("protocol")

# Do not expose the imported TestCase to unittest discovery.
del _Accounts
