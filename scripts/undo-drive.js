// Open two viewer pages, two users of one served document, and drive the
// undo list (docs/MultiViewEdit.md sec 12.11, web/src/undolist.tsx): alice's
// list, its group's row opened, her undo refused for a step of bob's, asked
// of bob, and bob agreeing on his page.
//
// Usage:  node scripts/undo-drive.js <alice url> <bob url> [timeout_ms]
//   each url wants `?doc=<name>&token=<secret>&client=<name>`, the serve
//   started with an invitation for each name (tests/gui/wsclient.py
//   invitation()).
//
// Prints one line `REPORT <json>`: alice's rows, the steps of her group row
// opened, the card her undo raised and its buttons, what she was told
// after asking, the question bob's page put, and what alice's page said
// when bob answered. Exits 0 when alice's page said the undo went.
//
// PUPPETEER_PATH names a puppeteer-core install, CHROME the browser, and
// CHROME_LIBS is prepended to its LD_LIBRARY_PATH, as scripts/request-drive.js.
const puppeteer = require(process.env.PUPPETEER_PATH || 'puppeteer-core');

(async () => {
  const [aliceUrl, bobUrl, timeoutArg] = process.argv.slice(2);
  if (!aliceUrl || !bobUrl) {
    console.error('usage: node undo-drive.js <alice url> <bob url> [timeout_ms]');
    process.exit(2);
  }
  const timeout = +(timeoutArg || 60000);
  const env = Object.assign({}, process.env);
  if (process.env.CHROME_LIBS)
    env.LD_LIBRARY_PATH = process.env.CHROME_LIBS
      + (env.LD_LIBRARY_PATH ? ':' + env.LD_LIBRARY_PATH : '');
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    args: ['--no-sandbox', '--window-size=1280,1000',
           '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
    env,
    // two viewers render on one CPU: a page answers slowly
    protocolTimeout: timeout * 3,
  });
  const report = { rows: [], opened: [], card: null, buttons: [], asked: null,
                   question: null, said: null };
  let code = 2;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const open = async (url) => {
    const page = await browser.newPage();
    await page.setViewport({ width: 900, height: 700 });
    page.on('pageerror', (e) => console.log('[pageerror] ' + e.message));
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout });
    await page.waitForSelector('.fc-launcher-undo .fc-launch', { timeout });
    return page;
  };
  const text = (page, sel) => page.evaluate((s) =>
    [...document.querySelectorAll(s)].map((el) => (el.textContent || '').trim()), sel);
  const step = (what) => console.log('[step] ' + what + ' ' + new Date().toISOString());
  const until = async (page, sel) => {
    await page.waitForSelector(sel, { timeout });
    return (await text(page, sel))[0];
  };
  try {
    const alice = await open(aliceUrl);
    step('alice open');
    const bob = await open(bobUrl);
    step('bob open');
    // the pages learn what they may do from the host's answer to their hello
    await sleep(+(process.env.FC_UNDO_SETTLE || 4000));

    await alice.bringToFront();
    await alice.click('.fc-launcher-undo .fc-launch');
    step('list opened');
    await alice.waitForSelector('.fc-undo-panel .fc-undo-take', { timeout });
    report.rows = await text(alice, '.fc-undo-panel .fc-undo-take');
    // the group's row opened: its steps
    const exp = await alice.$('.fc-undo-panel button.fc-undo-exp');
    if (exp) {
      await exp.click();
      await alice.waitForSelector('.fc-undo-step', { timeout });
      report.opened = await text(alice, '.fc-undo-step');
    }
    // the newest row taken: refused for bob's step
    const rows = await alice.$$('.fc-undo-panel .fc-undo-take');
    await rows[0].click();
    report.card = await until(alice, '.fc-undo-card .fc-undo-says');
    step('card');
    report.buttons = await text(alice, '.fc-undo-card .fc-undo-act');
    await alice.click('.fc-undo-card .fc-undo-with');
    report.asked = await until(alice, '.fc-undo-note');

    // bob's page asks him
    await bob.bringToFront();
    report.question = await until(bob, '.fc-consent .fc-undo-says');
    step('question');
    await bob.click('.fc-consent .fc-consent-allow');
    // and alice's says how it went
    report.said = await until(alice, '.fc-consent-said');
    code = report.said && report.said.includes('agreed') ? 0 : 1;
  } catch (e) {
    console.log('[drive] ' + e.message);
  } finally {
    console.log('REPORT ' + JSON.stringify(report));
    await browser.close();
  }
  process.exit(code);
})();
