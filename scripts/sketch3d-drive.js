// Drive the WASM viewer through a 3D sketch's edit (docs/Sketch3D.md S4).
//
// tests/gui/serve-sketch3d-edit.py checks the server side against a
// synthetic socket client that speaks the wire itself. What only a browser
// can check is that the viewer draws the 3D edit -- its curves, and its
// constraint icons in their two colours, which the stream carries as a
// BLEND texture (the texel each pixel's place from the fill to the border)
// -- and that its pointer and keys, the value editor it draws for a typed
// length included, drive the 3D tools. The serving process reads the
// document; this answers only what the page saw.
//
// Usage:  node scripts/sketch3d-drive.js <url> <object> <settle_ms>
// Phases (also appended to S3D_PHASES): settled | entered | drawn | typing
//   | typed | escaped | done
// The caller sets the colours counted here: the edited edges magenta, the
// 3D sketch's icons pure green inside a pure blue border.
// S3D_RESULT names a file the page's readings are written to as JSON;
// S3D_SHOTS a directory each counted canvas is written to.
const ppPath = process.env.PUPPETEER_PATH || 'puppeteer-core';
const puppeteer = require(ppPath);
const fs = require('fs');

const sleep = ms => new Promise(r => setTimeout(r, ms));

const phaseFile = process.env.S3D_PHASES;
function phase(name) {
  console.log('PHASE ' + name);
  if (phaseFile) {
    try { fs.appendFileSync(phaseFile, name + '\n'); }
    catch (e) { console.error('phase file:', e.message); }
  }
}

// Injected rather than driven through page.mouse (scripts/edit-drive.js
// says why): a dispatched event reaches exactly the code a real one would.
const HELPERS = () => {
  window.__fcReplies = {};
  window.addEventListener('fc:control', e => {
    const d = e.detail;
    if (d && typeof d.id === 'number' && d.id >= 9300 && d.id < 9400) window.__fcReplies[d.id] = d;
  });
  window.__fcRect = () => {
    const c = document.getElementById('canvas');
    const r = c.getBoundingClientRect();
    return {c, cx: r.left + r.width / 2, cy: r.top + r.height / 2,
            w: r.width, h: r.height};
  };
  window.__fcMouse = (type, x, y, buttons, button) => {
    const {c} = window.__fcRect();
    const target = type === 'mousedown' ? c : document;
    target.dispatchEvent(new MouseEvent(type, {
      bubbles: true, cancelable: true, view: window,
      clientX: x, clientY: y, buttons: buttons, button: button || 0}));
  };
  window.__fcClick = async (x, y) => {
    for (let i = 3; i >= 0; --i) {
      window.__fcMouse('mousemove', x + i, y, 0, 0);
      await new Promise(r => requestAnimationFrame(r));
    }
    window.__fcMouse('mousedown', x, y, 1, 0);
    await new Promise(r => setTimeout(r, 80));
    window.__fcMouse('mouseup', x, y, 0, 0);
    await new Promise(r => setTimeout(r, 400));
  };
  // A key on the page, as the view has it: the viewer sends it up
  window.__fcDocKey = (key, shift) => {
    for (const type of ['keydown', 'keyup']) {
      document.dispatchEvent(new KeyboardEvent(type, {
        bubbles: true, cancelable: true, key: key, shiftKey: !!shift}));
    }
  };
  // A key into the value editor's field, as a user typing there
  window.__fcEditorKey = async (key) => {
    const el = document.querySelector('.fc-onview-datum-value');
    if (!el) return false;
    el.focus();
    for (const type of ['keydown', 'keyup']) {
      el.dispatchEvent(new KeyboardEvent(type, {key, bubbles: true, cancelable: true}));
    }
    await new Promise(r => setTimeout(r, 300));
    return true;
  };
  window.__fcEditor = () => {
    const el = document.querySelector('.fc-onview-datum');
    if (!el) return null;
    const value = el.querySelector('.fc-onview-datum-value');
    return {value: value ? value.value : null};
  };
  // Pixels by colour in a PNG of the canvas, decoded in the page
  window.__fcCount = async (b64) => {
    const img = new Image();
    img.src = 'data:image/png;base64,' + b64;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    const g = c.getContext('2d', {willReadFrequently: true});
    g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height).data;
    const n = {magenta: 0, green: 0, blue: 0};
    for (let i = 0; i < d.length; i += 4) {
      const r = d[i], gr = d[i + 1], bl = d[i + 2];
      if (r > 150 && bl > 150 && gr < 0.5 * Math.min(r, bl))
        ++n.magenta;
      else if (gr > 150 && r < 0.4 * gr && bl < 0.4 * gr)
        ++n.green;
      else if (bl > 150 && r < 0.4 * bl && gr < 0.4 * bl)
        ++n.blue;
    }
    return n;
  };
};

async function count(page, label) {
  const r = await page.evaluate(() => {
    const b = document.getElementById('canvas').getBoundingClientRect();
    return {x: b.left, y: b.top, w: b.width, h: b.height};
  });
  const b64 = await page.screenshot({
    encoding: 'base64',
    clip: {x: Math.round(r.x), y: Math.round(r.y),
           width: Math.round(r.w), height: Math.round(r.h)},
  });
  if (process.env.S3D_SHOTS && label) {
    try {
      fs.writeFileSync(process.env.S3D_SHOTS + '/' + label + '.png',
                       Buffer.from(b64, 'base64'));
    }
    catch (e) { console.error('shot file:', e.message); }
  }
  return page.evaluate(d => window.__fcCount(d), b64);
}

// Count until `done` says so or the time runs out: the overlay comes and
// goes with a publish, not with the state change the caller waited on.
async function countUntil(page, label, done, ms) {
  const t0 = Date.now();
  let n = await count(page, label);
  while (!done(n) && Date.now() - t0 < ms) {
    await sleep(250);
    n = await count(page, label);
  }
  return n;
}

async function waitFor(page, fn, ms, ...args) {
  try {
    await page.waitForFunction(fn, {timeout: ms}, ...args);
    return true;
  }
  catch (e) {
    return false;
  }
}

async function command(page, id, name) {
  await page.evaluate((i, n) => !!window.fcviewerControlSend(JSON.stringify(
      {id: i, op: 'command', name: n})), id, name);
  await waitFor(page, (i) => !!window.__fcReplies[i], 15000);
  return page.evaluate((i) => window.__fcReplies[i] || null, id);
}

(async () => {
  const url = process.argv[2];
  const obj = process.argv[3] || 'Sketch3D';
  const settleMs = parseInt(process.argv[4] || '12000', 10);
  if (!url) {
    console.error('usage: node sketch3d-drive.js <url> [object] [settle_ms]');
    process.exit(2);
  }
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    protocolTimeout: 180000,
    args: ['--no-sandbox', '--enable-unsafe-swiftshader',
           '--use-angle=swiftshader', '--window-size=1100,900',
           '--disable-background-timer-throttling',
           '--disable-backgrounding-occluded-windows',
           '--disable-renderer-backgrounding'],
  });
  const out = {};
  try {
    const page = await browser.newPage();
    page.setDefaultTimeout(60000);
    await page.setViewport({width: 1100, height: 900});
    const verbose = !!process.env.S3D_VERBOSE;
    let kept = 0;
    page.on('console', m => {
      const t = m.text();
      if (kept < (verbose ? 400 : 60)
          && (verbose || /edit mode|edit refused|websocket|onview|refused/i.test(t))) {
        ++kept;
        console.log('page:', t);
      }
    });
    await page.goto(url, {waitUntil: 'domcontentloaded'});
    await page.evaluate(HELPERS);
    await sleep(settleMs);
    out.view = await count(page, 'view');
    phase('settled');

    // 1. Into the 3D sketch's edit, on the page's own ask
    out.editCall = await page.evaluate((o) => !!window.fcviewerEdit(o, 0, ''), obj);
    out.entered = await waitFor(page, (o) => window.fcviewerEditing === o, 20000, obj);
    out.edit = await countUntil(page, 'edit', n => n.magenta > 100 && n.blue > 20, 10000);
    phase('entered');

    // 2. A line by two clicks, below the middle where the working plane is in
    // front of the eye whatever the camera
    out.lineReply = await command(page, 9301, 'Sketcher_CreateLine');
    await page.evaluate(async () => {
      const {cx, cy} = window.__fcRect();
      await window.__fcClick(cx - 90, cy + 60);
      await window.__fcClick(cx + 90, cy + 100);
    });
    await sleep(800);
    phase('drawn');
    await page.evaluate(() => { window.__fcDocKey('Escape'); });
    await sleep(500);
    await page.evaluate(() => { window.__fcDocKey('Escape'); });
    await sleep(800);

    // 3. A length typed: a click, the pointer moved on, a digit on the view
    // opens the value editor, the rest typed into it
    out.typedReply = await command(page, 9302, 'Sketcher_CreateLine');
    await page.evaluate(async () => {
      const {cx, cy} = window.__fcRect();
      await window.__fcClick(cx - 60, cy + 160);
      for (let i = 0; i <= 8; ++i) {
        window.__fcMouse('mousemove', cx - 60 + i * 15, cy + 160, 0, 0);
        await new Promise(r => requestAnimationFrame(r));
      }
      await new Promise(r => setTimeout(r, 300));
      window.__fcDocKey('2');
    });
    out.editorShown = await waitFor(page, () => !!document.querySelector('.fc-onview-datum'), 15000);
    out.editorOpened = await page.evaluate(() => window.__fcEditor());
    phase('typing');
    await page.evaluate(async () => {
      await window.__fcEditorKey('5');
      await window.__fcEditorKey('Enter');
    });
    out.editorClosed = await waitFor(page, () => !document.querySelector('.fc-onview-datum'), 10000);
    await sleep(800);
    phase('typed');
    await page.evaluate(() => { window.__fcDocKey('Escape'); });
    await sleep(500);
    await page.evaluate(() => { window.__fcDocKey('Escape'); });
    await sleep(500);

    // 4. Out, by End edit's shortcut: the server ends the session and tells
    // the page
    await page.evaluate(() => {
      document.dispatchEvent(new KeyboardEvent('keydown', {bubbles: true, key: 'Shift', shiftKey: true}));
      window.__fcDocKey('Escape', true);
      document.dispatchEvent(new KeyboardEvent('keyup', {bubbles: true, key: 'Shift'}));
    });
    out.left = await waitFor(page, () => !window.fcviewerEditing, 20000);
    out.escaped = await countUntil(page, 'escaped', n => n.magenta < 20, 10000);
    phase('escaped');
    await sleep(500);
  }
  catch (e) {
    out.error = String(e && e.message || e);
    console.error('DRIVE ERROR', e);
  }
  finally {
    await browser.close();
  }
  if (process.env.S3D_RESULT) {
    try { fs.writeFileSync(process.env.S3D_RESULT, JSON.stringify(out)); }
    catch (e) { console.error('result file:', e.message); }
  }
  console.log('RESULT ' + JSON.stringify(out));
  phase('done');
  if (out.error)
    process.exit(1);
})().catch(e => { console.error('DRIVE ERROR', e); process.exit(1); });
