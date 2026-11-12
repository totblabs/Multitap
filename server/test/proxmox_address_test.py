"""Enrollment diagnostics/renewal fixtures; never runs a real Proxmox command."""
import base64
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('address_setup', Path(__file__).resolve().parents[1] / 'setup' / 'proxmox.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class AddressTests(unittest.TestCase):
    def test_own_broker_is_not_a_proxmox_endpoint_and_empty_template_is_not_a_real_install(self):
        with patch.object(setup, 'BROKER_ORIGIN', 'https://broker.example'):
            with self.assertRaisesRegex(setup.SetupError, '로그인 서버'):
                setup.proxmox_endpoint('https://broker.example/')
            self.assertEqual(setup.proxmox_endpoint('https://192.0.2.2:8006'), 'https://192.0.2.2:8006')
        with self.assertRaisesRegex(setup.SetupError, '관리자 페이지'):
            setup.enroll(None)

    def test_copy_payload_is_plain_and_expiry_survives_the_connection_block(self):
        expiry = 2000000000
        block = setup.connection_block('https://pve.example', 'multitap-fixture@pve', 'broker', 'fixture-not-production', '', expiry)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            setup.print_copy_block(block)
        text = output.getvalue()
        self.assertIn('▼ 아래 MULTITAP-CONNECTION-V1:부터 END-MULTITAP-CONNECTION까지 복사해서 관리자 페이지 2번 칸에 붙여 넣으세요\n' + block + '\n▲ 여기까지\n', text)
        payload = text[text.index('\nMULTITAP-CONNECTION-V1:') + 1:text.index('END-MULTITAP-CONNECTION\n')]
        self.assertNotIn('\x1b', payload)
        decoded = json.loads(base64.b64decode(''.join(payload.splitlines()[1:])))
        self.assertEqual(decoded['expiresAt'], setup.expiry_iso(expiry))

    @unittest.skipUnless(shutil.which('openssl'), 'OpenSSL TLS fixture required')
    def test_internal_ip_is_accepted_with_matching_certificate_and_ca(self):
        # Actual verified TLS on loopback, including rejection without the CA.
        with tempfile.TemporaryDirectory(prefix='multitap-address-') as directory:
            cert, key = str(Path(directory) / 'cert.pem'), str(Path(directory) / 'key.pem')
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], capture_output=True, check=True)
            ca = Path(cert).read_text()
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(0.2)
            endpoint = 'https://127.0.0.1:' + str(listener.getsockname()[1])
            stop = threading.Event()
            def serve():
                while not stop.is_set():
                    try:
                        peer, _ = listener.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    try:
                        with context.wrap_socket(peer, server_side=True):
                            pass
                    except ssl.SSLError:
                        peer.close()
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            try:
                with self.assertRaisesRegex(setup.SetupError, '신뢰'):
                    setup.trust_for(endpoint, '')
                exported, client_context, fingerprint = setup.trust_for(endpoint, ca)
                self.assertEqual(exported, ca)
                self.assertEqual(fingerprint, '', 'a verified name needs no pin')
                self.assertTrue(client_context.check_hostname)
                self.assertEqual(client_context.verify_mode, ssl.CERT_REQUIRED)
            finally:
                stop.set()
                listener.close()
                thread.join(timeout=2)

    def test_renewal_only_extends_the_owned_identity_and_token_without_new_secret(self):
        calls = []
        def runner(command, stage):
            calls.append(command)
            if command[1:3] == ['user', 'list']:
                return json.dumps([{'userid': 'multitap-fixture@pve', 'enable': 1, 'expire': 1900000000, 'comment': 'Multitap all-VM API integration'}])
            if command[1:4] == ['user', 'token', 'list']:
                return json.dumps([{'tokenid': 'broker', 'expire': 1900000000}])
            return ''
        expiry = int(time.time()) + 365 * 86400
        value = setup.renew_identity('multitap-fixture@pve!broker', expiry, runner)
        self.assertEqual(value, {'tokenId': 'multitap-fixture@pve!broker', 'expiresAt': setup.expiry_iso(expiry)})
        writes = [command for command in calls if 'modify' in command]
        self.assertEqual(len(writes), 2)
        self.assertTrue(all(command[-2:] == ['--expire', str(expiry)] for command in writes))
        self.assertFalse(any('add' in command or 'delete' in command or '--regenerate' in command for command in calls))
        with self.assertRaises(setup.SetupError):
            setup.renew_identity('root@pam!broker', expiry, runner)

    def test_renewal_failure_restores_the_previous_account_expiry(self):
        writes = []
        def runner(command, stage):
            if command[1:3] == ['user', 'list']:
                return json.dumps([{'userid': 'multitap-fixture@pve', 'expire': 1900000000, 'comment': 'Multitap all-VM API integration'}])
            if command[1:4] == ['user', 'token', 'list']:
                return json.dumps([{'tokenid': 'broker', 'expire': 1900000000}])
            writes.append(command)
            if command[1:4] == ['user', 'token', 'modify']:
                raise setup.SetupError('fixture failure')
            return ''
        with self.assertRaises(setup.SetupError):
            setup.renew_identity('multitap-fixture@pve!broker', int(time.time()) + 365 * 86400, runner)
        self.assertEqual(writes[-1], ['/usr/sbin/pveum', 'user', 'modify', 'multitap-fixture@pve', '--expire', '1900000000'])


if __name__ == '__main__':
    unittest.main()
