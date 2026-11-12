#!/usr/bin/env python3
"""Interactive Multitap enrollment. Run manually in a Proxmox root Shell.

Creates a new API identity scoped to the /vms tree. Never powers or reconfigures a VM.
No secret is written to a file or sent to Multitap by this program.
"""
import base64
import argparse
from collections import namedtuple
from datetime import datetime, timezone
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import socket
import ssl
import subprocess
import sys
import textwrap
import time
from urllib.parse import urlsplit
from urllib.request import Request

BROKER_ORIGIN = ''

# Language of every message (docs/i18n.md): main() sets it from --lang ko|en,
# else MULTITAP_LANG, else the locale (LC_ALL, LC_MESSAGES, LANG): Korean when
# it starts with ko, English otherwise. Korean is the source text and the
# default for code that imports this file without main() (the tests).
LANGUAGE = 'ko'


def t(ko, en):
    return en if LANGUAGE == 'en' else ko


def pick_language(argv, environ):
    chosen = ''
    for index, item in enumerate(argv):
        if item == '--lang' and index + 1 < len(argv):
            chosen = argv[index + 1]
        elif item.startswith('--lang='):
            chosen = item[len('--lang='):]
    for name in ('MULTITAP_LANG', 'LC_ALL', 'LC_MESSAGES', 'LANG'):
        chosen = chosen or environ.get(name, '')
    return 'ko' if chosen.lower().startswith('ko') else 'en'


# Identities this helper creates. Renewal and the console permission work only
# for them; ownership is proven by the comment, role and ACL shape.
OWN_USER = r'multitap-[A-Za-z0-9_.-]{2,40}@pve'
OWN_ROLE = r'Multitap-[a-f0-9]{12}'
ALL_VM_COMMENTS = ('Multitap all-VM API integration',)
SCOPED_COMMENTS = ('Multitap VM-scoped API integration',)


class SetupError(Exception):
    pass


class AddressError(SetupError):
    """An address that cannot be used, with a one-line reason for the automatic attempt."""
    def __init__(self, message, short):
        super().__init__(message)
        self.short = short


class Terminal:
    """Separate streams work on Linux TTY devices, which are not seekable."""
    def __enter__(self):
        self.reader = None
        self.writer = None
        try:
            self.reader = open('/dev/tty', 'r', encoding='utf8')
            self.writer = open('/dev/tty', 'w', encoding='utf8', buffering=1)
        except OSError:
            self.__exit__(None, None, None)
            raise SetupError(t('질문에 답할 수 있는 Proxmox Shell에서 실행하세요.', 'Run this in a Proxmox Shell where you can answer questions.')) from None
        return self

    def __exit__(self, *_):
        for stream in (self.reader, self.writer):
            if stream is not None:
                stream.close()

    def ask(self, label, default=''):
        # The question stands out on a terminal; its words are the same everywhere.
        self.writer.write(painted('1;33', '? ', self.writer) + painted('1', label, self.writer) + (painted('2', ' [' + default + ']', self.writer) if default else '') + ': ')
        self.writer.flush()
        value = self.reader.readline()
        if not value:
            raise KeyboardInterrupt()
        return value.strip() or default


def set_password(user, terminal=None):
    if terminal is None:
        with Terminal() as opened:
            return set_password(user, opened)
    terminal.writer.write(t('\n연동 계정 비밀번호를 입력하고, 같은 비밀번호를 다시 입력하세요.\n입력 내용은 화면에 표시되지 않습니다.\n',
                            '\nEnter a password for the integration account, then enter it again.\nWhat you type is not shown.\n'))
    terminal.writer.flush()
    try:
        # pveum owns its hidden, confirmed password prompt. Neither Python's piped
        # stdin nor command arguments/environment/files ever contain the password.
        subprocess.run(['/usr/sbin/pveum', 'passwd', user], stdin=terminal.reader,
                       stdout=terminal.writer, stderr=terminal.writer, check=True)
    except (OSError, subprocess.SubprocessError):
        raise SetupError(t('비밀번호 설정에 실패했습니다. 새 연동 계정 생성을 취소합니다.', 'Setting the password failed. The new integration account is not created.')) from None


def run(command, stage):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=25, check=True)
        if len(result.stdout) > 4 * 1024 * 1024:
            raise SetupError(t(stage + ': 응답이 너무 큽니다.', stage + ': the response is too large.'))
        return result.stdout
    except (OSError, subprocess.SubprocessError):
        # CLI errors can contain credentials. Do not print raw stderr/stdout.
        raise SetupError(t(stage + ' 단계에서 실패했습니다. Proxmox 상태와 권한을 확인하세요.', 'Failed at this step: ' + stage + '. Check the Proxmox status and permissions.')) from None


def read_json(command, stage, runner=run):
    try:
        return json.loads(runner(command, stage))
    except (ValueError, TypeError):
        raise SetupError(t(stage + ': JSON 응답을 확인하지 못했습니다.', stage + ': could not read the JSON response.')) from None


def endpoint(value):
    value = value.strip()
    if '://' not in value:
        value = 'https://' + value
    try:
        url = urlsplit(value)
        if url.scheme != 'https' or not url.hostname or url.username or url.password or url.path not in ('', '/') or url.query or url.fragment:
            raise ValueError()
        port = url.port or 443
        if not 1 <= port <= 65535:
            raise ValueError()
    except ValueError:
        raise SetupError(t('https://Proxmox주소:8006 형식으로 입력하세요.', 'Enter it as https://<Proxmox address>:8006.')) from None
    return value.rstrip('/')


def local_addresses():
    """This node's own IP addresses (`hostname -I`), in the order it lists them."""
    found = []
    try:
        values = subprocess.run(['/bin/hostname', '-I'], capture_output=True, text=True, timeout=5, check=True).stdout.split()
    except (OSError, subprocess.SubprocessError):
        return found
    for value in values:
        try:
            address = str(ipaddress.ip_address(value))
        except ValueError:
            continue
        if address not in found:
            found.append(address)
    return found


def default_endpoint():
    """The address of this node that the Multitap server most likely reaches.

    First the address this node uses toward the Multitap server (a UDP connect
    sends nothing), then the address its own name resolves to (Proxmox keeps
    it in /etc/hosts), then the first one `hostname -I` lists.
    """
    addresses = [value for value in local_addresses()
                 if ipaddress.ip_address(value).version == 4 and not ipaddress.ip_address(value).is_loopback and not ipaddress.ip_address(value).is_link_local]
    preferred = []
    broker = urlsplit(BROKER_ORIGIN) if BROKER_ORIGIN else None
    if broker and broker.hostname:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.connect((broker.hostname, broker.port or 443))
                preferred.append(probe.getsockname()[0])
        except (OSError, ValueError):
            pass
    try:
        preferred.append(socket.gethostbyname(socket.gethostname()))
    except OSError:
        pass
    for value in preferred + addresses:
        if value in addresses:
            return 'https://' + value + ':8006'
    return 'https://' + socket.gethostname() + ':8006'


def is_this_host(host):
    """True when `host` names this Proxmox node: one of its own IPs, or its own name."""
    host = (host or '').strip('[]').rstrip('.').lower()
    if not host:
        return False
    addresses = set(local_addresses())
    try:
        address = ipaddress.ip_address(host)
        return address.is_loopback or str(address) in addresses
    except ValueError:
        pass
    if host in ('localhost', socket.gethostname().lower(), socket.getfqdn().lower()):
        return True
    try:
        resolved = {info[4][0] for info in socket.getaddrinfo(host, None)}
    except (OSError, UnicodeError):
        return False
    try:
        return bool(resolved) and all(ipaddress.ip_address(value).is_loopback or str(ipaddress.ip_address(value)) in addresses for value in resolved)
    except ValueError:
        return False


def proxmox_endpoint(value):
    url = endpoint(value)
    target, broker = urlsplit(url), urlsplit(BROKER_ORIGIN)
    if target.hostname == broker.hostname and (target.port or 443) == (broker.port or 443):
        raise SetupError(t('이 주소는 Multitap 로그인 서버입니다. VM을 관리하는 Proxmox 웹 주소를 입력하세요. 예: https://pve.example.com',
                           'This address is the Multitap sign-in server. Enter the Proxmox web address that manages the VMs, e.g. https://pve.example.com'))
    return url


def inventory(runner=run):
    result = read_json(['/usr/bin/pvesh', 'get', '/cluster/resources', '--type', 'vm', '--output-format', 'json'], t('VM 목록 조회', 'Listing VMs'), runner)
    if not isinstance(result, list):
        raise SetupError(t('VM 목록 형식이 올바르지 않습니다.', 'The VM list has an unexpected format.'))
    return [vm for vm in result if isinstance(vm, dict) and vm.get('type') == 'qemu' and not vm.get('template') and isinstance(vm.get('vmid'), int) and 100 <= vm['vmid'] <= 999999999]


def check_new_identity(user, role, runner=run):
    users = read_json(['/usr/sbin/pveum', 'user', 'list', '--output-format', 'json'], t('기존 사용자 확인', 'Checking existing users'), runner)
    roles = read_json(['/usr/sbin/pveum', 'role', 'list', '--output-format', 'json'], t('기존 역할 확인', 'Checking existing roles'), runner)
    if not isinstance(users, list) or not isinstance(roles, list):
        raise SetupError(t('기존 사용자·역할 목록을 확인하지 못했습니다.', 'Could not read the existing users and roles.'))
    if any(item.get('userid') == user for item in users) or any(item.get('roleid') == role for item in roles):
        raise SetupError(t('같은 이름의 계정 또는 역할이 있습니다. 기존 항목을 덮어쓰지 않습니다. 다시 실행해 다른 이름을 선택하세요.',
                           'An account or role with this name already exists. It is not overwritten. Run again and choose another name.'))


def rollback(user, role, token, created, runner=run):
    failed = False
    if created.get('user'):
        principals = []
        if created.get('user_acl'):
            principals.append((user, '--users'))
        if created.get('token_acl'):
            principals.append((user + '!' + token, '--tokens'))
        for principal, flag in principals:
            try:
                runner(['/usr/sbin/pveum', 'acl', 'delete', '/vms', flag, principal, '--roles', role], t('이번 실행의 VM 권한 정리', 'Removing the VM permissions of this run'))
            except SetupError:
                failed = True
        if created.get('token'):
            try:
                runner(['/usr/sbin/pveum', 'user', 'token', 'delete', user, token], t('이번 실행의 토큰 정리', 'Removing the token of this run'))
            except SetupError:
                failed = True
        try:
            runner(['/usr/sbin/pveum', 'user', 'delete', user], t('이번 실행의 사용자 정리', 'Removing the user of this run'))
        except SetupError:
            failed = True
    if created.get('role'):
        try:
            runner(['/usr/sbin/pveum', 'role', 'delete', role], t('이번 실행의 역할 정리', 'Removing the role of this run'))
        except SetupError:
            failed = True
    return not failed


def agent_privilege(runner=run):
    """The QEMU guest-agent read privilege of the running Proxmox VE, or None.

    PVE 9 removed VM.Monitor and added VM.GuestAgent.Audit; PVE 8 has no
    VM.GuestAgent.* privileges and needs VM.Monitor. An unreadable or unknown
    version returns None so the caller keeps the base privileges and says so.
    """
    try:
        text = runner(['/usr/bin/pveversion'], t('Proxmox 버전 확인', 'Checking the Proxmox version'))
    except SetupError:
        return None
    match = re.match(r'pve-manager/(\d+)\.', str(text).strip())
    if not match:
        return None
    major = int(match.group(1))
    if major >= 9:
        return 'VM.GuestAgent.Audit'
    return 'VM.Monitor' if major == 8 else None


def role_privs(privilege):
    # VM.Audit: list, status and config. VM.PowerMgmt: start, shutdown, reboot.
    # VM.Console: the emergency console (vncproxy). `privilege`: the guest-agent
    # read of this Proxmox version. Nothing else: Multitap does not change VM
    # hardware (an audio device is added in Proxmox by its administrator).
    return 'VM.Audit VM.PowerMgmt VM.Console' + (' ' + privilege if privilege else '')


def provision(user, role, token, expiry, runner=run, verify=None, password_setter=set_password, privilege=None):
    if not re.fullmatch(r'multitap-[a-zA-Z0-9_.-]{2,40}@pve', user) or not re.fullmatch(r'Multitap-[a-f0-9]{12}', role) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.-]{1,31}', token):
        raise SetupError(t('Multitap 전용 계정 이름을 확인하세요.', 'Check the name of the Multitap account.'))
    check_new_identity(user, role, runner)
    created = {}
    try:
        runner(['/usr/sbin/pveum', 'role', 'add', role, '--privs', role_privs(privilege)], t('전용 역할 생성', 'Creating the role'))
        created['role'] = True
        runner(['/usr/sbin/pveum', 'user', 'add', user, '--enable', '1', '--expire', str(expiry), '--comment', 'Multitap all-VM API integration'], t('전용 사용자 생성', 'Creating the user'))
        created['user'] = True
        password_setter(user)
        runner(['/usr/sbin/pveum', 'acl', 'modify', '/vms', '--users', user, '--roles', role, '--propagate', '1'], t('사용자 VM 권한 설정', "Setting the user's VM permissions"))
        created['user_acl'] = True
        result = read_json(['/usr/sbin/pveum', 'user', 'token', 'add', user, token, '--privsep', '1', '--expire', str(expiry), '--output-format', 'json'], t('API 토큰 생성', 'Creating the API token'), runner)
        created['token'] = True
        if not isinstance(result, dict) or not isinstance(result.get('value'), str) or not re.fullmatch(r'[\x21-\x7e]{8,256}', result['value']):
            raise SetupError(t('토큰 Secret 응답을 확인하지 못했습니다.', 'Could not read the token secret from the response.'))
        secret = result['value']
        runner(['/usr/sbin/pveum', 'acl', 'modify', '/vms', '--tokens', user + '!' + token, '--roles', role, '--propagate', '1'], t('토큰 VM 권한 설정', "Setting the token's VM permissions"))
        created['token_acl'] = True
        if verify:
            verify(user + '!' + token, secret)
        return secret
    except (SetupError, KeyboardInterrupt):
        cleaned = rollback(user, role, token, created, runner)
        if not cleaned:
            print(t('\n이번 실행의 일부 항목 정리를 확인하지 못했습니다. Proxmox에서 아래 전용 항목을 확인하세요:\n사용자: ' + user + '\n역할: ' + role,
                    '\nCould not confirm that everything this run created was removed. Check these items in Proxmox:\nUser: ' + user + '\nRole: ' + role), file=sys.stderr)
        raise


# How the Multitap server will check this Proxmox address: `ca` (PEM, may be
# empty for public trust) and `context` for this script, and `fingerprint`
# (SHA-256 of the DER leaf, uppercase hex with colons, as Proxmox shows it)
# only when the address is pinned to this node's exact certificate.
Trust = namedtuple('Trust', ['ca', 'context', 'fingerprint'])

# This node's certificate as pveproxy serves it: a custom (uploaded or ACME)
# certificate replaces the self-signed one when it exists.
LOCAL_CERTIFICATES = ('/etc/pve/local/pveproxy-ssl.pem', '/etc/pve/local/pve-ssl.pem')
FINGERPRINT = re.compile(r'[0-9A-F]{2}(?::[0-9A-F]{2}){31}')
PEM_CERTIFICATE = re.compile(r'-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+?-----END CERTIFICATE-----')


def certificate_fingerprint(der):
    return ':'.join('%02X' % byte for byte in hashlib.sha256(der).digest())


def local_certificate():
    """DER of the first certificate in this node's served certificate file, or None."""
    for name in LOCAL_CERTIFICATES:
        path = Path(name)
        if not path.is_file():
            continue
        try:
            match = PEM_CERTIFICATE.search(path.read_text(encoding='ascii'))
            return ssl.PEM_cert_to_DER_cert(match.group(0)) if match else None
        except (OSError, ValueError, UnicodeError):
            return None
    return None


def served_certificate(url):
    """The leaf certificate presented at `url`. Read only: nothing is sent after the handshake."""
    target = urlsplit(url)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    host = target.hostname
    try:
        ipaddress.ip_address(host)
        name = None
    except ValueError:
        name = host
    with socket.create_connection((host, target.port or 443), timeout=6) as sock:
        with context.wrap_socket(sock, server_hostname=name) as tls:
            return tls.getpeercert(binary_form=True)


def pinned_context(der):
    """Trusts exactly this one certificate, whatever names it carries."""
    context = ssl.create_default_context(cadata=ssl.DER_cert_to_PEM_cert(der))
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    # A CA-issued node certificate is the trust anchor itself; nothing above it is trusted.
    context.verify_flags = (context.verify_flags | ssl.VERIFY_X509_PARTIAL_CHAIN) & ~getattr(ssl, 'VERIFY_X509_STRICT', 0)
    return context


def check_pinned(url, context, fingerprint):
    target = urlsplit(url)
    try:
        ipaddress.ip_address(target.hostname)
        name = None
    except ValueError:
        name = target.hostname
    with socket.create_connection((target.hostname, target.port or 443), timeout=6) as sock:
        with context.wrap_socket(sock, server_hostname=name) as tls:
            if certificate_fingerprint(tls.getpeercert(binary_form=True)) != fingerprint:
                raise ssl.SSLError('certificate changed')


def pin_this_node(url):
    """Trust for an address of this node whose certificate lacks the address.

    Accepted only when the certificate served there is byte-identical to this
    node's own certificate file: the handshake proves the server holds its key.
    None when this node has no certificate file to compare with.
    """
    own = local_certificate()
    if not own:
        return None
    try:
        served = served_certificate(url)
    except (OSError, ssl.SSLError):
        served = None
    if not served or served != own:
        raise AddressError(t('이 주소에서 받은 인증서가 이 Proxmox 노드의 인증서와 다릅니다. 다른 장치가 응답하고 있을 수 있어 연결하지 않습니다.',
                             "The certificate at this address differs from this Proxmox node's certificate. Another device may be answering, so Multitap does not connect."),
                           t('받은 인증서가 이 노드의 인증서와 달라서 쓸 수 없었습니다.', "Not usable: the certificate differs from this node's certificate."))
    fingerprint = certificate_fingerprint(served)
    context = pinned_context(served)
    try:
        check_pinned(url, context, fingerprint)
    except (OSError, ssl.SSLError):
        raise AddressError(t('이 노드의 인증서로 다시 확인하지 못했습니다. 인증서가 만료되지 않았는지 확인하세요.',
                             "Could not check the address again with this node's certificate. Make sure the certificate has not expired."),
                           t('이 노드의 인증서로 확인하지 못했습니다.', "Could not check it with this node's certificate.")) from None
    return Trust(ssl.DER_cert_to_PEM_cert(served), context, fingerprint)


def trust_for(url, ca):
    target = urlsplit(url)
    failure = 'network'
    detail = ''
    for extra_ca in ('', ca) if ca else ('',):
        try:
            context = ssl.create_default_context()
            if extra_ca:
                context.load_verify_locations(cadata=extra_ca)
            with socket.create_connection((target.hostname, target.port or 443), timeout=6) as sock:
                with context.wrap_socket(sock, server_hostname=target.hostname):
                    return Trust(extra_ca, context, '')
        except ssl.SSLCertVerificationError as error:
            failure = ('name' if error.verify_code in (62, 64) else
                       'expired' if error.verify_code == 10 else 'trust')
            detail = re.sub(r'[\x00-\x1f\x7f-\x9f]', '', error.verify_message)[:180]
            continue
        except socket.gaierror:
            failure = 'dns'
            continue
        except (OSError, ssl.SSLError):
            failure = 'network'
            continue
    # A name or trust failure on this node's own address (an IP added after
    # installation, a second network card, a replaced certificate): pin this
    # node's exact certificate. Another machine's address keeps the rules above.
    if failure in ('name', 'trust') and is_this_host(target.hostname):
        pinned = pin_this_node(url)
        if pinned:
            return pinned
    messages = {
        'name': ('인증서의 주소와 입력한 IP/호스트 이름이 다릅니다. 브라우저에서 사용하는 Proxmox 도메인 주소를 입력하세요.',
                 "The certificate's address differs from the IP or host name you entered. Enter the Proxmox domain address you use in the browser."),
        'expired': ('Proxmox 인증서가 만료됐습니다. 유효한 인증서를 사용하는 Proxmox 관리 주소를 입력하세요.',
                    'The Proxmox certificate has expired. Enter a Proxmox management address with a valid certificate.'),
        'trust': ('Proxmox 인증서를 신뢰할 수 없습니다. 공인 인증서가 있는 관리 도메인을 입력하거나 Proxmox 클러스터 CA를 확인하세요.',
                  'The Proxmox certificate is not trusted. Enter a management domain with a public certificate, or check the Proxmox cluster CA.'),
        'dns': ('입력한 Proxmox 호스트 이름을 찾지 못했습니다. 도메인 철자와 DNS를 확인하세요.',
                'Could not find the Proxmox host name you entered. Check the spelling of the domain and DNS.'),
        'network': ('Proxmox HTTPS 주소·포트에 연결하지 못했습니다. 브라우저에서 사용하는 전체 Proxmox 주소를 입력하세요.',
                    'Could not connect to the Proxmox HTTPS address and port. Enter the full Proxmox address you use in the browser.'),
    }
    short = {
        'name': ('인증서의 주소가 달라서 쓸 수 없었습니다.', 'Not usable: the certificate names another address.'),
        'expired': ('인증서가 만료되어 쓸 수 없었습니다.', 'Not usable: the certificate has expired.'),
        'trust': ('인증서를 신뢰할 수 없어서 쓸 수 없었습니다.', 'Not usable: the certificate is not trusted.'),
        'dns': ('주소를 찾지 못했습니다.', 'The address was not found.'),
        'network': ('연결하지 못했습니다.', 'Could not connect.'),
    }
    raise AddressError(t(*messages[failure]) + (t('\n인증서 확인 내용: ', '\nCertificate check: ') + detail if detail else '') +
                       t('\n내부 주소도 사용할 수 있습니다. 인증서의 IP/이름과 CA가 일치해야 합니다.',
                         "\nAn internal address works too, when the certificate's IP or name and its CA match."), t(*short[failure]))


def opener_for(trust):
    """HTTPS without redirects, checked like the Multitap server will check it."""
    from urllib.request import build_opener, HTTPSHandler, HTTPRedirectHandler
    class NoRedirect(HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    context = trust.context if trust is not None else None
    fingerprint = trust.fingerprint if trust is not None else ''
    if not fingerprint:
        return build_opener(HTTPSHandler(context=context), NoRedirect())
    class PinnedConnection(http.client.HTTPSConnection):
        def connect(self):
            super().connect()
            if certificate_fingerprint(self.sock.getpeercert(binary_form=True)) != fingerprint:
                self.sock.close()
                raise ssl.SSLError('certificate changed')
    class PinnedHandler(HTTPSHandler):
        def https_open(self, req):
            return self.do_open(PinnedConnection, req, context=context)
    return build_opener(PinnedHandler(context=context), NoRedirect())


def check_proxmox(url, trust):
    """Proxmox answers /api2/json/version without login: 401 (or 200) with a JSON body.

    Catches an address that speaks HTTPS but is something else, such as a
    server's management board (IPMI/BMC) on 443. Sends no token or password.
    """
    from urllib.error import HTTPError
    status, kind, body, server = 0, '', b'', ''
    try:
        with opener_for(trust).open(Request(url + '/api2/json/version', headers={'Accept': 'application/json'}), timeout=8) as response:
            status, kind, server, body = response.status, response.headers.get('Content-Type', ''), response.headers.get('Server', ''), response.read(65537)
    except HTTPError as error:
        status, kind, server = error.code, error.headers.get('Content-Type', '') if error.headers else '', error.headers.get('Server', '') if error.headers else ''
        try:
            body = error.read(65537)
        except OSError:
            body = b''
    except (OSError, ValueError):
        raise AddressError(t('Proxmox API 응답을 받지 못했습니다. 브라우저에서 사용하는 전체 Proxmox 주소를 입력하세요.', 'No answer from the Proxmox API. Enter the full Proxmox address you use in the browser.'),
                           t('Proxmox API 응답을 받지 못했습니다.', 'No answer from the Proxmox API.')) from None
    proxmox = False
    if status in (200, 401) and len(body) <= 65536:
        try:
            data = json.loads(body)
            proxmox = isinstance(data, dict) and 'data' in data
        except ValueError:
            proxmox = False
        proxmox = proxmox or server.startswith('pve-api-daemon')
    if not proxmox:
        seen = 'HTTP ' + str(status) + (', ' + re.sub(r'[^\x21-\x7e ]', '', kind.split(';')[0])[:40] if kind else '')
        raise AddressError(t('이 주소는 Proxmox가 아닙니다(' + seen + '). 서버 관리 보드(IPMI/BMC) 주소가 아닌지 확인하세요. Proxmox 웹 주소는 보통 https://주소:8006 입니다.',
                             'This address is not Proxmox (' + seen + '). Check that it is not a server management board (IPMI/BMC). The Proxmox web address is usually https://<address>:8006.'),
                           t('Proxmox가 아닙니다(' + seen + ').', 'Not Proxmox (' + seen + ').'))


def check_address(url, ca):
    """The trust for `url`, once its certificate is verified and Proxmox answers there."""
    trust = trust_for(url, ca)
    check_proxmox(url, trust)
    return trust


def verify_api(url, trust, token_id, secret):
    request = Request(url + '/api2/json/cluster/resources?type=vm', headers={'Authorization': 'PVEAPIToken=' + token_id + '=' + secret, 'Accept': 'application/json'})
    try:
        # No redirects: credentials must remain at the entered Proxmox origin.
        opener = opener_for(trust)
        with opener.open(request, timeout=8) as response:
            raw = response.read(4 * 1024 * 1024 + 1)
            if len(raw) > 4 * 1024 * 1024:
                raise ValueError()
            data = json.loads(raw)['data']
        if not isinstance(data, list) or any(not isinstance(vm, dict) for vm in data):
            raise ValueError()
        # /vms inheritance covers current and future VMs. Inventory may be empty
        # or change during setup, so do not require an earlier snapshot to match.
    except (OSError, ValueError, TypeError, KeyError):
        raise SetupError(t('새 토큰으로 VM 목록 조회를 확인하지 못했습니다.', 'Could not list the VMs with the new token.')) from None


def expiry_iso(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z')


def connection_block(url, user, token, secret, ca, expires_at=None, fingerprint=''):
    data = {'endpoint': url, 'tokenId': user + '!' + token, 'secret': secret, 'ca': ca}
    if fingerprint:
        # Pinned: `ca` is that one certificate. A server older than this field
        # refuses the whole block instead of connecting without the pin.
        if not FINGERPRINT.fullmatch(fingerprint) or not ca:
            raise SetupError(t('인증서 지문을 확인하지 못했습니다.', 'Could not check the certificate fingerprint.'))
        data['fingerprint'] = fingerprint
    if expires_at is not None:
        data['expiresAt'] = expiry_iso(expires_at)
    encoded = base64.b64encode(json.dumps(data, ensure_ascii=True, separators=(',', ':')).encode('utf8')).decode('ascii')
    return 'MULTITAP-CONNECTION-V1:\n' + '\n'.join(textwrap.wrap(encoded, 76)) + '\nEND-MULTITAP-CONNECTION'


# Terminal output. `curl … | python3` pipes stdin, but stdout is the terminal:
# colours only there, never with NO_COLOR or TERM=dumb, and never inside a
# block the person copies. The words alone carry the meaning.
def colored(stream=None):
    stream = stream or sys.stdout
    try:
        return stream.isatty() and not os.environ.get('NO_COLOR') and os.environ.get('TERM') != 'dumb'
    except (AttributeError, ValueError):
        return False


def painted(code, text, stream=None):
    return '\033[' + code + 'm' + text + '\033[0m' if colored(stream) else text


def indented(text):
    return '\n'.join('  ' + line for line in str(text).splitlines())


# A symbol and its text. On a terminal one more space: ✔ ✖ ⚠ are drawn wide
# and would touch the first letter.
def marked(code, symbol, text, stream=None):
    return painted(code, symbol, stream) + (' ' if colored(stream) else '') + ' ' + text


# "label: value" with a quiet label and a bold value; plain text unchanged.
def kv(label, value, code='1'):
    return painted('2', label + ':') + ' ' + painted(code, str(value))


def say_heading(text):
    if colored():
        print('\n' + painted('1;30;43', ' ▶ ' + text + ' '), flush=True)
    else:
        print('\n▶ ' + text, flush=True)


def say_step(number, total, text):
    print('\n' + painted('1;36', '[' + str(number) + '/' + str(total) + '] ' + text), flush=True)
    if colored():
        print(painted('2;36', '─' * 44), flush=True)


def say_ok(text):
    print(marked('1;32', '✔', text), flush=True)


def say_fail(text, reason=''):
    print(marked('1;31', '✖', painted('1;31', text)) + ('\n' + painted('2', indented(reason)) if reason else ''), flush=True)


def say_warn(text, detail=''):
    print(marked('1;33', '⚠', painted('33', text)) + ('\n' + painted('2', indented(detail)) if detail else ''), flush=True)


def say_next(text):
    print(marked('1;32', '→', painted('1', text)), flush=True)


def print_copy_block(block, where=None):
    where = where or t('관리자 페이지 2번 칸', 'box 2 in the admin console')
    first, last = block.splitlines()[0], block.splitlines()[-1]
    # One sentence per language: the order of its parts differs.
    hint = t('아래 ' + first + '부터 ' + last + '까지 복사해서 ' + where + '에 붙여 넣으세요',
             'Copy everything below from ' + first + ' to ' + last + ' and paste it into ' + where)
    if colored():
        print('\n' + painted('1;30;46', ' ▼ ' + hint + ' '))
    else:
        print('\n▼ ' + hint)
    print(block)
    end = t('여기까지', 'Up to here')
    print(painted('1;30;46', ' ▲ ' + end + ' ') if colored() else '▲ ' + end, flush=True)
    say_warn(t('위 내용에는 비밀 토큰이 들어 있을 수 있습니다. 다른 곳에 저장하거나 보내지 마세요.',
               'The text above can contain a secret token. Do not save it anywhere else or send it to anyone.'))


# What each privilege the role holds is for, in the admin page's words.
PRIVILEGE_PURPOSE = {'VM.Audit': ('VM 상태·Host 찾기', 'VM status, finding Hosts'), 'VM.PowerMgmt': ('전원', 'power'),
                     'VM.Console': ('긴급 Console', 'emergency Console'),
                     'VM.GuestAgent.Audit': ('Windows 켜짐 확인', 'checking that Windows is up'), 'VM.Monitor': ('Windows 켜짐 확인', 'checking that Windows is up')}


def privilege_lines(privilege):
    return [name + ' (' + t(*PRIVILEGE_PURPOSE.get(name, ('', ''))) + ')' for name in role_privs(privilege).split()]


def renew_identity(token_id, expiry, runner=run):
    match = re.fullmatch('(' + OWN_USER + r')!([A-Za-z][A-Za-z0-9_.-]{1,31})', token_id)
    if not match or type(expiry) is not int or expiry <= int(time.time()):
        raise SetupError(t('갱신할 Multitap 연동 계정과 토큰을 확인하세요.', 'Check the Multitap integration account and token to renew.'))
    user, token = match.groups()
    users = read_json(['/usr/sbin/pveum', 'user', 'list', '--output-format', 'json'], t('갱신 대상 사용자 확인', 'Checking the user to renew'), runner)
    current = next((item for item in users if isinstance(item, dict) and item.get('userid') == user), None) if isinstance(users, list) else None
    if not current or current.get('comment') not in ALL_VM_COMMENTS + SCOPED_COMMENTS or not current.get('enable', 1):
        raise SetupError(t('이 도우미로 만든 활성 Multitap 연동 계정만 갱신할 수 있습니다. 기존 계정은 변경하지 않았습니다.',
                           'Only an active Multitap integration account made by this helper can be renewed. The existing account was not changed.'))
    tokens = read_json(['/usr/sbin/pveum', 'user', 'token', 'list', user, '--output-format', 'json'], t('갱신 대상 토큰 확인', 'Checking the token to renew'), runner)
    previous_token = next((item for item in tokens if isinstance(item, dict) and item.get('tokenid') == token), None) if isinstance(tokens, list) else None
    if previous_token is None:
        raise SetupError(t('갱신 대상 토큰이 없습니다. 다른 토큰은 변경하지 않았습니다.', 'The token to renew does not exist. No other token was changed.'))
    previous_user_expiry = current.get('expire', 0)
    if type(previous_user_expiry) is not int or previous_user_expiry < 0:
        raise SetupError(t('기존 계정 유효기간을 확인하지 못했습니다. 변경하지 않았습니다.', "Could not read the account's current expiry. Nothing was changed."))
    runner(['/usr/sbin/pveum', 'user', 'modify', user, '--expire', str(expiry)], t('연동 계정 유효기간 갱신', "Extending the integration account's expiry"))
    try:
        runner(['/usr/sbin/pveum', 'user', 'token', 'modify', user, token, '--expire', str(expiry)], t('연동 토큰 유효기간 갱신', "Extending the integration token's expiry"))
    except (SetupError, KeyboardInterrupt):
        try:
            runner(['/usr/sbin/pveum', 'user', 'modify', user, '--expire', str(previous_user_expiry)], t('계정 유효기간 복구', "Restoring the account's expiry"))
        except SetupError:
            print(t('계정 유효기간 복구를 확인하지 못했습니다. Proxmox에서 ' + user + '의 만료일을 확인하세요.',
                    'Could not confirm that the account expiry was restored. Check the expiry date of ' + user + ' in Proxmox.'), file=sys.stderr)
        raise
    return {'tokenId': token_id, 'expiresAt': expiry_iso(expiry)}


def ensure_agent_privilege(token_id, runner=run):
    """Add the guest-agent read privilege to this integration's own Multitap-<hex> role.

    Returns the privilege name added (or already needed), or None when the
    version is unknown or no Multitap role is bound to the identity. Only a role
    named Multitap-<12 hex> that is bound at /vms to this user or token is touched.
    """
    privilege = agent_privilege(runner)
    if not privilege:
        return None
    user = token_id.split('!')[0]
    acls = read_json(['/usr/sbin/pveum', 'acl', 'list', '--output-format', 'json'], t('연동 역할 확인', 'Checking the integration role'), runner)
    roles = sorted({str(item.get('roleid')) for item in acls if isinstance(item, dict) and item.get('path') == '/vms' and item.get('ugid') in (user, token_id) and re.fullmatch(OWN_ROLE, str(item.get('roleid')))}) if isinstance(acls, list) else []
    if len(roles) != 1:
        return None
    runner(['/usr/sbin/pveum', 'role', 'modify', roles[0], '--append', '1', '--privs', privilege], t('게스트 에이전트 권한 추가', 'Adding the guest agent privilege'))
    return privilege


def console_identity_plan(token_id, runner=run, now=None):
    """Read-only proof for one unshared identity created by this all-VM helper.

    This is deliberately stricter than renewal. It never infers ownership from
    a role-name prefix alone, and never accepts a shared/group/extra-path ACL.
    """
    match = re.fullmatch('(' + OWN_USER + r')!([A-Za-z][A-Za-z0-9_.-]{1,31})', token_id) if isinstance(token_id, str) else None
    if not match:
        raise SetupError(t('콘솔 권한을 추가할 전용 Multitap 계정·토큰을 확인하세요.', 'Check the Multitap account and token to add the console permission to.'))
    user, token = match.groups()
    users = read_json(['/usr/sbin/pveum', 'user', 'list', '--output-format', 'json'], t('콘솔 대상 사용자 확인', 'Checking the user for the console permission'), runner)
    tokens = read_json(['/usr/sbin/pveum', 'user', 'token', 'list', user, '--output-format', 'json'], t('콘솔 대상 토큰 확인', 'Checking the token for the console permission'), runner)
    acls = read_json(['/usr/sbin/pveum', 'acl', 'list', '--output-format', 'json'], t('콘솔 권한 범위 확인', 'Checking the console permission scope'), runner)
    roles = read_json(['/usr/sbin/pveum', 'role', 'list', '--output-format', 'json'], t('콘솔 전용 역할 확인', 'Checking the role for the console permission'), runner)
    if any(not isinstance(rows, list) or len(rows) > 20000 or any(not isinstance(row, dict) for row in rows) for rows in (users, tokens, acls, roles)):
        raise SetupError(t('사용자·토큰·역할·ACL 전체 상태를 확인하지 못했습니다.', 'Could not read all users, tokens, roles and ACLs.'))
    current = [row for row in users if row.get('userid') == user]
    selected = [row for row in tokens if row.get('tokenid') == token]
    if len(current) != 1 or len(tokens) != 1 or len(selected) != 1:
        raise SetupError(t('정확히 하나의 전용 사용자·토큰 구성만 변경할 수 있습니다.', 'Only exactly one dedicated user with one token can be changed.'))
    current, selected = current[0], selected[0]
    if current.get('comment') not in ALL_VM_COMMENTS or type(current.get('enable')) not in (int, bool) or current['enable'] != 1 or current.get('groups') not in (None, '', []):
        raise SetupError(t('이 도우미가 만든 활성·그룹 미소속 전체 VM 연동 계정이 아닙니다.', 'This is not an active all-VM integration account made by this helper and outside any group.'))
    if type(selected.get('privsep')) not in (int, bool) or selected['privsep'] != 1:
        raise SetupError(t('권한 분리된 전용 토큰을 확인하지 못했습니다.', 'Could not find a dedicated token with separated privileges.'))
    now = int(time.time()) if now is None else now
    for row in (current, selected):
        expiry = row.get('expire', 0)
        if type(expiry) is not int or expiry < 0 or expiry and expiry <= now:
            raise SetupError(t('계정·토큰 유효기간을 먼저 별도 갱신하세요. 콘솔 작업은 만료일을 바꾸지 않습니다.',
                               'Renew the account and token expiry first, separately. Adding the console permission does not change the expiry.'))
    acl_fields = {'path', 'type', 'ugid', 'roleid', 'propagate'}
    if any(set(row) != acl_fields or not all(isinstance(row[key], str) for key in ('path', 'type', 'ugid', 'roleid')) or row['type'] not in ('user', 'token', 'group') or type(row['propagate']) not in (int, bool) or row['propagate'] not in (0, 1) for row in acls):
        raise SetupError(t('ACL 형식을 완전히 확인하지 못했습니다. 역할은 변경하지 않습니다.', 'Could not fully read the ACL format. The role is not changed.'))
    own_acls = [row for row in acls if row['ugid'] in (user, token_id)]
    role_ids = {row['roleid'] for row in own_acls}
    if len(own_acls) != 2 or len(role_ids) != 1:
        raise SetupError(t('사용자와 토큰의 단일 전용 역할 구성을 확인하지 못했습니다.', 'Could not confirm one dedicated role shared by the user and the token.'))
    role = next(iter(role_ids))
    if not re.fullmatch(OWN_ROLE, role):
        raise SetupError(t('전용 Multitap 역할이 아닙니다.', 'This is not a dedicated Multitap role.'))
    expected = {('user', user), ('token', token_id)}
    if {(row['type'], row['ugid']) for row in own_acls} != expected or any(row['path'] != '/vms' or row['propagate'] != 1 for row in own_acls):
        raise SetupError(t('사용자·토큰 양쪽의 /vms 상속 ACL만 허용합니다.', 'Only inherited /vms ACLs for both the user and the token are allowed.'))
    bound = [row for row in acls if row['roleid'] == role]
    if len(bound) != 2 or any(row not in own_acls for row in bound):
        raise SetupError(t('다른 계정·그룹·토큰·경로가 공유하는 역할은 변경하지 않습니다.', 'A role shared with another account, group, token or path is not changed.'))
    selected_roles = [row for row in roles if row.get('roleid') == role]
    if len(selected_roles) != 1 or selected_roles[0].get('special', 0) not in (0, False) or not isinstance(selected_roles[0].get('privs'), str):
        raise SetupError(t('전용 역할의 현재 권한을 확인하지 못했습니다.', 'Could not read the current privileges of the dedicated role.'))
    role_row = selected_roles[0]
    privileges = re.split(r'[,\s]+', role_row['privs'].strip())
    # VM.Config.HWType: roles made before the sound card left Multitap still hold it; it is accepted, never added.
    allowed = {'VM.Audit', 'VM.PowerMgmt', 'VM.Console', 'VM.Config.HWType', 'VM.GuestAgent.Audit', 'VM.Monitor'}
    if len(privileges) != len(set(privileges)) or not {'VM.Audit', 'VM.PowerMgmt'}.issubset(privileges) or not set(privileges).issubset(allowed):
        raise SetupError(t('기존 전용 역할 권한이 도우미 구성과 다릅니다. 다른 권한을 추측해 바꾸지 않습니다.',
                           "The dedicated role's privileges differ from what this helper sets up. Nothing is changed on a guess."))
    canonical = lambda rows: sorted(rows, key=lambda row: json.dumps(row, sort_keys=True, separators=(',', ':')))
    stable = {'users': canonical(users), 'tokens': canonical(tokens), 'acls': canonical(acls),
              'otherRoles': canonical([row for row in roles if row.get('roleid') != role]),
              'roleMetadata': {key: value for key, value in role_row.items() if key != 'privs'}}
    return {'tokenId': token_id, 'role': role, 'privileges': sorted(privileges),
            'proof': json.dumps(stable, sort_keys=True, separators=(',', ':'))}


def enable_console(tty, token_id, runner=run, now=None):
    plan = console_identity_plan(token_id, runner, now)
    print(t('\nMultitap 긴급 콘솔 권한 추가\n대상: ' + token_id + '\n역할: ' + plan['role'] +
            '\n범위: 사용자·토큰의 /vms 하위 상속\n추가 권한: VM.Console\n비밀번호·Secret·유효기간·기타 권한과 ACL은 변경하지 않습니다.',
            '\nAdd the Multitap emergency Console permission\nFor: ' + token_id + '\nRole: ' + plan['role'] +
            '\nScope: the user and the token, inherited below /vms\nPrivilege added: VM.Console\nPasswords, secrets, expiry, other privileges and ACLs are not changed.'), flush=True)
    if 'VM.Console' in plan['privileges']:
        print(t('이 전용 역할에는 이미 VM.Console 권한이 있습니다. 변경하지 않았습니다.', 'This role already has VM.Console. Nothing was changed.'), flush=True)
        return {'changed': False, 'role': plan['role']}
    if tty.ask(t('이 전용 역할에 콘솔 권한만 추가할까요? (y/N)', 'Add only the console permission to this role? (y/N)'), 'N').lower() not in ('y', 'yes'):
        print(t('취소했습니다. 변경한 항목이 없습니다.', 'Cancelled. Nothing was changed.'), flush=True)
        return {'changed': False, 'role': plan['role']}
    fresh = console_identity_plan(token_id, runner, now)
    if fresh != plan:
        raise SetupError(t('확인 중 계정·토큰·역할·ACL이 달라졌습니다. 변경하지 않았습니다.', 'The account, token, role or ACLs changed during the check. Nothing was changed.'))
    try:
        # PVE locks this append internally. CLI proof and append are not one
        # cluster transaction: never claim protection from concurrent root ACL
        # edits, and never remove privileges as a blind automatic rollback.
        runner(['/usr/sbin/pveum', 'role', 'modify', plan['role'], '--append', '1', '--privs', 'VM.Console'], t('전용 콘솔 권한 추가', 'Adding the console permission'))
        after = console_identity_plan(token_id, runner, now)
        if after['role'] != plan['role'] or after['proof'] != plan['proof'] or after['privileges'] != sorted(set(plan['privileges']) | {'VM.Console'}):
            raise SetupError(t('콘솔 권한 읽기 확인 불일치', 'Console permission read-back mismatch'))
    except (SetupError, KeyboardInterrupt):
        raise SetupError(t('콘솔 권한 추가 요청 후 완료 여부를 확인하지 못했습니다. Proxmox에서 전용 역할과 ACL을 확인하세요. 추가 변경이나 자동 복구는 하지 않았습니다.',
                           'The console permission was requested, but whether it finished could not be confirmed. Check the dedicated role and ACLs in Proxmox. Nothing more was changed or rolled back.')) from None
    print(t('전용 역할의 VM.Console 추가와 기존 설정 유지를 확인했습니다. 관리자 페이지에서 콘솔 권한을 다시 확인하세요.',
            'Confirmed: VM.Console was added to the role and the other settings are unchanged. Check the console permission again in the admin console.'), flush=True)
    return {'changed': True, 'role': plan['role']}


def renew_enrollment(tty, token_id, pve_url=''):
    expiry = int(time.time()) + 365 * 86400
    print(t('\nMultitap 연동 유효기간 갱신\n대상: ' + token_id + '\n새 만료일: ' + expiry_iso(expiry) + '\n계정과 해당 토큰의 유효기간만 연장합니다. 비밀번호·Secret·VM 권한은 유지합니다.',
            '\nRenew the Multitap integration\nFor: ' + token_id + '\nNew expiry: ' + expiry_iso(expiry) + '\nOnly the expiry of the account and this token is extended. The password, secret and VM permissions stay.'), flush=True)
    if pve_url:
        print('Proxmox: ' + proxmox_endpoint(pve_url), flush=True)
    if tty.ask(t('이 내용으로 갱신할까요? (y/N)', 'Renew with these settings? (y/N)'), 'N').lower() not in ('y', 'yes'):
        print(t('취소했습니다. 변경한 항목이 없습니다.', 'Cancelled. Nothing was changed.'))
        return
    result = renew_identity(token_id, expiry)
    try:
        added = ensure_agent_privilege(token_id)
        print(t('게스트 에이전트 조회 권한 확인: ' + added, 'Guest agent read privilege checked: ' + added) if added else
              t('게스트 에이전트 권한은 Proxmox 버전 또는 역할을 확인하지 못해 건드리지 않았습니다. (연결 준비 중 단계만 건너뜁니다.)',
                'The guest agent privilege was left alone: the Proxmox version or the role could not be read. (Only the "preparing the connection" step is skipped.)'), flush=True)
    except SetupError as error:
        print(t('유효기간은 갱신했지만 게스트 에이전트 권한은 추가하지 못했습니다: ', 'The expiry was renewed, but the guest agent privilege could not be added: ') + str(error), flush=True)
    encoded = base64.b64encode(json.dumps(result, separators=(',', ':')).encode('utf8')).decode('ascii')
    say_ok(t('갱신 완료', 'Renewed'))
    say_next(t('관리자 페이지의 갱신 창에 아래 결과를 붙여넣고 ‘갱신 확인’을 누르세요.', 'Paste the result below into the renewal window in the admin console and press "Confirm renewal".'))
    print_copy_block('MULTITAP-RENEWAL-V1:\n' + '\n'.join(textwrap.wrap(encoded, 76)) + '\nEND-MULTITAP-RENEWAL', t('관리자 페이지 갱신 창', 'the renewal window in the admin console'))


def main():
    global LANGUAGE
    LANGUAGE = pick_language(sys.argv[1:], os.environ)
    if not all(os.access(path, os.X_OK) for path in ['/usr/sbin/pveum', '/usr/bin/pvesh']):
        # Run where Multitap itself is installed (pasted into the server's own container, say): the message says so.
        if os.path.isdir('/opt/multitap'):
            where = t('여기는 Multitap 서버입니다.', 'This is the Multitap server.')
        else:
            where = t('여기는 Proxmox 노드가 아닙니다.', 'This is not a Proxmox node.')
        raise SetupError(where + t(' 이 명령은 Proxmox 노드에서 실행합니다.\n→ Proxmox 웹에서 노드(예: ' + 'pve' + ')를 고르고 ‘Shell’을 연 뒤 붙여 넣으세요.',
                                   ' Run this command on a Proxmox node.\n→ In the Proxmox web UI, select the node (e.g. pve), open "Shell" and paste it there.'))
    if os.geteuid() != 0:
        raise SetupError(t('Proxmox 노드의 root Shell에서 실행하세요.\n→ Proxmox 웹에서 노드를 고르고 ‘Shell’을 열면 root로 열립니다.',
                           'Run this in the root Shell of a Proxmox node.\n→ In the Proxmox web UI, select the node and open "Shell": it opens as root.'))
    parser = argparse.ArgumentParser(description=t('Multitap Proxmox 연동 설정', 'Multitap Proxmox integration setup'))
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--renew', metavar='USER@pve!TOKEN')
    action.add_argument('--enable-console', metavar='USER@pve!TOKEN')
    parser.add_argument('--endpoint', default='')
    # The admin console adds the language it shows in (pick_language read it already).
    parser.add_argument('--lang', choices=('ko', 'en'))
    # The setup command with `https://pve.example.com` added at the end: connect by this address
    # instead of the one found automatically (checked like a typed address).
    parser.add_argument('address', nargs='?', default='')
    args = parser.parse_args()
    if args.address and (args.renew or args.enable_console):
        parser.error(t('Proxmox 주소는 새 연결에서만 지정할 수 있습니다.', 'A Proxmox address can be given only for a new connection.'))
    with Terminal() as tty:
        if args.enable_console:
            enable_console(tty, args.enable_console)
        elif args.renew:
            renew_enrollment(tty, args.renew, args.endpoint)
        else:
            enroll(tty, args.address)


def choose_endpoint(tty, address, ca):
    """The Proxmox address and its trust: asked only when no automatic or given address works."""
    if address:
        label, candidate = t('명령에서 지정', 'given in the command'), address
    else:
        label, candidate = t('자동으로 찾음', 'found automatically'), default_endpoint()
    try:
        url = proxmox_endpoint(candidate)
        trust = check_address(url, ca)
        say_ok(t('Proxmox 주소: ', 'Proxmox address: ') + url + ' (' + label + ') — ' + t('주소·인증서 확인', 'address and certificate checked'))
        if trust.fingerprint:
            say_pin(trust)
        return url, trust
    except SetupError as error:
        reason = getattr(error, 'short', '') or str(error).splitlines()[0]
        say_warn((t('지정한 주소: ', 'Given address: ') if address else t('자동으로 시도한 주소: ', 'Address tried automatically: ')) + candidate, reason)
    say_next(t('평소 브라우저에서 여는 Proxmox 주소를 입력하세요. 예: https://pve.example.com 또는 https://ProxmoxIP:8006\n  Multitap 서버에서도 이 주소로 접속할 수 있어야 합니다.',
               'Enter the Proxmox address you open in your browser, e.g. https://pve.example.com or https://<Proxmox IP>:8006\n  The Multitap server must be able to reach this address too.'))
    while True:
        try:
            value = tty.ask(t('Proxmox 웹 주소 (위 Multitap 주소와 다름)', 'Proxmox web address (not the Multitap address above)'))
            if not value:
                raise SetupError(t('Proxmox 웹 주소를 입력하세요. 예: https://pve.example.com', 'Enter the Proxmox web address, e.g. https://pve.example.com'))
            url = proxmox_endpoint(value)
            trust = check_address(url, ca)
            say_ok(t('Proxmox 주소: ', 'Proxmox address: ') + url + ' — ' + t('주소·인증서 확인', 'address and certificate checked'))
            if trust.fingerprint:
                say_pin(trust)
            return url, trust
        except SetupError as error:
            lines = str(error).splitlines()
            say_fail(lines[0], '\n'.join(lines[1:]))


def say_pin(trust):
    say_warn(t('인증서에 이 주소가 없어 이 노드의 지금 인증서를 그대로 기억합니다.',
               "The certificate does not name this address, so this node's current certificate is remembered exactly."),
             t('지문(SHA-256): ', 'Fingerprint (SHA-256): ') + trust.fingerprint + t('\nProxmox 인증서가 바뀌면 이 명령을 다시 실행하세요.', '\nIf the Proxmox certificate changes, run this command again.'))


def found_vms(count):
    if LANGUAGE != 'en':
        return 'Proxmox에서 VM ' + str(count) + '개를 확인했습니다.'
    return 'Found 1 VM in Proxmox.' if count == 1 else 'Found ' + str(count) + ' VMs in Proxmox.'


def enroll(tty, address=''):
    if not BROKER_ORIGIN:
        raise SetupError(t('설치한 Multitap 관리자 페이지에서 쉘 명령을 복사해 실행하세요. 이 원본 파일에는 다른 설치의 서버 주소를 지정하지 않습니다.',
                           'Copy the Shell command from your Multitap admin console and run that. This source file names no server address.'))
    say_heading(t('Multitap Proxmox 연결', 'Connect Multitap to Proxmox'))
    print(t('Multitap 전용 연동 계정을 만듭니다.\n', 'This creates a dedicated Multitap integration account.\n') +
          painted('2', t('Multitap에 등록된 데스크톱(Host를 설치하고 승인한 VM)의 전원 켜기·끄기와 긴급 콘솔에 씁니다.\n데스크톱 목록에는 Host를 설치하고 승인한 VM만 나옵니다.\n이 명령은 VM 전원과 하드웨어 설정을 바꾸지 않습니다.',
                         'It is used to start and shut down the desktops registered in Multitap (VMs with an approved Host) and for the emergency Console.\nThe desktop list shows only VMs with an installed and approved Host.\nThis command does not change VM power or hardware settings.')) +
          t('\n\nWindows 앱에 입력할 Multitap 서버 주소: ', '\n\nMultitap server address to enter in the Windows app: ') + painted('1;36', BROKER_ORIGIN) + '\n' +
          painted('2', t('(Docker로 배포해도 앱에는 위 공개 주소를 사용합니다.)', '(Apps use the address above, with a Docker install too.)')), flush=True)
    ca_path = Path('/etc/pve/pve-root-ca.pem')
    ca = ca_path.read_text(encoding='ascii') if ca_path.is_file() else ''
    say_step(1, 3, t('Proxmox 주소', 'Proxmox address'))
    url, trust = choose_endpoint(tty, address, ca)
    vms = inventory()
    say_ok(found_vms(len(vms)))
    say_step(2, 3, t('연동 계정', 'Integration account'))
    suffix = secrets.token_hex(6)
    while True:
        name = tty.ask(t('새 연동 아이디 (multitap- 접두사 자동 추가)', 'New integration ID (multitap- is added in front)'), suffix)
        if re.fullmatch(r'[A-Za-z0-9_.-]{2,40}', name):
            user = 'multitap-' + name + '@pve'
            break
        say_fail(t('영문·숫자·점·밑줄·하이픈 2~40자를 사용하세요.', 'Use 2 to 40 letters, digits, dots, underscores or hyphens.'))
    token = 'broker'
    days = 365
    role = 'Multitap-' + suffix
    check_new_identity(user, role)
    privilege = agent_privilege()
    if not privilege:
        say_warn(t('Proxmox 버전을 확인하지 못해 게스트 에이전트 조회 권한은 추가하지 않습니다.', 'Could not read the Proxmox version, so the guest agent read privilege is not added.'),
                 t('(Windows 시작 후 "연결 준비 중" 단계만 건너뜁니다. 나중에 관리자 페이지의 갱신 명령으로 추가할 수 있습니다.)',
                   '(Only the "preparing the connection" step after Windows starts is skipped. You can add it later with the renewal command in the admin console.)'))
    print('\n' + painted('1', t('만들 항목', 'To be created')) + '\n' + indented(kv(t('연동 아이디', 'Integration ID'), user, '1;33') + '\n' + kv(t('토큰', 'Token'), user + '!' + token, '1;33') + '\n' + kv(t('역할', 'Role'), role) +
          '\n' + kv(t('권한 범위', 'Scope'), t('모든 VM(/vms)', 'all VMs (/vms)'), '1') + painted('2', t(' — 새로 등록하는 Host의 VM을 찾으려고 필요합니다. Multitap에는 등록한 데스크톱만 나옵니다.',
                                                                                                    ' — needed to find the VM of each newly registered Host. Multitap shows only registered desktops.')) +
          '\n' + kv(t('권한', 'Privileges'), ', '.join(privilege_lines(privilege)), '32') +
          '\n' + kv(t('계정·토큰 유효기간', 'Account and token valid for'), str(days) + t('일', ' days'))) + '\n' + painted('2', t('확인 후 비밀번호와 비밀번호 확인을 차례로 입력합니다.', 'After you confirm, you enter the password twice.')), flush=True)
    if tty.ask(t('이 내용으로 생성할까요? (y/N)', 'Create these? (y/N)'), 'N').lower() not in ('y', 'yes'):
        print(t('취소했습니다. 변경한 항목이 없습니다.', 'Cancelled. Nothing was changed.'))
        return
    expires_at = int(time.time()) + days * 86400
    secret = provision(user, role, token, expires_at,
                       verify=lambda token_id, value: verify_api(url, trust, token_id, value),
                       password_setter=lambda identity: set_password(identity, tty), privilege=privilege)
    say_ok(t('연동 계정·토큰을 만들었습니다.', 'Integration account and token created.'))
    say_ok(t('새 토큰으로 Proxmox API를 확인했습니다.', 'Proxmox API checked with the new token.'))
    say_step(3, 3, t('연결 정보', 'Connection details'))
    print_copy_block(connection_block(url, user, token, secret, trust.ca, expires_at, trust.fingerprint))
    summary = (kv(t('Proxmox 주소', 'Proxmox address'), url, '1;36') + (painted('2', t(' (인증서 지문으로 확인)', ' (checked by certificate fingerprint)')) if trust.fingerprint else '') +
               '\n' + kv(t('연동 사용자', 'Integration user'), user, '1;33') + painted('2', t(' (토큰 ', ' (token ') + user + '!' + token + ')') +
               '\n' + kv(t('토큰 만료일', 'Token expires'), expiry_iso(expires_at)) +
               '\n' + kv(t('권한', 'Privileges'), '\n      '.join(painted('32', line) for line in privilege_lines(privilege)), '0') +
               '\n' + kv(t('관리자 페이지', 'Admin console'), BROKER_ORIGIN + '/admin', '1;36'))
    heading = t('요약', 'Summary')
    print('\n' + (painted('1;30;47', ' ' + heading + ' ') if colored() else heading) + '\n' + indented(summary) +
          '\n' + painted('1', t('데스크톱 목록에는 Host를 설치하고 승인한 VM만 나옵니다.', 'The desktop list shows only VMs with an installed and approved Host.')), flush=True)
    say_next(t('관리자 페이지에서 연결 설정을 붙여넣고 ‘연결하기’를 누르세요.', 'In the admin console, paste the connection details and press "Connect".'))
    say_next(t('그다음 각 VM에 Host를 설치하고 관리자 페이지에서 승인하면 데스크톱 목록에 나옵니다.', 'Then install the Host on each VM and approve it in the admin console. It then appears in the desktop list.'))


if __name__ == '__main__':
    try:
        main()
    except SetupError as error:
        lines = str(error).splitlines() or ['']
        print('\n' + marked('1;31', '✖', painted('1;31', t('설정 중단: ', 'Setup stopped: ') + lines[0], sys.stderr), sys.stderr) + ('\n' + indented('\n'.join(lines[1:])) if len(lines) > 1 else ''), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('\n' + t('취소했습니다.', 'Cancelled.'), file=sys.stderr)
        sys.exit(130)
