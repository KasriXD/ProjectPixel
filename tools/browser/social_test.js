// The social side of the platform's /play page in headless Chromium
// (docs/launcher.md#the-browser): logs in, checks the catalog's pictures, a
// game's page and its leaderboards, accepts a friend request, sends one,
// edits the profile, joins a friend's game and invites another friend from
// it. tools/browser/social_test.sh prepares the platform.
//
// Usage: node social_test.js <platform-url> <out-dir> --game ID --user NAME --password TEXT
//          --friend NAME (playing, with a score) --requester NAME --requester-password TEXT
//          [--phone] [--ignore-https-errors] [--timeout SECONDS]
//   --phone: a phone-sized touch screen (390x844)
// Screenshots: catalog.png, details.png, friends.png, you.png, playing.png in out-dir.
const { chromium } = require('playwright');
const path = require('path');

function parse(argv) {
  const o = { url: argv[0], out: argv[1], game: null, user: null, password: null, friend: null, requester: null,
    requesterPassword: null, phone: false, ignoreHttps: false, timeout: 120 };
  for (let i = 2; i < argv.length; ++i) {
    const a = argv[i];
    const v = () => argv[++i];
    if (a === '--game') o.game = v();
    else if (a === '--user') o.user = v();
    else if (a === '--password') o.password = v();
    else if (a === '--friend') o.friend = v();
    else if (a === '--requester') o.requester = v();
    else if (a === '--requester-password') o.requesterPassword = v();
    else if (a === '--phone') o.phone = true;
    else if (a === '--ignore-https-errors') o.ignoreHttps = true;
    else if (a === '--timeout') o.timeout = Number(v());
    else throw new Error('unknown option ' + a);
  }
  if (!o.url || !o.out || !o.game || !o.user || !o.friend || !o.requester) {
    throw new Error('usage: node social_test.js <platform-url> <out-dir> --game ID --user NAME --password TEXT ' +
      '--friend NAME --requester NAME --requester-password TEXT');
  }
  o.url = o.url.replace(/\/$/, '');
  return o;
}

async function main() {
  const o = parse(process.argv.slice(2));
  if (o.ignoreHttps) process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const context = await browser.newContext({
    viewport: o.phone ? { width: 390, height: 844 } : { width: 1100, height: 820 },
    isMobile: o.phone, hasTouch: o.phone, ignoreHTTPSErrors: o.ignoreHttps,
  });
  const page = await context.newPage();
  const lines = [];
  const started = Date.now();
  const log = (text) => console.log(`[${((Date.now() - started) / 1000).toFixed(1)} s] ${text}`);
  page.on('console', (m) => {
    lines.push(m.text());
    if (/Joined|Joining|Invited|CSP|ERROR|error|stub:/.test(m.text())) log(m.text().replace(/--token","[^"]+"/, '--token","<token>"'));
  });
  page.on('pageerror', (e) => { lines.push('PAGE ERROR: ' + e.message); log('PAGE ERROR: ' + e.message); });
  const prefix = o.phone ? 'phone-' : '';
  const shot = (name) => page.screenshot({ path: path.join(o.out, prefix + name), fullPage: true }).catch(() => {});
  const text = (sel) => page.textContent(sel);
  const waitFor = async (what, check, seconds = 20) => {
    const until = Date.now() + seconds * 1000;
    while (Date.now() < until) {
      if (await check()) return;
      if (await page.isVisible('#message')) throw new Error(`${what}: the page says "${await text('#message')}"`);
      await page.waitForTimeout(100);
    }
    throw new Error(what + ': timed out');
  };
  const api = async (method, p, token, body) => {
    const res = await fetch(o.url + p, {
      method,
      headers: { Authorization: 'Bearer ' + token, 'Content-Type': 'application/json' },
      body: body ? JSON.stringify(body) : undefined,
    });
    return res.json();
  };
  // Nothing may stick out sideways (on phones especially).
  const fits = async (where) => {
    const wide = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    if (wide > 1) throw new Error(`${where}: the page is ${wide} px wider than the screen`);
  };
  let failure = '';

  try {
    await page.goto(o.url + '/play');
    await page.waitForSelector('#login:not([hidden])', { timeout: 20000 });
    await page.fill('#username', o.user);
    await page.fill('#password', o.password);
    await page.click('#login-form button[type=submit]');
    await page.waitForSelector('#catalog:not([hidden])', { timeout: 20000 });
    log(`Logged in as ${o.user}`);

    // The catalog: the game's picture arrives (320x180), the friend request shows as a badge.
    const img = `#games a.open[href="#game=${o.game}"] img`;
    await page.waitForSelector(img, { timeout: 20000 });
    await waitFor('the picture', () => page.$eval(img, (e) => e.complete && e.naturalWidth > 0));
    const size = await page.$eval(img, (e) => `${e.naturalWidth}x${e.naturalHeight}`);
    if (size !== '320x180') throw new Error('the picture is ' + size);
    await waitFor('the friends badge', async () => (await text('#friends-badge')) === '1' && page.isVisible('#friends-badge'));
    log(`Catalog: picture ${size}, friends badge 1`);
    await fits('catalog');
    await shot('catalog.png');

    // A link to play= or join= opens the game's page or the friends page:
    // only a click on the page starts the player.
    await page.goto(`${o.url}/play#join=${o.friend}`);
    await page.waitForSelector('#friends:not([hidden])', { timeout: 20000 });
    await page.goto(`${o.url}/play#play=${o.game}`);
    await page.waitForSelector('#details:not([hidden])', { timeout: 20000 });
    if (lines.some((l) => /Joining|stub:|Connecting/.test(l))) throw new Error('a link started the player');
    log('Links to play= and join= opened pages without starting the player');
    await page.click('#nav a[data-page=games]');
    await page.waitForSelector('#catalog:not([hidden])');

    // The game's page: the friend's score, for everyone and among friends.
    await page.click(`#games a.open[href="#game=${o.game}"]`);
    await page.waitForSelector('#details:not([hidden])');
    await waitFor('the leaderboard', async () => (await page.$$('#boards tbody tr td.score')).length > 0);
    const row = await page.$eval('#boards tbody tr', (tr) => tr.textContent);
    if (!row.includes(o.friend) || !/\d+:\d\d\.\d\d/.test(row)) throw new Error('leaderboard row: ' + row);
    const playing = await text('#details-friends');
    if (!playing.includes(`${o.friend} is playing`)) throw new Error('friends in the game: ' + playing);
    await page.check('input[name=board-scope][value=friends]');
    const friendsRow = await page.$eval('#boards tbody tr', (tr) => tr.textContent);
    if (!friendsRow.includes(o.friend)) throw new Error('friends leaderboard: ' + friendsRow);
    log(`Game page: "${row.trim()}", ${playing.trim()}`);
    await fits('details');
    await shot('details.png');

    // Friends: accept the request, send one (the suggestions find the name).
    await page.click('#nav a[data-page=friends]');
    await page.waitForSelector('#friends:not([hidden])');
    await waitFor('the request', async () => (await text('#incoming')).includes(o.requester));
    await page.click(`#incoming li:has-text("${o.requester}") button:has-text("Accept")`);
    await waitFor('accepting', async () => (await text('#notice')).includes(`${o.requester} are friends`));
    await waitFor('the new friend', async () => (await text('#friend-list')).includes(o.requester));
    await page.fill('#add-name', 'ma');
    await waitFor('suggestions', async () => (await page.$$('#suggestions option[value=maker]')).length > 0);
    await page.fill('#add-name', 'maker');
    await page.click('#add-friend button[type=submit]');
    await waitFor('the request to maker', async () => (await text('#outgoing')).includes('maker'));
    const list = await text('#friend-list');
    if (!/Playing/.test(list)) throw new Error('presence: ' + list);
    log('Friends: accepted a request, sent one, presence shown');
    await fits('friends');
    await shot('friends.png');

    // You: a portrait, a bio and who may join; the header follows.
    await page.click('#nav a[data-page=you]');
    await page.waitForSelector('#avatar-picker .pick');
    await page.click('#avatar-picker .pick:nth-child(4)');
    await page.fill('#bio', 'Gem hunter.');
    await page.check('input[name=join-policy][value=nobody]');
    await page.click('#save-profile');
    await waitFor('saving', async () => (await text('#notice')).includes('Saved'));
    const headerAvatar = await page.getAttribute('#who-avatar', 'class');
    if (!headerAvatar.includes('av3')) throw new Error('header portrait: ' + headerAvatar);
    log('Profile saved');
    await fits('you');
    await shot('you.png');

    // Join the friend from the friends page, then invite the other friend.
    await page.click('#nav a[data-page=friends]');
    await page.waitForSelector(`#friend-list li:has-text("${o.friend}") button:has-text("Join")`);
    await page.click(`#friend-list li:has-text("${o.friend}") button:has-text("Join")`);
    await waitFor('joining the friend', async () => lines.some((l) => l.includes('Joined')), o.timeout);
    await page.waitForSelector(`#invite-list li:has-text("${o.requester}") button:has-text("Invite")`, { timeout: 20000 });
    await page.click(`#invite-list li:has-text("${o.requester}") button:has-text("Invite")`);
    await waitFor('inviting', async () => lines.some((l) => l.includes(`Invited ${o.requester}`)));
    // The invitation reaches the other friend while we are in the game.
    const login = await (await fetch(o.url + '/api/v1/sessions', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: o.requester, password: o.requesterPassword }),
    })).json();
    const { social } = await api('GET', '/api/v1/me/social', login.token);
    if (!social.invitations.some((i) => i.from === o.user)) throw new Error('the invitation did not arrive');
    log(`Joined ${o.friend} and invited ${o.requester}`);
    await fits('playing');
    await shot('playing.png');
  } catch (e) {
    failure = e.message.split('\n')[0];
    await shot('failure.png');
  }
  await browser.close();
  if (!failure && lines.some((l) => l.startsWith('CSP blocked'))) failure = 'the page broke its content security policy';
  if (!failure && lines.some((l) => l.startsWith('PAGE ERROR'))) failure = 'the page had a script error';
  console.log(failure ? 'FAILED: ' + failure : `PASSED in ${((Date.now() - started) / 1000).toFixed(1)} s`);
  process.exit(failure ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
