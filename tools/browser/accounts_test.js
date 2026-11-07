// Accounts on the platform's /play page in headless Chromium
// (docs/accounts.md): makes an account, adds an email address and confirms
// it with the link from the mail, changes the password, downloads the data,
// resets a forgotten password with a link, and deletes the account. The page
// runs without a browser player, so it also checks that the games are there
// without Play buttons. tools/browser/accounts_test.sh prepares the platform
// and the mail (files, or a real SMTP server with STARTTLS).
//
// Usage: node accounts_test.js <platform-url> <out-dir> --mail DIR --game ID [--phone]
//   --mail: where the platform's messages end up (a --mail-dir folder or a Maildir)
// Screenshots: accounts-*.png in out-dir.
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

function parse(argv) {
  const o = { url: argv[0], out: argv[1], mail: null, game: null, phone: false };
  for (let i = 2; i < argv.length; ++i) {
    const a = argv[i];
    const v = () => argv[++i];
    if (a === '--mail') o.mail = v();
    else if (a === '--game') o.game = v();
    else if (a === '--phone') o.phone = true;
    else throw new Error('unknown option ' + a);
  }
  if (!o.url || !o.out || !o.mail || !o.game) throw new Error('usage: node accounts_test.js <url> <out-dir> --mail DIR --game ID');
  o.url = o.url.replace(/\/$/, '');
  return o;
}

// Every message under dir (a folder of .eml files, or a Maildir), oldest first.
function messages(dir) {
  const files = [];
  const walk = (d) => {
    for (const e of fs.existsSync(d) ? fs.readdirSync(d, { withFileTypes: true }) : []) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.isFile()) files.push({ p, t: fs.statSync(p).mtimeMs, n: e.name });
    }
  };
  walk(dir);
  files.sort((a, b) => a.t - b.t || a.n.localeCompare(b.n));
  return files.map(({ p }) => {
    const raw = fs.readFileSync(p, 'utf8');
    const split = raw.search(/\r?\n\r?\n/);
    const headers = raw.slice(0, split);
    const body = raw.slice(split).replace(/\s+/g, '');
    return { headers, text: Buffer.from(body, 'base64').toString('utf8') };
  });
}

async function main() {
  const o = parse(process.argv.slice(2));
  const browser = await chromium.launch();
  const context = await browser.newContext({
    viewport: o.phone ? { width: 390, height: 844 } : { width: 1100, height: 820 },
    isMobile: o.phone, hasTouch: o.phone, acceptDownloads: true,
  });
  const page = await context.newPage();
  const lines = [];
  const started = Date.now();
  const log = (text) => console.log(`[${((Date.now() - started) / 1000).toFixed(1)} s] ${text}`);
  page.on('console', (m) => { lines.push(m.text()); if (/CSP|error/i.test(m.text())) log(m.text()); });
  page.on('pageerror', (e) => { lines.push('PAGE ERROR: ' + e.message); log('PAGE ERROR: ' + e.message); });
  const prefix = o.phone ? 'phone-' : '';
  const shot = (name) => page.screenshot({ path: path.join(o.out, prefix + name), fullPage: true }).catch(() => {});
  const text = (sel) => page.textContent(sel);
  const visible = (sel) => page.waitForSelector(sel, { state: 'visible', timeout: 20000 });
  const fits = async (where) => {
    const wide = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    if (wide > 1) throw new Error(`${where}: the page is ${wide} px wider than the screen`);
  };
  const noticeSays = async (what, re) => {
    const until = Date.now() + 20000;
    while (Date.now() < until) {
      if (await page.isVisible('#message')) throw new Error(`${what}: the page says "${await text('#message')}"`);
      if (await page.isVisible('#notice') && re.test(await text('#notice'))) return text('#notice');
      await page.waitForTimeout(100);
    }
    throw new Error(`${what}: no notice (${await text('#notice')})`);
  };
  const messageSays = async (what, re) => {
    await visible('#message');
    const m = await text('#message');
    if (!re.test(m)) throw new Error(`${what}: the page says "${m}"`);
  };
  // The newest link of a kind, once its message arrived.
  const nextLink = async (kind, count) => {
    const until = Date.now() + 30000;
    while (Date.now() < until) {
      const all = messages(o.mail);
      if (all.length >= count) {
        const m = all[count - 1].text.match(new RegExp(`(https?://[^\\s]+/play)#${kind}=([A-Za-z0-9_-]+)`));
        if (!m) throw new Error(`message ${count} has no ${kind} link:\n${all[count - 1].text}`);
        return { url: `${m[1]}#${kind}=${m[2]}`, token: m[2], mail: all[count - 1] };
      }
      await page.waitForTimeout(200);
    }
    throw new Error(`no message ${count} (${kind}) arrived`);
  };
  const login = async (username, password) => (await fetch(o.url + '/api/v1/sessions', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username, password }),
  })).status;
  let failure = '';

  try {
    // Making an account; the page works without a browser player.
    await page.goto(o.url + '/play');
    await visible('#login');
    await visible('#forgot');
    if (!(await page.evaluate(() => document.body.classList.contains('no-player')))) throw new Error('the page thinks it has a player');
    await page.fill('#username', 'ana');
    await page.fill('#password', 'first-password');
    await page.click('#register');
    await visible('#catalog');
    await visible(`#games a.open[href="#game=${o.game}"]`);
    if (await page.isVisible('#games button:has-text("Play")')) throw new Error('Play buttons without a player');
    await page.click(`#games a.open[href="#game=${o.game}"]`);
    await visible('#details .only-app');
    if (await page.isVisible('#details-play')) throw new Error('a Play button without a player');
    log('Account made; games listed without Play buttons; the game page says to use the app');

    // An email address, confirmed by the link in the mail.
    await page.click('#nav a[data-page=you]');
    await visible('#email-form');
    // (The account settings arrive after the page opens.)
    const stateIs = (re, what) => page.waitForFunction((src) => new RegExp(src).test(document.getElementById('email-state').textContent),
      re.source, { timeout: 10000 }).catch(async () => { throw new Error(`${what}: ${await text('#email-state')}`); });
    await stateIs(/No email address/, 'email state before');
    await page.fill('#email', 'Ana@Example.com');
    await page.fill('#email-password', 'wrong-password');
    await page.click('#email-form button[type=submit]');
    await messageSays('a wrong password', /not your password/);
    await page.fill('#email-password', 'first-password');
    await page.click('#email-form button[type=submit]');
    await noticeSays('saving the address', /link is on its way to ana@example\.com/);
    await visible('#resend-email');
    await stateIs(/not confirmed/, 'email state after saving');
    await fits('account');
    await shot('accounts-you.png');
    const verify = await nextLink('verify', 1);
    if (!/To: <ana@example\.com>/.test(verify.mail.headers)) throw new Error('mail headers:\n' + verify.mail.headers);
    if (!/Hello ana/.test(verify.mail.text)) throw new Error('mail text:\n' + verify.mail.text);
    await page.goto(verify.url);
    await noticeSays('confirming', /ana@example\.com is confirmed for ana/);
    if (page.url().includes('verify=')) throw new Error('the token stayed in the address: ' + page.url());
    await visible('#you');
    await stateIs(/· confirmed$/, 'email state after confirming');
    if (await page.isVisible('#resend-email')) throw new Error('"send again" for a confirmed address');
    log(`Email address confirmed from the mail (${verify.url.replace(verify.token, '<token>')})`);

    // A used link says so.
    await page.goto(verify.url);
    await messageSays('a used link', /used already/);

    // Changing the password: other sessions end, this page stays logged in.
    await page.click('#nav a[data-page=you]');
    await visible('#password-form');
    await page.fill('#current-password', 'first-password');
    await page.fill('#new-password', 'second-password');
    await page.click('#password-form button[type=submit]');
    await noticeSays('changing the password', /password is changed/);
    if (await login('ana', 'first-password') !== 401) throw new Error('the old password still works');
    if (await login('ana', 'second-password') !== 200) throw new Error('the new password does not work');
    await page.reload();
    await visible('#who');
    log('Password changed');

    // Downloading the data.
    await page.click('#nav a[data-page=you]');
    await visible('#export');
    const [download] = await Promise.all([page.waitForEvent('download'), page.click('#export')]);
    const file = path.join(o.out, prefix + 'ana-account.json');
    await download.saveAs(file);
    const data = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (data.account.username !== 'ana' || data.account.email !== 'ana@example.com' || !data.account.emailVerified) {
      throw new Error('the download: ' + JSON.stringify(data.account));
    }
    log(`Downloaded ${download.suggestedFilename()} (${fs.statSync(file).size} bytes)`);

    // A forgotten password: a link by email, a new password, logged out everywhere.
    await page.click('#logout');
    await visible('#login');
    await page.fill('#username', 'ana');
    await page.click('#forgot');
    await visible('#forgot-page');
    if (await page.inputValue('#forgot-name') !== 'ana') throw new Error('the name was not carried over');
    await page.fill('#forgot-name', 'ANA@example.com');
    await page.click('#forgot-form button[type=submit]');
    await noticeSays('asking for a link', /a link is on its way/);
    await fits('forgot');
    await shot('accounts-forgot.png');
    const reset = await nextLink('reset', 2);
    if (!/reset the password of the account ana/.test(reset.mail.text)) throw new Error('mail text:\n' + reset.mail.text);
    await page.goto(reset.url);
    await visible('#reset-page');
    if (page.url().includes('reset=')) throw new Error('the token stayed in the address: ' + page.url());
    await page.fill('#reset-password', 'third-password');
    await page.fill('#reset-repeat', 'third-passwort');
    await page.click('#reset-form button[type=submit]');
    await messageSays('different passwords', /not the same/);
    await page.fill('#reset-repeat', 'third-password');
    await fits('reset');
    await shot('accounts-reset.png');
    await page.click('#reset-form button[type=submit]');
    await noticeSays('the new password', /Log in with the new one/);
    await visible('#login');
    if (await page.inputValue('#username') !== 'ana') throw new Error('the name is not filled in');
    if (await login('ana', 'second-password') !== 401) throw new Error('the old password still works');
    await page.fill('#password', 'third-password');
    await page.click('#login-form button[type=submit]');
    await visible('#catalog');
    log('Password reset from the mail; logged in with the new one');

    // Deleting the account.
    await page.click('#nav a[data-page=you]');
    await visible('#delete-account');
    await page.click('#delete-account');
    await visible('#delete-dialog');
    await page.fill('#delete-password', 'wrong-password');
    await page.click('#delete-form button[type=submit]');
    await messageSays('a wrong password', /not your password/);
    await page.click('#delete-account');
    await page.fill('#delete-password', 'third-password');
    await shot('accounts-delete.png');
    await page.click('#delete-form button[type=submit]');
    await noticeSays('deleting', /account is deleted/);
    await visible('#login');
    if (await page.isVisible('#nav')) throw new Error('still logged in');
    if (await login('ana', 'third-password') !== 401) throw new Error('the deleted account can log in');
    log('Account deleted');
  } catch (e) {
    failure = e.message.split('\n')[0];
    console.log(e.message);
    await shot('accounts-failure.png');
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
