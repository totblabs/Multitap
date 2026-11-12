import base64
import importlib.util
import json
import io
import os
from pathlib import Path
import select
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('multitap_setup', Path(__file__).resolve().parents[1] / 'setup' / 'proxmox.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class FakePve:
    """Command fixture only: never invokes Proxmox commands or changes real VMs."""
    def __init__(self, fail=None, existing=False):
        self.commands = []
        self.fail = fail
        self.existing = existing

    def __call__(self, command, stage):
        self.commands.append(command)
        if self.fail and self.fail(command):
            raise setup.SetupError('simulated CLI failure')
        operation = command[1:]
        if operation[:2] == ['user', 'list']:
            return json.dumps([{'userid': 'root@pam'}, *([{'userid': 'multitap-fixture@pve'}] if self.existing else [])])
        if operation[:2] == ['role', 'list']:
            return json.dumps([{'roleid': 'Administrator'}])
        if operation[:3] == ['user', 'token', 'add']:
            return json.dumps({'value': 'fixture-secret-not-production'})
        return ''


class EnrollmentTests(unittest.TestCase):
    def test_scope_is_all_current_future_vms_and_uses_separate_privileges(self):
        runner = FakePve()
        password_users = []
        secret = setup.provision('multitap-fixture@pve', 'Multitap-123456abcdef', 'broker', 2000000000, runner=runner, password_setter=password_users.append)
        self.assertEqual(password_users, ['multitap-fixture@pve'])
        self.assertEqual(secret, 'fixture-secret-not-production')
        writes = [command for command in runner.commands if 'list' not in command]
        self.assertTrue(all(command[0] == '/usr/sbin/pveum' for command in writes))
        acls = [command for command in writes if command[1:3] == ['acl', 'modify']]
        self.assertEqual(len(acls), 2)
        self.assertEqual({command[3] for command in acls}, {'/vms'})
        self.assertTrue(all(command[command.index('--propagate') + 1] == '1' for command in acls))
        self.assertTrue(any('--users' in command for command in acls))
        self.assertTrue(any('--tokens' in command for command in acls))
        self.assertIn('VM.Audit VM.PowerMgmt VM.Console', writes[0])
        # The sound card left Multitap: no new role may change VM hardware.
        self.assertTrue(all('VM.Config' not in str(command) for command in runner.commands))
        token = next(command for command in writes if command[1:4] == ['user', 'token', 'add'])
        self.assertEqual(token[token.index('--privsep') + 1], '1')
        self.assertTrue(all(secret not in str(command) for command in runner.commands))

    def test_agent_privilege_follows_the_pve_major_version(self):
        version = lambda text: (lambda command, stage: text)
        self.assertEqual(setup.agent_privilege(version('pve-manager/9.0.3/025864202ebb6109 (running kernel: 6.14.8-2-pve)')), 'VM.GuestAgent.Audit')
        self.assertEqual(setup.agent_privilege(version('pve-manager/8.4.1/2a5fa54a8503f96d (running kernel: 6.8.12-9-pve)')), 'VM.Monitor')
        self.assertIsNone(setup.agent_privilege(version('pve-manager/7.4-3/abc')))
        self.assertIsNone(setup.agent_privilege(version('garbage')))
        def broken(command, stage):
            raise setup.SetupError('x')
        self.assertIsNone(setup.agent_privilege(broken))

    def test_new_role_gets_console_and_the_detected_read_privilege_only(self):
        for privilege, expected in (('VM.GuestAgent.Audit', 'VM.Audit VM.PowerMgmt VM.Console VM.GuestAgent.Audit'), ('VM.Monitor', 'VM.Audit VM.PowerMgmt VM.Console VM.Monitor'), (None, 'VM.Audit VM.PowerMgmt VM.Console')):
            runner = FakePve()
            setup.provision('multitap-fixture@pve', 'Multitap-123456abcdef', 'broker', 2000000000, runner=runner, password_setter=lambda user: None, privilege=privilege)
            add = next(command for command in runner.commands if command[1:3] == ['role', 'add'])
            self.assertEqual(add[add.index('--privs') + 1], expected)

    def test_renewal_appends_the_privilege_to_only_this_identitys_multitap_role(self):
        acls = [
            {'path': '/vms', 'ugid': 'multitap-fixture@pve', 'roleid': 'Multitap-123456abcdef'},
            {'path': '/vms', 'ugid': 'multitap-fixture@pve!broker', 'roleid': 'Multitap-123456abcdef'},
            {'path': '/vms', 'ugid': 'other@pve', 'roleid': 'Multitap-aaaaaaaaaaaa'},
            {'path': '/', 'ugid': 'multitap-fixture@pve', 'roleid': 'Administrator'},
        ]
        commands = []
        def runner(command, stage):
            commands.append(command)
            if command[0] == '/usr/bin/pveversion':
                return 'pve-manager/9.0.3/x'
            if command[1:3] == ['acl', 'list']:
                return json.dumps(acls)
            return ''
        self.assertEqual(setup.ensure_agent_privilege('multitap-fixture@pve!broker', runner), 'VM.GuestAgent.Audit')
        modify = [command for command in commands if command[1:3] == ['role', 'modify']]
        self.assertEqual(modify, [['/usr/sbin/pveum', 'role', 'modify', 'Multitap-123456abcdef', '--append', '1', '--privs', 'VM.GuestAgent.Audit']])

    def test_no_action_or_text_of_the_helper_grants_a_hardware_privilege(self):
        source = Path(setup.__file__).read_text(encoding='utf8')
        self.assertNotIn('--enable-sound-card', source)
        self.assertFalse(hasattr(setup, 'enable_sound_card'))
        # The one mention left is the list of privileges an existing role may already hold.
        self.assertEqual(source.count('VM.Config.HWType'), 2)
        self.assertNotIn("'--privs', 'VM.Config", source)

    def test_renewal_leaves_roles_alone_when_version_or_role_is_unknown(self):
        commands = []
        def unknown_version(command, stage):
            commands.append(command); return 'nonsense'
        self.assertIsNone(setup.ensure_agent_privilege('multitap-fixture@pve!broker', unknown_version))
        def custom_role(command, stage):
            commands.append(command)
            if command[0] == '/usr/bin/pveversion':
                return 'pve-manager/8.2.2/x'
            return json.dumps([{'path': '/vms', 'ugid': 'multitap-fixture@pve', 'roleid': 'PVEVMAdmin'}]) if command[1:3] == ['acl', 'list'] else ''
        self.assertIsNone(setup.ensure_agent_privilege('multitap-fixture@pve!broker', custom_role))
        self.assertFalse(any(command[1:3] == ['role', 'modify'] for command in commands))

    def test_existing_identity_is_never_modified(self):
        runner = FakePve(existing=True)
        with self.assertRaises(setup.SetupError):
            setup.provision('multitap-fixture@pve', 'Multitap-123456abcdef', 'broker', 2000000000, runner=runner, password_setter=lambda user: self.fail('Existing identity must not have its password changed'))
        self.assertTrue(all('list' in command for command in runner.commands))

    def test_failure_rolls_back_only_its_new_identity(self):
        runner = FakePve(fail=lambda command: command[1:3] == ['acl', 'modify'] and '--tokens' in command)
        with self.assertRaises(setup.SetupError):
            setup.provision('multitap-fixture@pve', 'Multitap-123456abcdef', 'broker', 2000000000, runner=runner, password_setter=lambda user: None)
        deletes = [command for command in runner.commands if 'delete' in command]
        self.assertTrue(any(command[1:4] == ['user', 'token', 'delete'] for command in deletes))
        self.assertTrue(any(command[1:3] == ['user', 'delete'] for command in deletes))
        self.assertTrue(any(command[1:3] == ['role', 'delete'] for command in deletes))
        self.assertTrue(all('root@pam' not in command and 'Administrator' not in command for command in deletes))

    def test_url_and_connection_export_without_account_password(self):
        for value in ['http://pve:8006', 'https://root:password@pve:8006', 'https://pve:8006/#path']:
            with self.assertRaises(setup.SetupError):
                setup.endpoint(value)
        self.assertEqual(setup.endpoint('pve.example.com:8006/'), 'https://pve.example.com:8006')
        block = setup.connection_block('https://pve.example.com:8006', 'multitap-fixture@pve', 'broker', 'fixture-secret-not-production', '')
        lines = block.splitlines()
        self.assertEqual(lines[0], 'MULTITAP-CONNECTION-V1:')
        self.assertEqual(lines[-1], 'END-MULTITAP-CONNECTION')
        value = json.loads(base64.b64decode(''.join(lines[1:-1])))
        self.assertEqual(set(value), {'endpoint', 'tokenId', 'secret', 'ca'})
        self.assertEqual(value['tokenId'], 'multitap-fixture@pve!broker')

    def test_native_password_command_uses_tty_streams_and_no_secret_arguments(self):
        terminal = type('FixtureTerminal', (), {'reader': io.StringIO(), 'writer': io.StringIO()})()
        with patch.object(setup.subprocess, 'run') as run:
            setup.set_password('multitap-fixture@pve', terminal)
        args, kwargs = run.call_args
        self.assertEqual(args, (['/usr/sbin/pveum', 'passwd', 'multitap-fixture@pve'],))
        self.assertIs(kwargs['stdin'], terminal.reader)
        self.assertIs(kwargs['stdout'], terminal.writer)
        self.assertIs(kwargs['stderr'], terminal.writer)
        self.assertNotIn('capture_output', kwargs)
        self.assertNotIn('input', kwargs)
        self.assertNotIn('env', kwargs)

    def test_password_failure_removes_new_identity_before_token_creation(self):
        runner = FakePve()
        def rejected(user):
            raise setup.SetupError('password confirmation rejected')
        with self.assertRaises(setup.SetupError):
            setup.provision('multitap-fixture@pve', 'Multitap-123456abcdef', 'broker', 2000000000, runner=runner, password_setter=rejected)
        self.assertFalse(any(command[1:4] == ['user', 'token', 'add'] for command in runner.commands))
        self.assertTrue(any(command[1:3] == ['user', 'delete'] for command in runner.commands))
        self.assertTrue(any(command[1:3] == ['role', 'delete'] for command in runner.commands))

    def test_empty_or_changing_inventory_is_valid_and_no_vm_number_prompt_exists(self):
        from unittest.mock import MagicMock
        for items in [[], [{'vmid': 100, 'type': 'qemu'}], [{'vmid': 100, 'type': 'qemu'}, {'vmid': 101, 'type': 'qemu'}]]:
            response = MagicMock()
            response.__enter__.return_value = response
            response.read.return_value = json.dumps({'data': items}).encode()
            opener = MagicMock()
            opener.open.return_value = response
            with patch('urllib.request.build_opener', return_value=opener):
                setup.verify_api('https://pve.example:8006', None, 'multitap-fixture@pve!broker', 'fixture-secret')
        self.assertNotIn('select_vms', setup.__dict__)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux controlling PTY regression')
    def test_piped_python_stdin_uses_controlling_tty_for_sequential_prompts(self):
        # A real PTY and a piped Python program, without invoking any Proxmox CLI.
        import pty
        source = """import importlib.util,sys
spec=importlib.util.spec_from_file_location('enrollment', %r)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert not sys.stdin.isatty()
with module.Terminal() as terminal:
    assert terminal.reader.isatty() and terminal.writer.isatty()
    first=terminal.ask('Address')
    second=terminal.ask('Id')
assert terminal.reader.closed and terminal.writer.closed
assert first=='https://pve.example:8006' and second=='fixture'
print('PTY_PROMPTS_OK', flush=True)
""" % str(Path(setup.__file__).resolve())
        wrapper = 'import subprocess,sys;sys.exit(subprocess.run([sys.executable,"-"],input=%r,text=True).returncode)' % source
        pid, master = pty.fork()
        if pid == 0:
            os.execl(sys.executable, sys.executable, '-c', wrapper)
        seen = bytearray()
        deadline = time.monotonic() + 8
        answers = [(b'Address: ', b'https://pve.example:8006\n'), (b'Id: ', b'fixture\n')]
        status = None
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    try:
                        chunk = os.read(master, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    seen.extend(chunk)
                    if answers and answers[0][0] in seen:
                        os.write(master, answers.pop(0)[1])
                result, value = os.waitpid(pid, os.WNOHANG)
                if result:
                    status = value
                    break
            if status is None:
                result, value = os.waitpid(pid, os.WNOHANG)
                if result:
                    status = value
            self.assertIn(b'PTY_PROMPTS_OK', seen, bytes(seen))
            if status is None:
                _, status = os.waitpid(pid, 0)
            self.assertEqual(os.waitstatus_to_exitcode(status), 0)
        finally:
            if status is None:
                import signal
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            os.close(master)

    def test_raw_command_errors_never_expose_a_token(self):
        failure = subprocess.CalledProcessError(1, ['pveum'], output='fixture-sensitive-token', stderr='fixture-sensitive-token')
        with patch.object(setup.subprocess, 'run', side_effect=failure):
            with self.assertRaises(setup.SetupError) as caught:
                setup.run(['/usr/sbin/pveum', 'role', 'list'], 'test stage')
        self.assertNotIn('fixture-sensitive-token', str(caught.exception))


if __name__ == '__main__':
    unittest.main()
