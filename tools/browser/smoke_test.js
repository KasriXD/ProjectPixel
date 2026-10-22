// Runs the WebAssembly build of px-player in headless Chromium.
//
// Usage: node smoke_test.js <url> <screenshot.png>
// Plays the scripted route, waits for the player to report "Finished", saves
// a screenshot of the canvas and prints the game's log lines. Exit code 0
// only if the level was completed without script errors.
const { chromium } = require('playwright');

async function main() {
  const [url, screenshot] = process.argv.slice(2);
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = await browser.newPage({ viewport: { width: 1000, height: 640 } });
  const lines = [];
  page.on('console', (m) => lines.push(m.text()));
  page.on('pageerror', (e) => lines.push('PAGE ERROR: ' + e.message));
  await page.goto(url);
  const started = Date.now();
  while (!lines.some((l) => l.includes('Finished')) && Date.now() - started < 120000) {
    await page.waitForTimeout(250);
  }
  const canvas = await page.$('canvas');
  if (canvas) await canvas.screenshot({ path: screenshot });
  const webgl = await page.evaluate(() => {
    const c = document.createElement('canvas');
    const gl = c.getContext('webgl2') || c.getContext('webgl');
    return gl ? gl.getParameter(gl.VERSION) : 'none';
  });
  await browser.close();
  console.log('WebGL: ' + webgl);
  for (const l of lines) {
    if (/Window|Loaded|Level complete|Finished|ERROR|error|PAGE ERROR/.test(l)) console.log(l);
  }
  const finished = lines.find((l) => l.includes('Finished')) || '';
  process.exit(finished.includes('level complete: true') && finished.includes(', 0 script errors') ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
