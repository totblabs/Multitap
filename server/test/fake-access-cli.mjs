// The access-cli of fake-docker.sh: the real main() of src/access-cli.mjs on a
// folder that stands for the data volume, with the environment compose.yaml
// gives the service (read from the install's .env). The file reader is a plain
// one: the owner and mode the server demands of its files are not what the
// command tests show, and Windows has neither. A policy file that begins with
// the line `#another-owner` stands for one in a folder of another account: the
// server's account may not read it (reading fails as the server's own reader
// fails on such a file) and may not remove it either, so `reset` answers
// server_access_reset_denied and only root can clear it.
//   node fake-access-cli.mjs <data folder> <.env file> <access-cli arguments...>
import { readFileSync } from 'node:fs';
import { main } from '../src/access-cli.mjs';
import { configuration, selfAccessStorage } from '../src/config.mjs';

const [data, envFile, ...argv] = process.argv.slice(2);
const dot = Object.fromEntries(readFileSync(envFile, 'utf8').split(/\r?\n/).filter(line => /^[A-Z_]+=/.test(line)).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
// compose.yaml's `environment:` for the keys the access settings read.
const env = { NODE_ENV: 'production', MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DATA_DIR: data, MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_TLS_MODE: 'proxy',
  MULTITAP_PUBLIC_ORIGIN: dot.MULTITAP_PUBLIC_ORIGIN ?? '', MULTITAP_HTTP_ORIGIN: dot.MULTITAP_HTTP_ORIGIN ?? '', MULTITAP_ALLOW_INSECURE_HTTP: dot.MULTITAP_ALLOW_INSECURE_HTTP ?? '0',
  MULTITAP_SERVER_ADDRESS: dot.MULTITAP_SERVER_ADDRESS ?? '', MULTITAP_PUBLISHED_PORT: dot.MULTITAP_PUBLISH_PORT ?? '18480', MULTITAP_TRUSTED_PROXIES: dot.MULTITAP_TRUSTED_PROXIES ?? '' };
const readAccessFile = path => {
  let text;
  try { text = readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; }
  if (text.startsWith('#another-owner\n')) throw Error('server_access_storage_invalid');
  return text;
};
// What cannot be removed there: a path that is no file to unlink (the folder itself).
const storage = value => {
  const files = selfAccessStorage(value, { readAccessFile });
  if (!files) return files;
  let foreign = false;
  try { readAccessFile(files.paths.policy); } catch { foreign = true; }
  return foreign ? { ...files, paths: { ...files.paths, policy: files.paths.directory } } : files;
};
process.exitCode = await main(argv, { env, configure: value => configuration(value, { readAccessFile }), storage });
