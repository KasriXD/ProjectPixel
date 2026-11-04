// Plays a platform game in headless Chromium through the platform's /play
// page: makes an account (or logs in), picks the game, and lets the
// WebAssembly player join a game server over WebSockets.
//
// Usage: node platform_test.js <platform-url> <screenshot.png> [options]
//   --user NAME --password TEXT   the account (made if it does not exist)
//   --game ID                     the game to click (default: the first one)
//   --frames N                    player runs N ticks online, then reports "Finished"
//   --input-script PATH           a route in the player's files, e.g. /examples/topdown/tests/full_run.input
//   --expect-level-complete       the player saw the level completed
//   --expect-players N            the player saw at least N players at once
//   --ignore-https-errors         trust the platform's development certificate
//   --timeout SECONDS             give up after this long (default 150)
// Exit code 0 when the player joined and every expectation held.
const { chromium } = require('playwright');

const RELEVANT = /stub:|Loaded|Connecting|Joined|Level complete|Finished|Disconnected|ERROR|WARN|error|CSP|PAGE ERROR|Aborted/;

function parse(argv) {
  const o = { url: argv[0], screenshot: argv[1], user: 'webplayer', password: 'browser-pass-123', game: null,
    frames: null, script: null, expectComplete: false, expectPlayers: 0, ignoreHttps: false, timeout: 150 };
  for (let i = 2; i < argv.length; ++i) {
    const a = argv[i];
    const v = () => argv[++i];
    if (a === '--user') o.user = v();
    else if (a === '--password') o.password = v();
    else if (a === '--game') o.game = v();
    else if (a === '--frames') o.frames = v();
    else if (a === '--input-script') o.script = v();
    else if (a === '--expect-level-complete') o.expectComplete = true;
    else if (a === '--expect-players') o.expectPlayers = Number(v());
    else if (a === '--ignore-https-errors') o.ignoreHttps = true;
    else if (a === '--timeout') o.timeout = Number(v());
    else throw new Error('unknown option ' + a);
  }
  if (!o.url || !o.screenshot) throw new Error('usage: node platform_test.js <platform-url> <screenshot.png> [options]');
  return o;
}

async function main() {
  const o = parse(process.argv.slice(2));
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const context = await browser.newContext({ viewport: { width: 1100, height: 800 }, ignoreHTTPSErrors: o.ignoreHttps });
  const page = await context.newPage();
  const lines = [];
  const started = Date.now();
  const elapsed = () => ((Date.now() - started) / 1000).toFixed(1);
  // Relevant lines are printed as they come, so a script can wait for "Joined".
  const note = (text) => {
    lines.push(text);
    if (RELEVANT.test(text)) console.log(`[${elapsed()} s] ${text.replace(/--token","[^"]+"/, '--token","<token>"')}`);
  };
  page.on('console', (m) => note(m.text()));
  page.on('pageerror', (e) => note('PAGE ERROR: ' + e.message));
  let failure = '';

  try {
    const q = new URLSearchParams();
    if (o.frames) q.set('frames', o.frames);
    if (o.script) q.set('inputScript', o.script);
    await page.goto(o.url.replace(/\/$/, '') + '/play' + (q.toString() ? '?' + q.toString() : ''));
    await page.waitForSelector('#login:not([hidden]), #catalog:not([hidden])', { timeout: 20000 });
    if (await page.isVisible('#login')) {
      await page.fill('#username', o.user);
      await page.fill('#password', o.password);
      await page.click('#register');
      await page.waitForSelector('#catalog:not([hidden]), #message:not([hidden])', { timeout: 20000 });
      if (await page.isVisible('#message')) {
        const text = await page.textContent('#message');
        if (!/taken/i.test(text)) throw new Error('could not make the account: ' + text);
        await page.fill('#password', o.password);
        await page.click('#login-form button[type=submit]');
        await page.waitForSelector('#catalog:not([hidden])', { timeout: 20000 });
      }
    }
    console.log(`[${elapsed()} s] Logged in as ${o.user}`);
    const button = o.game ? `#games button[data-game="${o.game}"]` : '#games button';
    await page.waitForSelector(button, { timeout: 20000 });
    const titles = await page.$$eval('#games .title', (els) => els.map((e) => e.textContent));
    console.log(`[${elapsed()} s] Catalog: ${titles.join(', ')}`);
    await page.click(button);

    let shot = false;
    while (Date.now() - started < o.timeout * 1000) {
      if (!shot && lines.some((l) => l.includes('Level complete'))) {
        await page.screenshot({ path: o.screenshot }).catch(() => {});
        shot = true;
      }
      const done = o.frames ? lines.some((l) => l.includes('Finished')) : lines.some((l) => l.includes('Joined'));
      if (done || lines.some((l) => l.includes('PAGE ERROR') || l.includes('Aborted'))) break;
      if (await page.isVisible('#message')) break;
      await page.waitForTimeout(100);
    }
    if (!shot) await page.screenshot({ path: o.screenshot }).catch(() => {});
    const message = (await page.isVisible('#message')) ? await page.textContent('#message') : '';
    const status = await page.textContent('#game-status');
    console.log(`[${elapsed()} s] Page status: "${status}"${message ? `, message: "${message}"` : ''}`);
    if (message) failure = 'the page showed an error: ' + message;
  } catch (e) {
    failure = e.message.split('\n')[0];
    await page.screenshot({ path: o.screenshot }).catch(() => {});
  }
  await browser.close();

  const joined = lines.some((l) => l.includes('Joined'));
  const finished = lines.find((l) => l.includes('Finished')) || '';
  if (!failure && !joined) failure = 'the player did not join a game server';
  if (!failure && o.frames && !finished) failure = 'the player did not report "Finished"';
  if (!failure && o.expectComplete && !finished.includes('level complete: true')) failure = 'the level was not completed';
  if (!failure && o.expectPlayers > 0) {
    const m = /most at once (\d+)/.exec(finished);
    if (!m || Number(m[1]) < o.expectPlayers) failure = `expected ${o.expectPlayers} players at once`;
  }
  if (!failure && lines.some((l) => l.startsWith('CSP blocked'))) failure = 'the page broke its content security policy';
  console.log(failure ? 'FAILED: ' + failure : `PASSED in ${elapsed()} s`);
  process.exit(failure ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
