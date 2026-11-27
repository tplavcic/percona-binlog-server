// Copyright (c) 2026 Percona and/or its affiliates. GPL-2.0 (see COPYING).
//
// Smoke check of a generated dashboard page: loads it at desktop and phone
// width in Chromium (Playwright) and prints, per width, the page errors,
// the page width (no horizontal scroll: equal to the viewport) and the
// number of issue items. Exit code 1 on a page error or a horizontal scroll.
//   node extra/qa/report/check_report.js /absolute/path/pbs-findings.html
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch();
  let failed = false;
  for (const width of [1100, 390]) {
    const page = await browser.newPage({ viewport: { width, height: 900 } });
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    await page.goto('file://' + process.argv[2]);
    await page.waitForTimeout(500);
    const [lede, scrollWidth, items] = await page.evaluate(() => [
      document.getElementById('lede').textContent,
      document.documentElement.scrollWidth,
      document.querySelectorAll('.item').length]);
    console.log(width, errors, scrollWidth, items, lede);
    failed = failed || errors.length > 0 || scrollWidth > width || items === 0;
  }
  await browser.close();
  process.exit(failed ? 1 : 0);
})();
