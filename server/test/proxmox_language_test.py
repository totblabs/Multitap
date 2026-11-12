"""Korean and English (docs/i18n.md): how the language is chosen, the address
argument in any order, and English runs without Hangul. Memory fixtures only;
the one subprocess stops at its first check (no Proxmox command exists here)."""
import base64
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'setup' / 'proxmox.py'
spec = importlib.util.spec_from_file_location('language_setup', SOURCE)
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)

HANGUL = re.compile('[가-힣]')
TOKEN = 'multitap-fixture@pve!broker'
SECRET = 'fixture-secret-not-production'
FP = 'AB:' * 31 + 'CD'
PEM = '-----BEGIN CERTIFICATE-----\nZml4dHVyZQ==\n-----END CERTIFICATE-----\n'


class Tty:
    def __init__(self, answers=('', 'y')):
        self.answers, self.prompts = list(answers), []
    def ask(self, label, default=''):
        self.prompts.append(label)
        return self.answers.pop(0) or default
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass


class LanguageChoiceTests(unittest.TestCase):
    def test_flag_then_multitap_lang_then_the_locale_korean_only_for_ko(self):
        pick = setup.pick_language
        self.assertEqual(pick(['--lang', 'ko'], {}), 'ko')
        self.assertEqual(pick(['--lang=en'], {'MULTITAP_LANG': 'ko'}), 'en', 'the flag wins')
        self.assertEqual(pick(['https://pve.example.com', '--lang', 'ko'], {'LANG': 'en_US.UTF-8'}), 'ko')
        self.assertEqual(pick([], {'MULTITAP_LANG': 'ko', 'LANG': 'en_US.UTF-8'}), 'ko')
        self.assertEqual(pick([], {'LC_ALL': 'C', 'LANG': 'ko_KR.UTF-8'}), 'en', 'LC_ALL comes before LANG')
        self.assertEqual(pick([], {'LC_MESSAGES': 'ko_KR.UTF-8', 'LANG': 'en_US.UTF-8'}), 'ko')
        self.assertEqual(pick([], {'LANG': 'ko_KR.UTF-8'}), 'ko')
        # A Proxmox Shell is usually C or en_US: English.
        for environ in ({}, {'LANG': 'C'}, {'LANG': 'en_US.UTF-8'}, {'LANG': 'C.UTF-8'}, {'MULTITAP_LANG': 'fr'}):
            self.assertEqual(pick([], environ), 'en', environ)

    def run_main(self, argv, environ=None):
        calls = {}
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(setup, 'LANGUAGE', setup.LANGUAGE))
            stack.enter_context(patch.dict(setup.os.environ, environ or {}, clear=True))
            stack.enter_context(patch.object(setup.os, 'geteuid', return_value=0, create=True))
            stack.enter_context(patch.object(setup.os, 'access', return_value=True))
            stack.enter_context(patch.object(setup, 'Terminal', return_value=Tty()))
            for name in ('enroll', 'renew_enrollment', 'enable_console'):
                calls[name] = stack.enter_context(patch.object(setup, name))
            stack.enter_context(patch.object(sys, 'argv', argv))
            setup.main()
            language = setup.LANGUAGE
        return calls, language

    def test_the_address_works_before_or_after_the_language(self):
        calls, language = self.run_main(['-', '--lang', 'ko', 'https://pve.example.com'])
        self.assertEqual((calls['enroll'].call_args.args[1], language), ('https://pve.example.com', 'ko'))
        calls, language = self.run_main(['-', 'https://pve.example.com', '--lang', 'en'], {'LANG': 'ko_KR.UTF-8'})
        self.assertEqual((calls['enroll'].call_args.args[1], language), ('https://pve.example.com', 'en'))
        calls, language = self.run_main(['-', '--lang=ko'])
        self.assertEqual((calls['enroll'].call_args.args[1], language), ('', 'ko'))
        calls, language = self.run_main(['-'], {'MULTITAP_LANG': 'ko'})
        self.assertEqual(language, 'ko')
        calls, language = self.run_main(['-'], {'LANG': 'en_US.UTF-8'})
        self.assertEqual(language, 'en')
        calls, language = self.run_main(['-', '--renew', TOKEN, '--endpoint', 'https://pve.example:8006', '--lang', 'en'])
        self.assertEqual(calls['renew_enrollment'].call_args.args[1:], (TOKEN, 'https://pve.example:8006'))
        calls['enroll'].assert_not_called()
        calls, language = self.run_main(['-', '--lang', 'ko', '--enable-console', TOKEN])
        self.assertEqual((calls['enable_console'].call_args.args[1], language), (TOKEN, 'ko'))

    def test_an_unknown_language_or_an_address_with_renewal_is_refused(self):
        for argv in (['-', '--lang', 'fr'], ['-', '--lang'], ['-', '--lang', 'en', '--renew', TOKEN, 'https://pve.example.com']):
            with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit) as error:
                self.run_main(argv)
            self.assertEqual(error.exception.code, 2, argv)
        self.assertIn('A Proxmox address can be given only for a new connection.', errors.getvalue())

    def test_the_script_itself_stops_off_proxmox_in_the_chosen_language(self):
        # Here there is no /usr/sbin/pveum: the first check stops it, before any question.
        environ = {key: value for key, value in os.environ.items() if key not in ('MULTITAP_LANG', 'LC_ALL', 'LC_MESSAGES', 'LANG')}
        environ['PYTHONIOENCODING'] = 'utf-8'
        def run(*args, **extra):
            result = subprocess.run([sys.executable, str(SOURCE), *args], capture_output=True, text=True, encoding='utf-8', timeout=30, env={**environ, **extra})
            return result.returncode, result.stderr
        code, english = run('--lang', 'en', 'https://pve.example.com')
        self.assertEqual(code, 1)
        self.assertRegex(english, r'✖  ?Setup stopped: This is (not a Proxmox node|the Multitap server)\. Run this command on a Proxmox node\.')
        self.assertIsNone(HANGUL.search(english), english)
        code, korean = run('https://pve.example.com', '--lang', 'ko')
        self.assertEqual(code, 1)
        self.assertRegex(korean, r'설정 중단: 여기는 (Proxmox 노드가 아닙니다|Multitap 서버입니다)\. 이 명령은 Proxmox 노드에서 실행합니다\.')
        self.assertIn('설정 중단', run(MULTITAP_LANG='ko')[1])
        self.assertIn('Setup stopped', run(LANG='C')[1])
        self.assertIn('Setup stopped', run()[1], 'no locale at all: English')


class EnglishOutputTests(unittest.TestCase):
    def setUp(self):
        self.language = patch.object(setup, 'LANGUAGE', 'en')
        self.language.start()
        self.addCleanup(self.language.stop)

    def full_run(self, tty_output, language='en'):
        class Out(io.StringIO):
            def isatty(self):
                return tty_output
        output = Out()
        trust = setup.Trust(PEM, None, FP)
        with patch.object(setup, 'LANGUAGE', language), patch.object(setup, 'BROKER_ORIGIN', 'http://192.0.2.50:18480'), patch.object(setup, 'default_endpoint', return_value='https://192.0.2.2:8006'), \
                patch.object(setup, 'check_address', return_value=trust), patch.object(setup, 'inventory', return_value=[{'vmid': 101}, {'vmid': 102}]), \
                patch.object(setup, 'check_new_identity'), patch.object(setup, 'agent_privilege', return_value='VM.GuestAgent.Audit'), \
                patch.object(setup, 'provision', return_value=SECRET), patch.object(setup.secrets, 'token_hex', return_value='0a1b2c3d4e5f'), \
                patch.object(setup.time, 'time', return_value=1767225600), patch.dict(setup.os.environ, {'TERM': 'xterm'}), patch.object(sys, 'stdout', output):
            setup.os.environ.pop('NO_COLOR', None)
            tty = Tty()
            setup.enroll(tty)
        return output.getvalue(), tty.prompts

    @staticmethod
    def block(text):
        return text[text.index('MULTITAP-CONNECTION-V1:\n'):text.index('END-MULTITAP-CONNECTION\n') + len('END-MULTITAP-CONNECTION')]

    def test_a_full_english_run_has_no_hangul_and_the_same_copy_block(self):
        plain, prompts = self.full_run(False)
        self.assertNotIn('\x1b', plain)
        self.assertIsNone(HANGUL.search(plain + ''.join(prompts)), plain)
        order = ['▶ Connect Multitap to Proxmox', 'Multitap server address to enter in the Windows app: http://192.0.2.50:18480',
                 '[1/3] Proxmox address', '✔ Proxmox address: https://192.0.2.2:8006 (found automatically) — address and certificate checked',
                 "⚠ The certificate does not name this address", 'Fingerprint (SHA-256): ' + FP,
                 '✔ Found 2 VMs in Proxmox.', '[2/3] Integration account',
                 'Privileges: VM.Audit (VM status, finding Hosts), VM.PowerMgmt (power), VM.Console (emergency Console), VM.GuestAgent.Audit (checking that Windows is up)',
                 'Account and token valid for: 365 days', '✔ Integration account and token created.', '[3/3] Connection details',
                 '▼ Copy everything below from MULTITAP-CONNECTION-V1: to END-MULTITAP-CONNECTION and paste it into box 2 in the admin console\nMULTITAP-CONNECTION-V1:\n',
                 'END-MULTITAP-CONNECTION\n▲ Up to here', '⚠ The text above can contain a secret token.', 'Summary', 'Token expires: 2027-01-01T00:00:00Z',
                 'Admin console: http://192.0.2.50:18480/admin', 'The desktop list shows only VMs with an installed and approved Host.',
                 '→ In the admin console, paste the connection details and press "Connect".']
        position = 0
        for text in order:
            position = plain.index(text, position) + len(text)
        self.assertEqual(prompts, ['New integration ID (multitap- is added in front)', 'Create these? (y/N)'])
        korean, _ = self.full_run(False, 'ko')
        self.assertEqual(self.block(plain), self.block(korean), 'the block a person copies does not depend on the language')
        self.assertEqual(json.loads(base64.b64decode(''.join(self.block(plain).splitlines()[1:-1])))['fingerprint'], FP)
        coloured, _ = self.full_run(True)
        self.assertIn('\x1b[1;36m[1/3] Proxmox address\x1b[0m', coloured)
        self.assertIn('\n' + self.block(plain) + '\n', coloured)
        self.assertIsNone(HANGUL.search(coloured))

    def test_one_vm_is_singular(self):
        self.assertEqual(setup.found_vms(1), 'Found 1 VM in Proxmox.')
        self.assertEqual(setup.found_vms(0), 'Found 0 VMs in Proxmox.')

    def test_a_failing_automatic_address_and_errors_in_english(self):
        output = io.StringIO()
        answers = ['', 'pve.example.com']
        class Asking:
            def ask(self, label, default=''):
                output.write('? ' + label + '\n')
                return answers.pop(0)
        failure = setup.AddressError('long text', 'Not usable: the certificate names another address.')
        trust = setup.Trust('', None, '')
        results = {'https://198.51.100.5:8006': failure, 'https://pve.example.com': trust}
        def check(url, ca):
            result = results[url]
            if isinstance(result, Exception):
                raise result
            return result
        with patch.object(setup, 'BROKER_ORIGIN', 'https://broker.example'), patch.object(setup, 'check_address', side_effect=check), \
                patch.object(setup, 'default_endpoint', return_value='https://198.51.100.5:8006'), contextlib.redirect_stdout(output):
            self.assertEqual(setup.choose_endpoint(Asking(), '', ''), ('https://pve.example.com', trust))
        text = output.getvalue()
        self.assertIn('⚠ Address tried automatically: https://198.51.100.5:8006\n  Not usable: the certificate names another address.\n', text)
        self.assertIn('✖ Enter the Proxmox web address, e.g. https://pve.example.com', text)
        self.assertIn('? Proxmox web address (not the Multitap address above)', text)
        self.assertIsNone(HANGUL.search(text), text)
        with self.assertRaisesRegex(setup.SetupError, r'^Enter it as https://<Proxmox address>:8006\.$'):
            setup.endpoint('http://pve:8006')
        with patch.object(setup, 'BROKER_ORIGIN', 'https://broker.example'), self.assertRaisesRegex(setup.SetupError, 'Multitap sign-in server'):
            setup.proxmox_endpoint('https://broker.example/')
        with self.assertRaisesRegex(setup.SetupError, 'from your Multitap admin console'):
            setup.enroll(None)
        with patch.object(setup.subprocess, 'run', side_effect=setup.subprocess.CalledProcessError(1, ['pveum'])):
            with self.assertRaisesRegex(setup.SetupError, r'^Failed at this step: Listing VMs\. Check the Proxmox status and permissions\.$'):
                setup.inventory()

    def test_check_proxmox_and_trust_messages_in_english(self):
        with patch.object(setup, 'opener_for') as opener:
            response = opener.return_value.open.return_value.__enter__.return_value
            response.status, response.headers, response.read.return_value = 200, {'Content-Type': 'text/html', 'Server': 'Mini-BMC'}, b'<html></html>'
            with self.assertRaises(setup.AddressError) as caught:
                setup.check_proxmox('https://bmc.example', None)
        self.assertTrue(str(caught.exception).startswith('This address is not Proxmox (HTTP 200, text/html).'))
        self.assertEqual(caught.exception.short, 'Not Proxmox (HTTP 200, text/html).')
        with patch.object(setup.socket, 'create_connection', side_effect=setup.socket.gaierror()):
            with self.assertRaises(setup.AddressError) as caught:
                setup.trust_for('https://nowhere.invalid:8006', '')
        self.assertEqual(caught.exception.short, 'The address was not found.')
        self.assertIn('Could not find the Proxmox host name you entered.', str(caught.exception))
        self.assertIsNone(HANGUL.search(str(caught.exception) + caught.exception.short))

    def test_console_and_renewal_in_english(self):
        from proxmox_console_test import FakePve, NOW
        fake = FakePve()
        tty = Tty(['y'])
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = setup.enable_console(tty, TOKEN, runner=fake, now=NOW)
        self.assertTrue(result['changed'])
        self.assertEqual(tty.prompts, ['Add only the console permission to this role? (y/N)'])
        self.assertIn('Add the Multitap emergency Console permission\nFor: ' + TOKEN, output.getvalue())
        self.assertIn('Confirmed: VM.Console was added to the role', output.getvalue())
        self.assertIsNone(HANGUL.search(output.getvalue()), output.getvalue())
        tty = Tty(['y'])
        with patch.object(setup, 'renew_identity', return_value={'tokenId': TOKEN, 'expiresAt': '2099-01-01T00:00:00Z'}), \
                patch.object(setup, 'ensure_agent_privilege', return_value='VM.GuestAgent.Audit'), contextlib.redirect_stdout(io.StringIO()) as output:
            setup.renew_enrollment(tty, TOKEN)
        text = output.getvalue()
        self.assertEqual(tty.prompts, ['Renew with these settings? (y/N)'])
        for expected in ('Renew the Multitap integration\nFor: ' + TOKEN, 'Guest agent read privilege checked: VM.GuestAgent.Audit', '✔ Renewed',
                         '▼ Copy everything below from MULTITAP-RENEWAL-V1: to END-MULTITAP-RENEWAL and paste it into the renewal window in the admin console\nMULTITAP-RENEWAL-V1:\n'):
            self.assertIn(expected, text)
        self.assertIsNone(HANGUL.search(text), text)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            setup.renew_enrollment(Tty(['n']), TOKEN)
        self.assertIn('Cancelled. Nothing was changed.', output.getvalue())

    def test_every_english_text_in_the_source_is_free_of_hangul(self):
        # Each t('<Korean>', '<English>') call: the English side never carries Korean.
        source = SOURCE.read_text(encoding='utf8')
        import ast
        tree = ast.parse(source)
        pairs = 0
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and getattr(node.func, 'id', '') == 't' and len(node.args) == 2:
                english = ast.unparse(node.args[1])
                self.assertIsNone(HANGUL.search(english), english)
                pairs += 1
        for korean, english in setup.PRIVILEGE_PURPOSE.values():
            self.assertTrue(HANGUL.search(korean) and not HANGUL.search(english))
        self.assertGreater(pairs, 150)


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    unittest.main()
