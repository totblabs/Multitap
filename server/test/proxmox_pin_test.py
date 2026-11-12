"""This node's own certificate pinned when it lacks the address; never runs a Proxmox command."""
import base64
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pin_setup', Path(__file__).resolve().parents[1] / 'setup' / 'proxmox.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)

TOKEN = 'multitap-fixture@pve!broker'
SECRET = 'fixture-secret-not-production'


def certificate(directory, name, san):
    cert, key = str(Path(directory) / (name + '.pem')), str(Path(directory) / (name + '-key.pem'))
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1',
                    '-subj', '/CN=' + san.split(':', 1)[1].split(',')[0], '-addext', 'subjectAltName=' + san], capture_output=True, check=True)
    return cert, key


class FakeProxmox:
    """A local HTTPS server: Proxmox-like (401 JSON from pve-api-daemon) or a management board (HTML)."""
    def __init__(self, cert, key, kind='proxmox'):
        seen = self.seen = []
        class Handler(BaseHTTPRequestHandler):
            server_version = 'pve-api-daemon/3.0' if kind == 'proxmox' else 'Mini-BMC'
            sys_version = ''
            def log_message(self, *args):
                pass
            def do_GET(self):
                seen.append((self.path, self.headers.get('Authorization')))
                if kind != 'proxmox':
                    body, status, mime = b'<html>login</html>', 200, 'text/html'
                elif self.path == '/api2/json/version':
                    body, status, mime = b'{"data":null}', 401, 'application/json;charset=UTF-8'
                elif self.path.startswith('/api2/json/cluster/resources') and self.headers.get('Authorization') == 'PVEAPIToken=' + TOKEN + '=' + SECRET:
                    body, status, mime = b'{"data":[]}', 200, 'application/json;charset=UTF-8'
                else:
                    body, status, mime = b'{"data":null}', 401, 'application/json;charset=UTF-8'
                self.send_response(status)
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = 'https://127.0.0.1:' + str(self.server.server_address[1])
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


@unittest.skipUnless(shutil.which('openssl'), 'OpenSSL TLS fixture required')
class PinTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='multitap-pin-')
        # The served certificate names another host: 127.0.0.1 fails the name check (code 64).
        cls.node = certificate(cls.directory.name, 'node', 'DNS:pve.other.example')
        cls.other = certificate(cls.directory.name, 'other', 'DNS:pve.other.example')
        cls.named = certificate(cls.directory.name, 'named', 'DNS:localhost,IP:127.0.0.1')
        cls.missing = str(Path(cls.directory.name) / 'absent.pem')
        der = ssl.PEM_cert_to_DER_cert(Path(cls.node[0]).read_text())
        cls.fingerprint = ':'.join('%02X' % b for b in hashlib.sha256(der).digest())

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def local(self, *files):
        return patch.object(setup, 'LOCAL_CERTIFICATES', files)

    def test_this_nodes_address_pins_its_exact_certificate_and_the_api_check_uses_the_pin(self):
        with FakeProxmox(*self.node) as fake, self.local(self.missing, self.node[0]):
            trust = setup.check_address(fake.url, '')
            self.assertEqual(trust.fingerprint, self.fingerprint)
            self.assertRegex(trust.fingerprint, r'^[0-9A-F]{2}(?::[0-9A-F]{2}){31}$')
            self.assertEqual(ssl.PEM_cert_to_DER_cert(trust.ca), ssl.PEM_cert_to_DER_cert(Path(self.node[0]).read_text()))
            self.assertFalse(trust.context.check_hostname)
            self.assertEqual(trust.context.verify_mode, ssl.CERT_REQUIRED)
            # /version was asked without a token; the token went only to the pinned API check.
            self.assertEqual(fake.seen, [('/api2/json/version', None)])
            setup.verify_api(fake.url, trust, TOKEN, SECRET)
            self.assertEqual(fake.seen[-1][1], 'PVEAPIToken=' + TOKEN + '=' + SECRET)
            # Another fingerprint with the same trust anchor: the token is never sent.
            before = len(fake.seen)
            with self.assertRaises(setup.SetupError):
                setup.verify_api(fake.url, trust._replace(fingerprint='00:' * 31 + '00'), TOKEN, SECRET)
            self.assertEqual(len(fake.seen), before)

    def test_a_ca_issued_node_certificate_is_its_own_trust_anchor(self):
        # A custom/ACME certificate: issued by a CA this script does not trust.
        directory = self.directory.name
        ca_cert, ca_key = certificate(directory, 'issuer', 'DNS:issuer.example')
        leaf, key = str(Path(directory) / 'issued.pem'), str(Path(directory) / 'issued-key.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', leaf, '-days', '1', '-subj', '/CN=pve.issued.example',
                        '-addext', 'subjectAltName=DNS:pve.issued.example', '-addext', 'basicConstraints=critical,CA:FALSE', '-CA', ca_cert, '-CAkey', ca_key], capture_output=True, check=True)
        with FakeProxmox(leaf, key) as fake, self.local(leaf):
            trust = setup.check_address(fake.url, '')
            self.assertEqual(trust.fingerprint, setup.certificate_fingerprint(ssl.PEM_cert_to_DER_cert(Path(leaf).read_text())))
            setup.verify_api(fake.url, trust, TOKEN, SECRET)

    def test_the_custom_certificate_file_comes_first(self):
        with FakeProxmox(*self.node) as fake:
            with self.local(self.node[0], self.other[0]):
                self.assertEqual(setup.trust_for(fake.url, '').fingerprint, self.fingerprint)
            with self.local(self.other[0], self.node[0]), self.assertRaisesRegex(setup.SetupError, '노드의 인증서와 다릅니다'):
                setup.trust_for(fake.url, '')

    def test_another_certificate_at_this_nodes_address_is_refused(self):
        with FakeProxmox(*self.node) as fake, self.local(self.other[0]):
            with self.assertRaises(setup.AddressError) as caught:
                setup.trust_for(fake.url, '')
            self.assertIn('다른 장치가 응답하고 있을 수 있어', str(caught.exception))
            self.assertEqual(fake.seen, [], 'nothing was asked over the unverified handshake')

    def test_an_address_of_another_machine_keeps_the_name_rule(self):
        with FakeProxmox(*self.node) as fake, self.local(self.node[0]), patch.object(setup, 'is_this_host', return_value=False):
            # Trusted through the CA file, only the name differs (verify code 64).
            with self.assertRaises(setup.AddressError) as caught:
                setup.trust_for(fake.url, Path(self.node[0]).read_text())
            self.assertEqual(caught.exception.short, '인증서의 주소가 달라서 쓸 수 없었습니다.')
            self.assertIn('인증서의 주소와 입력한 IP/호스트 이름이 다릅니다', str(caught.exception))

    def test_https_that_is_not_proxmox_is_named_before_any_token_is_sent(self):
        ca = Path(self.named[0]).read_text()
        with FakeProxmox(*self.named, kind='bmc') as fake:
            with self.assertRaises(setup.AddressError) as caught:
                setup.check_address(fake.url, ca)
            self.assertIn('이 주소는 Proxmox가 아닙니다(HTTP 200, text/html)', str(caught.exception))
            self.assertEqual(fake.seen, [('/api2/json/version', None)])
        with FakeProxmox(*self.named) as fake:
            trust = setup.check_address(fake.url, ca)
            self.assertEqual((trust.ca, trust.fingerprint), (ca, ''), 'a verified name keeps the CA path and no pin')


class BlockAndFlowTests(unittest.TestCase):
    FP = 'AB:' * 31 + 'CD'
    PEM = '-----BEGIN CERTIFICATE-----\nZml4dHVyZQ==\n-----END CERTIFICATE-----\n'

    def decode(self, block):
        lines = block.splitlines()
        return json.loads(base64.b64decode(''.join(lines[1:-1])))

    def test_connection_block_carries_the_pin_only_when_pinned(self):
        pinned = self.decode(setup.connection_block('https://10.0.0.5:8006', 'multitap-fixture@pve', 'broker', SECRET, self.PEM, 2000000000, self.FP))
        self.assertEqual(set(pinned), {'endpoint', 'tokenId', 'secret', 'ca', 'expiresAt', 'fingerprint'})
        self.assertEqual((pinned['fingerprint'], pinned['ca']), (self.FP, self.PEM))
        plain = self.decode(setup.connection_block('https://pve.example:8006', 'multitap-fixture@pve', 'broker', SECRET, '', 2000000000))
        self.assertEqual(set(plain), {'endpoint', 'tokenId', 'secret', 'ca', 'expiresAt'})
        for bad, ca in ((self.FP.lower(), self.PEM), (self.FP[:-3], self.PEM), (self.FP, '')):
            with self.assertRaises(setup.SetupError):
                setup.connection_block('https://10.0.0.5:8006', 'multitap-fixture@pve', 'broker', SECRET, ca, None, bad)

    def flow(self, address, results, answers=()):
        asked, checked = [], []
        answers = list(answers)
        class Tty:
            def ask(self, label, default=''):
                asked.append(label)
                return answers.pop(0)
        def check(url, ca):
            checked.append(url)
            result = results[url]
            if isinstance(result, Exception):
                raise result
            return result
        output = io.StringIO()
        with patch.object(setup, 'BROKER_ORIGIN', 'https://broker.example'), patch.object(setup, 'check_address', side_effect=check), \
                patch.object(setup, 'default_endpoint', return_value='https://10.0.0.5:8006') as found, contextlib.redirect_stdout(output):
            chosen = setup.choose_endpoint(Tty(), address, '')
        return chosen, asked, checked, output.getvalue(), found.called

    def test_a_working_automatic_address_is_used_without_asking(self):
        trust = setup.Trust('', None, '')
        chosen, asked, checked, output, _ = self.flow('', {'https://10.0.0.5:8006': trust})
        self.assertEqual(chosen, ('https://10.0.0.5:8006', trust))
        self.assertEqual(asked, [])
        self.assertIn('Proxmox 주소: https://10.0.0.5:8006 (자동으로 찾음)', output)

    def test_a_pinned_automatic_address_says_what_it_remembers(self):
        trust = setup.Trust(self.PEM, None, self.FP)
        _, asked, _, output, _ = self.flow('', {'https://10.0.0.5:8006': trust})
        self.assertEqual(asked, [])
        self.assertIn(self.FP, output)
        self.assertIn('인증서가 바뀌면 이 명령을 다시 실행하세요', output)

    def test_a_failing_automatic_address_says_why_in_one_line_and_asks(self):
        failure = setup.AddressError('long text', '인증서의 주소가 달라서 쓸 수 없었습니다.')
        trust = setup.Trust('', None, '')
        chosen, asked, checked, output, _ = self.flow('', {'https://10.0.0.5:8006': failure, 'https://pve.example.com': trust}, ['pve.example.com'])
        self.assertEqual(chosen, ('https://pve.example.com', trust))
        self.assertEqual(len(asked), 1)
        self.assertIn('⚠ 자동으로 시도한 주소: https://10.0.0.5:8006\n  인증서의 주소가 달라서 쓸 수 없었습니다.\n', output)
        self.assertEqual(checked, ['https://10.0.0.5:8006', 'https://pve.example.com'])

    def test_an_address_given_on_the_command_line_is_used_without_asking(self):
        trust = setup.Trust('', None, '')
        chosen, asked, checked, output, found = self.flow('https://pve.example.com', {'https://pve.example.com': trust})
        self.assertEqual((chosen, asked, checked), (('https://pve.example.com', trust), [], ['https://pve.example.com']))
        self.assertFalse(found, 'the automatic address is not tried')
        self.assertIn('Proxmox 주소: https://pve.example.com (명령에서 지정)', output)

    def test_a_failing_given_address_falls_back_to_asking(self):
        failure = setup.AddressError('long text', 'Proxmox가 아닙니다(HTTP 200, text/html).')
        trust = setup.Trust('', None, '')
        chosen, asked, checked, output, _ = self.flow('https://bmc.example', {'https://bmc.example': failure, 'https://pve.example.com': trust}, ['https://pve.example.com'])
        self.assertEqual(chosen, ('https://pve.example.com', trust))
        self.assertEqual(len(asked), 1)
        self.assertIn('⚠ 지정한 주소: https://bmc.example\n  Proxmox가 아닙니다(HTTP 200, text/html).\n', output)
        # A malformed given address is checked like a typed one.
        _, asked, checked, output, _ = self.flow('http://pve.example.com', {'https://pve.example.com': trust}, ['https://pve.example.com'])
        self.assertEqual((len(asked), checked), (1, ['https://pve.example.com']))

    def test_main_passes_the_address_only_to_a_new_connection(self):
        class Tty:
            def __enter__(self):
                return self
            def __exit__(self, *_):
                pass
        # main() picks the language from the locale: each run keeps it to itself.
        base = [patch.object(setup, 'LANGUAGE', setup.LANGUAGE), patch.object(setup.os, 'geteuid', return_value=0, create=True), patch.object(setup.os, 'access', return_value=True), patch.object(setup, 'Terminal', return_value=Tty())]
        with contextlib.ExitStack() as stack:
            for item in base:
                stack.enter_context(item)
            enroll = stack.enter_context(patch.object(setup, 'enroll'))
            stack.enter_context(patch.object(sys, 'argv', ['-', 'https://pve.example.com']))
            setup.main()
            self.assertEqual(enroll.call_args.args[1], 'https://pve.example.com')
        with contextlib.ExitStack() as stack:
            for item in base:
                stack.enter_context(item)
            stack.enter_context(patch.object(sys, 'argv', ['-', '--renew', TOKEN, 'https://pve.example.com']))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            with self.assertRaises(SystemExit) as error:
                setup.main()
            self.assertEqual(error.exception.code, 2)

    def full_run(self, tty_output):
        class Out(io.StringIO):
            def isatty(self):
                return tty_output
        class Tty:
            def __init__(self):
                self.answers = ['', 'y']
            def ask(self, label, default=''):
                return self.answers.pop(0) or default
        output = Out()
        trust = setup.Trust(self.PEM, None, self.FP)
        with patch.object(setup, 'BROKER_ORIGIN', 'http://192.168.7.50:18480'), patch.object(setup, 'default_endpoint', return_value='https://192.168.7.2:8006'), \
                patch.object(setup, 'check_address', return_value=trust), patch.object(setup, 'inventory', return_value=[{'vmid': 101}, {'vmid': 102}]), \
                patch.object(setup, 'check_new_identity'), patch.object(setup, 'agent_privilege', return_value='VM.GuestAgent.Audit'), \
                patch.object(setup, 'provision', return_value=SECRET), patch.object(setup.secrets, 'token_hex', return_value='0a1b2c3d4e5f'), \
                patch.object(setup.time, 'time', return_value=1767225600), patch.dict(setup.os.environ, {'TERM': 'xterm'}), patch.object(sys, 'stdout', output):
            setup.os.environ.pop('NO_COLOR', None)
            setup.enroll(Tty())
        return output.getvalue()

    def test_a_full_run_reads_in_numbered_steps_and_the_copy_block_stays_plain(self):
        plain = self.full_run(False)
        self.assertNotIn('\x1b', plain)
        order = ['▶ Multitap Proxmox 연결', '[1/3] Proxmox 주소', '✔ Proxmox 주소: https://192.168.7.2:8006 (자동으로 찾음)', '⚠ 인증서에 이 주소가 없어',
                 '✔ Proxmox에서 VM 2개를 확인했습니다.', '[2/3] 연동 계정', '권한: VM.Audit (VM 상태·Host 찾기), VM.PowerMgmt (전원), VM.Console (긴급 Console), VM.GuestAgent.Audit (Windows 켜짐 확인)',
                 '✔ 연동 계정·토큰을 만들었습니다.', '✔ 새 토큰으로 Proxmox API를 확인했습니다.', '[3/3] 연결 정보',
                 '▼ 아래 MULTITAP-CONNECTION-V1:부터 END-MULTITAP-CONNECTION까지 복사해서 관리자 페이지 2번 칸에 붙여 넣으세요\nMULTITAP-CONNECTION-V1:\n',
                 'END-MULTITAP-CONNECTION\n▲ 여기까지', '요약', '토큰 만료일: 2027-01-01T00:00:00Z', '데스크톱 목록에는 Host를 설치하고 승인한 VM만 나옵니다.', '→ 관리자 페이지에서 연결 설정을 붙여넣고 ‘연결하기’를 누르세요.']
        position = 0
        for text in order:
            position = plain.index(text, position) + len(text)  # each after the one before
        block = plain[plain.index('MULTITAP-CONNECTION-V1:\n'):plain.index('END-MULTITAP-CONNECTION\n') + len('END-MULTITAP-CONNECTION')]
        self.assertEqual(self.decode(block)['fingerprint'], self.FP)
        coloured = self.full_run(True)
        self.assertIn('\x1b[1;32m✔\x1b[0m', coloured)
        self.assertIn('\x1b[1;33m⚠\x1b[0m', coloured)
        self.assertIn('\x1b[1;36m[1/3] Proxmox 주소\x1b[0m', coloured)
        # The block a person copies carries no escape code, in colour as well.
        self.assertIn('\n' + block + '\n', coloured)
        self.assertTrue(all('\x1b' not in line for line in block.splitlines()))
        with patch.dict(setup.os.environ, {'NO_COLOR': '1'}):
            self.assertEqual(setup.painted('32', 'x', type('T', (), {'isatty': lambda self: True})()), 'x')
        with patch.dict(setup.os.environ, {'TERM': 'dumb'}):
            setup.os.environ.pop('NO_COLOR', None)
            self.assertEqual(setup.painted('32', 'x', type('T', (), {'isatty': lambda self: True})()), 'x')

    def test_this_host_is_its_own_addresses_and_name_only(self):
        with patch.object(setup, 'local_addresses', return_value=['10.0.0.5', '192.168.7.2']), patch.object(setup.socket, 'gethostname', return_value='pve1'), \
                patch.object(setup.socket, 'getfqdn', return_value='pve1.lan'), patch.object(setup.socket, 'getaddrinfo', return_value=[(2, 1, 6, '', ('10.0.0.9', 0))]):
            self.assertTrue(setup.is_this_host('10.0.0.5'))
            self.assertTrue(setup.is_this_host('192.168.7.2'))
            self.assertTrue(setup.is_this_host('127.0.0.1'))
            self.assertTrue(setup.is_this_host('PVE1.lan.'))
            self.assertFalse(setup.is_this_host('10.0.0.6'))
            self.assertFalse(setup.is_this_host('pve2.lan'), 'a name of another address is not this host')

    def test_the_automatic_address_is_the_one_toward_the_multitap_server(self):
        class Probe:
            def __init__(self, *args):
                pass
            def __enter__(self):
                return self
            def __exit__(self, *_):
                pass
            def connect(self, target):
                self.target = target
            def getsockname(self):
                return ('192.168.7.2', 40000)
        with patch.object(setup, 'BROKER_ORIGIN', 'http://192.168.7.50:18480'), patch.object(setup, 'local_addresses', return_value=['10.0.0.5', '192.168.7.2']), \
                patch.object(setup.socket, 'socket', Probe), patch.object(setup.socket, 'gethostbyname', return_value='10.0.0.5'):
            self.assertEqual(setup.default_endpoint(), 'https://192.168.7.2:8006')
        with patch.object(setup, 'BROKER_ORIGIN', ''), patch.object(setup, 'local_addresses', return_value=['10.0.0.5', '192.168.7.2']), \
                patch.object(setup.socket, 'gethostbyname', return_value='192.168.7.2'):
            self.assertEqual(setup.default_endpoint(), 'https://192.168.7.2:8006', 'the address its own name resolves to')


if __name__ == '__main__':
    unittest.main()
