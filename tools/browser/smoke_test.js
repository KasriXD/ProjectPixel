// Runs the WebAssembly build of px-player in headless Chromium.
//
// Usage: node smoke_test.js <url> <screenshot.png>
// Plays the scripted route given in the URL, takes a screenshot of the page
// when the level is completed (while the game is still running), waits for
// the player to report "Finished", and prints the game's log lines.
// Exit code 0 only if the level was completed without script errors.
const { chromium } = require('playwright');

const RELEVANT = /Window|Loaded|COINS|Level complete|Finished|ERROR|error|PAGE ERROR|Aborted/;

async function main() {
  const [url, screenshot] = process.argv.slice(2);
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = await browser.newPage({ viewport: { width: 1000, height: 640 } });
  const lines = [];
  page.on('console', (m) => lines.push(m.text()));
  page.on('pageerror', (e) => lines.push('PAGE ERROR: ' + e.message));

  const started = Date.now();
  await page.goto(url);
  let shot = false;
  while (Date.now() - started < 120000) {
    if (!shot && lines.some((l) => l.includes('Level complete'))) {
      // Capture while the win banner is shown; the game exits a few frames later.
      try {
        await page.screenshot({ path: screenshot });
        shot = true;
      } catch (e) {
        lines.push('screenshot failed: ' + e.message.split('\n')[0]);
        shot = true;
      }
    }
    if (lines.some((l) => l.includes('Finished') || l.includes('PAGE ERROR') || l.includes('Aborted'))) break;
    await page.waitForTimeout(100);
  }
  if (!shot) {
    try {
      await page.screenshot({ path: screenshot });
    } catch (e) {
      lines.push('screenshot failed: ' + e.message.split('\n')[0]);
    }
  }
  let webgl = 'unknown';
  try {
    webgl = await page.evaluate(() => {
      const c = document.createElement('canvas');
      const gl = c.getContext('webgl2') || c.getContext('webgl');
      return gl ? gl.getParameter(gl.VERSION) : 'none';
    });
  } catch (e) {
    webgl = 'check failed: ' + e.message.split('\n')[0];
  }
  await browser.close();

  console.log('Seconds until finished: ' + ((Date.now() - started) / 1000).toFixed(1));
  console.log('Browser WebGL: ' + webgl);
  for (const l of lines) {
    if (RELEVANT.test(l)) console.log(l);
  }
  const finished = lines.find((l) => l.includes('Finished')) || '';
  if (!finished) console.log('The player did not report "Finished" within 120 s');
  process.exit(finished.includes('level complete: true') && finished.includes(', 0 script errors') ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
