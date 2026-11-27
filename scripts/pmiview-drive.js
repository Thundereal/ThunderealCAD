// Drive a real browser viewer through a PMI view made active in its own
// view (docs/GDandT.md sec 10.16, docs/ThinClient.md 8.11b).
//
// tests/gui/serve-pmi-view-browser.py serves the document and the page.
// What only a browser shows is read here: that the host's request turns the
// page's camera to the view's, and that letting the view go puts the page's
// camera back where it was. The menu is asked for with a ray
// from the page's eye onto the flatness's leader -- the server reads a ray as
// the pixel of the client's camera it passes through, so it has to start at
// the eye, as a right-click's does; the page is opened with ?cam= to know
// it. The entries are triggered as the page's own menu triggers them.
//
// Usage:  node scripts/pmiview-drive.js <url> <settle_ms> <ox> <oy> <oz> <dx> <dy> <dz>
// PMIVIEW_RESULT names the file the readings go to as JSON (node buffers
// stdout to a pipe); PMIVIEW_SHOTS a directory for the pictures.
const puppeteer = require(process.env.PUPPETEER_PATH || 'puppeteer-core');
const fs = require('fs');

const sleep = ms => new Promise(r => setTimeout(r, ms));

const HELPERS = () => {
  // One control op over the page's own socket, answered by id
  window.__fcOp = (op, args, ms) => new Promise((resolve) => {
    const id = 90000 + Math.floor(Math.random() * 9999);
    const done = (e) => {
      const d = e.detail;
      if (!d || d.id !== id) return;
      window.removeEventListener('fc:control', done);
      resolve(d);
    };
    window.addEventListener('fc:control', done);
    if (!window.fcviewerControlSend(JSON.stringify({id, op, ...args}))) {
      window.removeEventListener('fc:control', done);
      resolve({error: 'socket down'});
    }
    setTimeout(() => { window.removeEventListener('fc:control', done); resolve(null); }, ms);
  });
  // The fraction of pixels two pictures differ in, by more than a little
  window.__fcDiff = async (a, b) => {
    const load = async (b64) => {
      const img = new Image();
      img.src = 'data:image/png;base64,' + b64;
      await img.decode();
      const c = document.createElement('canvas');
      c.width = img.width;
      c.height = img.height;
      const g = c.getContext('2d', {willReadFrequently: true});
      g.drawImage(img, 0, 0);
      return g.getImageData(0, 0, c.width, c.height).data;
    };
    const da = await load(a), db = await load(b);
    let n = 0;
    for (let i = 0; i < da.length; i += 4) {
      const d = Math.abs(da[i] - db[i]) + Math.abs(da[i + 1] - db[i + 1])
        + Math.abs(da[i + 2] - db[i + 2]);
      if (d > 48)
        ++n;
    }
    return n / (da.length / 4);
  };
};

/// The submenu named \a text, at any depth
function submenu(items, text) {
  for (const it of items || []) {
    if (!it.items) continue;
    if ((it.text || '').replace(/&/g, '') === text) return it;
    const found = submenu(it.items, text);
    if (found) return found;
  }
  return null;
}

function walk(items, out = []) {
  for (const it of items || []) {
    if (it.items) walk(it.items, out);
    else if (!it.separator) out.push(it);
  }
  return out;
}

async function canvasShot(page, label) {
  const el = await page.$('canvas');
  const b64 = await (el || page).screenshot({encoding: 'base64'});
  if (process.env.PMIVIEW_SHOTS) {
    try {
      fs.writeFileSync(process.env.PMIVIEW_SHOTS + '/' + label + '.png',
                       Buffer.from(b64, 'base64'));
    }
    catch (e) { console.error('shot file:', e.message); }
  }
  return b64;
}

async function main() {
  const [url, settleArg, ...rayArgs] = process.argv.slice(2);
  const settle = parseInt(settleArg || '8000', 10);
  const ray = rayArgs.map(parseFloat);
  const out = {};
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    args: ['--no-sandbox', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
           '--window-size=1000,800'],
  });
  try {
    const page = await browser.newPage();
    await page.setViewport({width: 1000, height: 800});
    page.on('console', m => console.log('page:', m.text()));
    await page.evaluateOnNewDocument(HELPERS);
    await page.goto(url, {waitUntil: 'domcontentloaded', timeout: 60000});
    out.connected = await page.waitForFunction(() => window.fcviewerConnected === true,
                                               {timeout: 30000}).then(() => true, () => false);
    await sleep(settle);
    const before = await canvasShot(page, 'before');
    // A page nobody has clicked in has stated no camera (docs/ThinClient.md
    // 8.10b); a right-click would have
    out.stated = await page.evaluate(() => window.fcviewerStateCamera());
    await sleep(500);

    const menu = await page.evaluate((r) => window.__fcOp('contextMenu', {ray: r}, 10000), ray);
    const top = walk((submenu(menu && menu.items, 'PMI views') || {}).items)
      .find(e => e.text === 'Top');
    out.top = top || null;
    if (!top)
      out.menu = menu;
    if (!top)
      return out;
    out.on = await page.evaluate((m, i) => window.__fcOp('contextMenu.trigger',
                                                          {menu: m, item: i}, 10000),
                                 menu.menu, top.id);
    await sleep(3000);
    const on = await canvasShot(page, 'on');
    out.onDiff = await page.evaluate((a, b) => window.__fcDiff(a, b), before, on);

    const menu2 = await page.evaluate((r) => window.__fcOp('contextMenu', {ray: r}, 10000), ray);
    const views = walk((submenu(menu2 && menu2.items, 'PMI views') || {}).items);
    const off = views.find(e => e.text === 'Show all annotations');
    out.checked = views.filter(e => e.checked).map(e => e.text);
    out.offEntry = off || null;
    if (!off)
      return out;
    out.off = await page.evaluate((m, i) => window.__fcOp('contextMenu.trigger',
                                                           {menu: m, item: i}, 10000),
                                  menu2.menu, off.id);
    await sleep(3000);
    const after = await canvasShot(page, 'after');
    out.offDiff = await page.evaluate((a, b) => window.__fcDiff(a, b), before, after);
    return out;
  }
  finally {
    await browser.close();
  }
}

main().then((out) => {
  if (process.env.PMIVIEW_RESULT)
    fs.writeFileSync(process.env.PMIVIEW_RESULT, JSON.stringify(out, null, 1));
  console.log(JSON.stringify(out));
}, (err) => {
  console.error(err && err.stack || err);
  process.exit(1);
});
