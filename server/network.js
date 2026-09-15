import { spawn, execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const remote = 'https://github.com/joshuwamccoy23-glitch/gw-drops.git';
const cloudflared = process.env.CLOUDFLARED || 'C:/Program Files (x86)/cloudflared/cloudflared.exe';
let stopping = false;
let server;
let tunnel;
let desiredUrl = '';
let publishedUrl = '';
let publishFailures = 0;
let checkout;
const options = { windowsHide: true, timeout: 60000, env: { ...process.env, GIT_TERMINAL_PROMPT: '0' }, stdio: ['ignore', 'pipe', 'pipe'] };

function git(args) {
  return execFileSync('git', args, { ...options, cwd: checkout }).toString().trim();
}

async function publish() {
  if (!desiredUrl || desiredUrl === publishedUrl || stopping) return;
  const url = desiredUrl;
  try {
    const response = await fetch(`${url}/v1/listings?limit=1`, { signal: AbortSignal.timeout(10000) });
    if (!response.ok || (await response.json()).schema_version !== 2) throw new Error('Public listings route is not ready');
    if (!checkout) {
      const folder = mkdtempSync(path.join(tmpdir(), 'gw-drops-discovery-'));
      execFileSync('git', ['clone', '--depth=1', '--branch', 'main', remote, folder], options);
      checkout = folder;
    }
    git(['pull', '--rebase', 'origin', 'main']);
    writeFileSync(path.join(checkout, 'data/auction-droplistings.txt'), `${url}\n`);
    git(['add', 'data/auction-droplistings.txt']);
    if (git(['diff', '--cached', '--name-only'])) {
      git(['-c', 'user.name=GW-Drops Server', '-c', 'user.email=gw-drops@users.noreply.github.com', 'commit', '-m', 'Update auction server discovery address']);
    }
    git(['push', 'origin', 'HEAD:main']);
    publishedUrl = url;
    publishFailures = 0;
    console.log(`Published server address: ${url}`);
  } catch (error) {
    publishFailures++;
    const details = [];
    for (let current = error; current; current = current.cause) {
      const detail = current.code ? `${current.message} (${current.code})` : current.message;
      if (detail && !details.includes(detail)) details.push(detail);
    }
    console.error(`Address publication failed (${publishFailures}/4): ${details.join(': ')}`);
    if (publishFailures >= 4 && desiredUrl === url && tunnel?.exitCode === null) {
      console.error('The public tunnel did not become reachable; replacing it in 5 seconds.');
      publishFailures = 0;
      desiredUrl = '';
      tunnel.kill();
    }
  }
}

function startTunnel() {
  desiredUrl = '';
  tunnel = spawn(existsSync(cloudflared) ? cloudflared : 'cloudflared', ['tunnel', '--url', 'http://127.0.0.1:8787', '--no-autoupdate'], { cwd: root, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let buffer = '';
  const read = chunk => {
    buffer = (buffer + chunk.toString()).slice(-16000);
    const match = buffer.match(/https:\/\/[a-z0-9-]+\.trycloudflare\.com/);
    if (match && desiredUrl !== match[0]) {
      desiredUrl = match[0];
      publishFailures = 0;
      console.log(`Tunnel ready: ${desiredUrl}`);
    }
  };
  tunnel.stdout.on('data', read);
  tunnel.stderr.on('data', read);
  tunnel.on('error', error => console.error(error.message));
  tunnel.on('close', () => { if (!stopping) setTimeout(startTunnel, 5000); });
}

async function startServer() {
  try {
    const response = await fetch('http://127.0.0.1:8787/v1/listings?limit=1', { signal: AbortSignal.timeout(2000) });
    if (response.ok && (await response.json()).schema_version === 2) return;
  } catch {}
  if (server && server.exitCode === null) return;
  server = spawn(process.execPath, ['server/server.js'], {
    cwd: root,
    windowsHide: true,
    env: {
      ...process.env,
      AUTO_GIT_PUSH: 'false',
      DROP_LOG_PATH: process.env.DROP_LOG_PATH || 'C:/Jarvis/Jarvis/Projects/GWToolboxpp/drop_log.md'
    },
    stdio: ['ignore', 'inherit', 'inherit']
  });
  server.on('error', error => console.error(error.message));
}

await startServer();
startTunnel();
let publishing = false;
setInterval(async () => {
  if (stopping || publishing) return;
  publishing = true;
  try { await startServer(); await publish(); } finally { publishing = false; }
}, 15000);
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => {
  stopping = true;
  tunnel?.kill();
  server?.kill();
  process.exit(0);
});
