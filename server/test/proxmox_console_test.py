"""Memory command/TTY fixtures only. No pveum, shell, network or user files."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('multitap_console_setup', Path(__file__).resolve().parents[1] / 'setup' / 'proxmox.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)
USER = 'multitap-fixture@pve'
TOKEN = USER + '!broker'
ROLE = 'Multitap-123456abcdef'
NOW = 1700000000


class Tty:
    def __init__(self, answer='y', before=None):
        self.answer, self.before, self.prompts = answer, before, []
    def ask(self, label, default=''):
        self.prompts.append((label, default))
        if self.before:
            self.before()
        return self.answer or default
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass


class FakePve:
    def __init__(self):
        self.users = [{'userid': USER, 'enable': 1, 'expire': 2000000000, 'comment': 'Multitap all-VM API integration'}]
        self.tokens = [{'tokenid': 'broker', 'privsep': 1, 'expire': 2000000001}]
        self.acls = [{'path': '/vms', 'type': kind, 'ugid': principal, 'roleid': ROLE, 'propagate': 1}
                     for kind, principal in [('user', USER), ('token', TOKEN)]]
        self.roles = [{'roleid': ROLE, 'privs': 'VM.Audit,VM.GuestAgent.Audit,VM.PowerMgmt', 'special': 0},
                      {'roleid': 'PVEAdmin', 'privs': 'Sys.Modify', 'special': 1}]
        self.commands, self.writes = [], []
        self.fail_after_apply = False
        self.after_apply = None
    def __call__(self, command, stage):
        self.commands.append(list(command))
        if command == ['/usr/sbin/pveum', 'user', 'list', '--output-format', 'json']:
            return json.dumps(self.users)
        if command == ['/usr/sbin/pveum', 'user', 'token', 'list', USER, '--output-format', 'json']:
            return json.dumps(self.tokens)
        if command == ['/usr/sbin/pveum', 'acl', 'list', '--output-format', 'json']:
            return json.dumps(self.acls)
        if command == ['/usr/sbin/pveum', 'role', 'list', '--output-format', 'json']:
            return json.dumps(self.roles)
        expected = ['/usr/sbin/pveum', 'role', 'modify', ROLE, '--append', '1', '--privs', 'VM.Console']
        if command != expected:
            raise AssertionError('Unexpected mutation or command')
        self.writes.append(list(command))
        self.roles[0]['privs'] += ',VM.Console'
        if self.after_apply:
            self.after_apply()
        if self.fail_after_apply:
            raise setup.SetupError('PRIVATE_CLI_SENTINEL')
        return ''


class ConsoleTests(unittest.TestCase):
    def run_enable(self, fake, tty=None):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = setup.enable_console(tty or Tty(), TOKEN, runner=fake, now=NOW)
        return result, output.getvalue()

    def test_only_console_append_after_explicit_confirmation_and_readback(self):
        fake = FakePve()
        before = copy.deepcopy((fake.users, fake.tokens, fake.acls, fake.roles[1]))
        result, output = self.run_enable(fake)
        self.assertTrue(result['changed'])
        self.assertEqual(len(fake.writes), 1)
        self.assertEqual(len(fake.commands), 13)  # four reads, four rechecks, one append, four verification reads
        self.assertEqual((fake.users, fake.tokens, fake.acls, fake.roles[1]), before)
        self.assertEqual(set(fake.roles[0]['privs'].split(',')), {'VM.Audit', 'VM.PowerMgmt', 'VM.GuestAgent.Audit', 'VM.Console'})
        self.assertIn('VM.Console', output)

    def test_cancel_defaults_to_no_and_never_writes(self):
        for answer in ['', 'n', 'no']:
            fake, tty = FakePve(), Tty(answer)
            result, _ = self.run_enable(fake, tty)
            self.assertFalse(result['changed'])
            self.assertEqual(fake.writes, [])
            self.assertEqual(tty.prompts[0][1], 'N')

    def test_already_enabled_is_read_only(self):
        fake, tty = FakePve(), Tty()
        fake.roles[0]['privs'] += ',VM.Console'
        result, _ = self.run_enable(fake, tty)
        self.assertFalse(result['changed'])
        self.assertEqual(fake.writes, [])
        self.assertEqual(tty.prompts, [])

    def test_invalid_or_foreign_identity_is_rejected_before_any_command(self):
        for value in ['root@pam!broker', 'other@pve!broker', TOKEN + ';id', TOKEN + '\n', "multitap-fixture@pve!broker'", TOKEN + '$(id)']:
            fake = FakePve()
            with self.assertRaises(setup.SetupError):
                setup.console_identity_plan(value, fake, NOW)
            self.assertEqual(fake.commands, [])

    def test_disabled_unknown_comment_groups_multiple_tokens_and_privsep_refused(self):
        changes = [lambda f: f.users[0].update(enable=0), lambda f: f.users[0].pop('enable'),
                   lambda f: f.users[0].update(comment='manual account'), lambda f: f.users[0].update(groups='administrators'),
                   lambda f: f.tokens.clear(), lambda f: f.tokens.append({'tokenid': 'other', 'privsep': 1}),
                   lambda f: f.tokens[0].update(privsep=0), lambda f: f.users[0].update(expire=NOW - 1),
                   lambda f: f.tokens[0].update(expire=NOW - 1)]
        for change in changes:
            fake = FakePve(); change(fake)
            with self.assertRaises(setup.SetupError):
                self.run_enable(fake)
            self.assertEqual(fake.writes, [])

    def test_shared_role_with_other_identity_group_or_path_is_refused(self):
        for extra in [{'path': '/vms', 'type': 'user', 'ugid': 'other@pve', 'roleid': ROLE, 'propagate': 1},
                      {'path': '/vms', 'type': 'group', 'ugid': 'operators', 'roleid': ROLE, 'propagate': 1},
                      {'path': '/nodes', 'type': 'token', 'ugid': TOKEN, 'roleid': ROLE, 'propagate': 1}]:
            fake = FakePve(); fake.acls.append(extra)
            with self.assertRaises(setup.SetupError):
                self.run_enable(fake)
            self.assertEqual(fake.writes, [])

    def test_both_exact_vms_propagated_acls_and_one_matching_role_required(self):
        changes = [lambda f: f.acls.pop(), lambda f: f.acls[0].update(path='/'),
                   lambda f: f.acls[1].update(path='/vms/149'), lambda f: f.acls[1].update(propagate=0),
                   lambda f: f.acls[1].update(type='user'), lambda f: f.acls[1].update(roleid='Multitap-aaaaaaaaaaaa'),
                   lambda f: f.acls.append({'path': '/', 'type': 'user', 'ugid': USER, 'roleid': 'PVEAdmin', 'propagate': 1})]
        for change in changes:
            fake = FakePve(); change(fake)
            with self.assertRaises(setup.SetupError):
                self.run_enable(fake)
            self.assertEqual(fake.writes, [])

    def test_unknown_custom_special_or_malformed_role_and_acl_refused(self):
        changes = [lambda f: f.roles[0].update(privs='VM.Audit,VM.PowerMgmt,Sys.Modify'),
                   lambda f: f.roles[0].update(special=1), lambda f: f.roles[0].update(privs=['VM.Audit']),
                   lambda f: f.roles.append(copy.deepcopy(f.roles[0])), lambda f: f.acls[0].pop('propagate'),
                   lambda f: f.acls[0].update(unexpected=True)]
        for change in changes:
            fake = FakePve(); change(fake)
            with self.assertRaises(setup.SetupError):
                self.run_enable(fake)
            self.assertEqual(fake.writes, [])

    def test_configuration_change_during_confirmation_never_writes(self):
        fake = FakePve()
        tty = Tty(before=lambda: fake.tokens[0].update(expire=2000000010))
        with self.assertRaises(setup.SetupError):
            self.run_enable(fake, tty)
        self.assertEqual(fake.writes, [])

    def test_append_failure_is_uncertain_not_success_or_blind_rollback(self):
        fake = FakePve(); fake.fail_after_apply = True
        with self.assertRaisesRegex(setup.SetupError, '완료 여부') as error:
            self.run_enable(fake)
        self.assertNotIn('PRIVATE_CLI_SENTINEL', str(error.exception))
        self.assertEqual(len(fake.writes), 1)
        self.assertIn('VM.Console', fake.roles[0]['privs'])
        self.assertFalse(any('delete' in command for command in fake.commands))

    def test_post_append_acl_change_is_uncertain_without_more_mutation(self):
        fake = FakePve()
        fake.after_apply = lambda: fake.acls.append({'path': '/vms', 'type': 'group', 'ugid': 'operators', 'roleid': ROLE, 'propagate': 1})
        with self.assertRaisesRegex(setup.SetupError, '완료 여부'):
            self.run_enable(fake)
        self.assertEqual(len(fake.writes), 1)

    def test_output_contains_no_synthetic_token_secret(self):
        fake = FakePve(); fake.tokens[0]['secret'] = 'PRIVATE_TOKEN_SENTINEL'
        _, output = self.run_enable(fake)
        self.assertNotIn('PRIVATE_TOKEN_SENTINEL', output)
        self.assertNotIn('MULTITAP-CONNECTION', output)

    def test_main_dispatches_enable_console_without_enroll_or_renew(self):
        # main() picks the language from the locale: the run keeps it to itself.
        with patch.object(setup, 'LANGUAGE', setup.LANGUAGE), patch.object(setup.os, 'geteuid', return_value=0, create=True), patch.object(setup.os, 'access', return_value=True), patch.object(setup, 'Terminal', return_value=Tty()), patch.object(sys, 'argv', ['proxmox.py', '--enable-console', TOKEN]), patch.object(setup, 'enable_console') as enable, patch.object(setup, 'renew_enrollment') as renew, patch.object(setup, 'enroll') as enroll:
            setup.main()
            self.assertEqual(enable.call_args.args[1], TOKEN)
            renew.assert_not_called(); enroll.assert_not_called()

    def test_main_cannot_mix_renew_and_enable_console(self):
        # main() picks the language from the locale: the run keeps it to itself.
        with patch.object(setup, 'LANGUAGE', setup.LANGUAGE), patch.object(setup.os, 'geteuid', return_value=0, create=True), patch.object(setup.os, 'access', return_value=True), patch.object(sys, 'argv', ['proxmox.py', '--renew', TOKEN, '--enable-console', TOKEN]), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            setup.main()
        self.assertEqual(error.exception.code, 2)

    def test_renewal_remains_distinct_and_never_calls_enable_console(self):
        with patch.object(setup, 'renew_identity', return_value={'tokenId': TOKEN, 'expiresAt': '2099-01-01T00:00:00Z'}), patch.object(setup, 'ensure_agent_privilege', return_value=None), patch.object(setup, 'enable_console') as enable, contextlib.redirect_stdout(io.StringIO()) as output:
            setup.renew_enrollment(Tty(), TOKEN)
            enable.assert_not_called()
        self.assertNotIn('VM.Console', output.getvalue())


if __name__ == '__main__':
    unittest.main()
