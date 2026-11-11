const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 320, height: 568 }, deviceScaleFactor: 2 });
  page.on('console', m => { if (/error/i.test(m.text())) console.log('console:', m.text()); });
  const shots = process.argv.slice(2);
  for (const s of shots) {
    const [mode, nav] = s.split(':');
    await page.goto(`http://127.0.0.1:8765/index.html?mode=${mode}&nav=${nav}`);
    await page.waitForFunction(() => window.RENDERED === true, null, { timeout: 60000 });
    await page.waitForTimeout(500);
    const out = `${process.env.OUT}/${mode}-${nav === '1' ? 'nav' : 'browse'}.png`;
    await page.screenshot({ path: out });
    console.log('wrote', out);
  }
  await browser.close();
})();
