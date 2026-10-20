// Drive an edit's handles with a finger in the WASM viewer
// (docs/OnViewValueEditing.md sec 9.3, entry 189).
//
// A real Chrome, headful, with touch emulated: the touches are CDP's
// Input.dispatchTouchEvent, which reaches the page as the touch events a
// touch screen gives, through the compositor -- not events built in the
// page. The page is entered into the edit of the served fillet, the radius
// arrow is found by the client's own pick (window.fcviewerEditHandleAt,
// what a touch decides by), and then:
//
//   tap       a finger on the arrow, lifted: the value is active;
//   drag      a finger on the arrow, moved: the radius changes and the
//             camera stays (as the page's HUD reads it);
//   hold      a finger held on the arrow: the loupe, then the handle's
//             menu ("One radius"); its Settings, "Default drag", "Fine"
//             tapped in that menu;
//   finedrag  the same drag as the first, now fine;
//   label     a tap on the value's label: its box in the page, a value
//             typed into it;
//   orbit     a finger off the handles, moved: the camera turns and no
//             radius changes.
//
// The serving process reads the radius and the counters on the phase
// marks. Usage: node scripts/handle-touch-drive.js <url> <settle_ms>
// TOUCH_PHASES names the phase file, TOUCH_RESULT the page's readings
// (JSON), TOUCH_SHOTS a directory for screenshots.
const ppPath = process.env.PUPPETEER_PATH || 'puppeteer-core';
const puppeteer = require(ppPath);
const fs = require('fs');

const sleep = ms => new Promise(r => setTimeout(r, ms));

const phaseFile = process.env.TOUCH_PHASES;
function phase(name) {
  console.log('PHASE ' + name);
  if (phaseFile) {
    try { fs.appendFileSync(phaseFile, name + '\n'); }
    catch (e) { console.error('phase file:', e.message); }
  }
}

async function shot(page, label) {
  if (!process.env.TOUCH_SHOTS)
    return;
  try {
    await page.screenshot({path: process.env.TOUCH_SHOTS + '/' + label + '.png'});
  }
  catch (e) { console.error('shot:', e.message); }
}

// The canvas rect, in CSS px
async function canvasRect(page) {
  return page.evaluate(() => {
    const r = document.getElementById('canvas').getBoundingClientRect();
    return {x: r.left, y: r.top, w: r.width, h: r.height};
  });
}

// Every point of a grid over the canvas where a finger lands on the
// edit's handles, by the client's own pick
async function handleHits(page, step) {
  return page.evaluate((step) => {
    const r = document.getElementById('canvas').getBoundingClientRect();
    const hits = [];
    for (let y = r.top + step; y < r.bottom - step; y += step)
      for (let x = r.left + step; x < r.right - step; x += step)
        if (window.fcviewerEditHandleAt(x, y))
          hits.push([x, y]);
    return hits;
  }, step);
}

// The hit with the most hits around it: the middle of the largest piece
function densest(hits, reach) {
  let best = null, bestN = -1;
  for (const [x, y] of hits) {
    let n = 0;
    for (const [u, v] of hits)
      if (Math.abs(u - x) <= reach && Math.abs(v - y) <= reach)
        ++n;
    if (n > bestN) {
      bestN = n;
      best = [x, y];
    }
  }
  return best;
}

// Green pixels within 24 px of `at`: an active value's arrows are green,
// the others red
async function greenNear(page, at) {
  const b = await page.screenshot({
    encoding: 'base64',
    clip: {x: Math.round(at[0] - 24), y: Math.round(at[1] - 24), width: 48, height: 48},
  });
  return page.evaluate(async (b64) => {
    const img = new Image();
    img.src = 'data:image/png;base64,' + b64;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    const g = c.getContext('2d', {willReadFrequently: true});
    g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4)
      if (d[i + 1] > 150 && d[i] < 110 && d[i + 2] < 110)
        ++n;
    return n;
  }, b);
}

// The camera as the page's HUD shows it: yaw, pitch, distance
async function hudCamera(page) {
  return page.evaluate(() => {
    const m = /yaw\s+(-?[\d.]+)\s+pitch\s+(-?[\d.]+)\s+dist\s+(-?[\d.]+)/
        .exec(document.body.innerText);
    return m ? [m[1], m[2], m[3]].join(',') : null;
  });
}

(async () => {
  const url = process.argv[2];
  const settleMs = parseInt(process.argv[3] || '10000', 10);
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: false,
    protocolTimeout: 180000,
    args: ['--window-size=1100,900', '--ignore-gpu-blocklist',
           '--disable-background-timer-throttling',
           '--disable-backgrounding-occluded-windows',
           '--disable-renderer-backgrounding'],
  });
  const out = {};
  let cdp = null;
  const touch = async (type, x, y) => {
    await cdp.send('Input.dispatchTouchEvent', {
      type, touchPoints: type === 'touchEnd' ? [] : [{x, y}],
    });
  };
  // A finger from (x, y) along (dx, dy) per step, `steps` times
  const drag = async (x, y, dx, dy, steps) => {
    if (process.env.TOUCH_MOUSE) {
      // the control: the same drag with the mouse
      const mouse = (type, mx, my) => cdp.send('Input.dispatchMouseEvent', {
        type, x: mx, y: my, button: 'left', clickCount: 1,
        buttons: type === 'mouseReleased' ? 0 : 1});
      await mouse('mouseMoved', x, y);
      await sleep(200);
      await mouse('mousePressed', x, y);
      await sleep(80);
      for (let i = 1; i <= steps; ++i) {
        await mouse('mouseMoved', x + dx * i, y + dy * i);
        await sleep(120);
      }
      await sleep(200);
      await mouse('mouseReleased', x + dx * steps, y + dy * steps);
      return;
    }
    // At a finger's pace: past the tap slop well inside the hold's 350 ms,
    // or the hold's loupe takes the finger, as it does anywhere
    await touch('touchStart', x, y);
    await sleep(40);
    for (let i = 1; i <= steps; ++i) {
      await touch('touchMove', x + dx * i, y + dy * i);
      await sleep(40);
    }
    await sleep(200);
    await touch('touchEnd', 0, 0);
  };
  const tap = async (x, y) => {
    await touch('touchStart', x, y);
    await sleep(90);
    await touch('touchEnd', 0, 0);
  };
  // Tap the menu row whose text is `text` (its middle, through CDP like
  // any other touch); false when the menu has no such row
  const tapRow = async (page, text) => {
    const at = await page.evaluate((text) => {
      for (const b of document.querySelectorAll('.fc-ctxmenu .fc-ctx-item')) {
        const t = b.querySelector('.fc-ctx-text');
        if (t && t.textContent.trim() === text) {
          const r = b.getBoundingClientRect();
          return [r.left + r.width / 2, r.top + r.height / 2];
        }
      }
      return null;
    }, text);
    if (!at)
      return false;
    await tap(at[0], at[1]);
    await sleep(700);
    return true;
  };
  const menuRows = page => page.evaluate(() =>
    [...document.querySelectorAll('.fc-ctxmenu .fc-ctx-item .fc-ctx-text')]
        .map(e => e.textContent.trim()));
  // Find the arrow again: it moves as the radius does. Below the band
  // under the tool bars, where the top-middle label is
  const findHandle = async (page, label) => {
    const r = await canvasRect(page);
    const hits = (await handleHits(page, 4)).filter(([x, y]) => y >= r.y + 90);
    out[label + 'Hits'] = hits.length;
    return hits.length ? densest(hits, 12) : null;
  };
  try {
    const page = await browser.newPage();
    page.setDefaultTimeout(60000);
    await page.setViewport({width: 1000, height: 760, hasTouch: true});
    cdp = await page.target().createCDPSession();
    await cdp.send('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    page.on('console', m => {
      const t = m.text();
      if (/edit mode|edit refused|websocket (connected|lost)|error/i.test(t))
        console.log('page:', t);
    });
    await page.goto(url, {waitUntil: 'domcontentloaded'});
    await sleep(settleMs);
    phase('settled');

    // asked again until the socket is up and the answer came back: the
    // ask is refused here while the socket is still connecting
    out.asks = 0;
    for (let i = 0; i < 160 && !(await page.evaluate(() => window.fcviewerEditing)); ++i) {
      if (i % 12 === 0 && await page.evaluate(() => window.fcviewerEdit('Fillet', 0, '')))
        ++out.asks;
      await sleep(250);
    }
    out.editing = await page.evaluate(() => window.fcviewerEditing || null);
    // the overlay comes with a publish after the answer
    let h = null;
    for (let i = 0; i < 40 && !h; ++i) {
      await sleep(250);
      h = await findHandle(page, 'entered');
    }
    out.handle = h;
    await shot(page, '1-entered');
    phase('entered');
    if (!h)
      throw new Error('no handle found on the canvas');
    const r = await canvasRect(page);
    // inward: from the handle to the middle of the canvas, the way the
    // radius arrow points from above, which makes the radius larger
    let ox = (r.x + r.w / 2) - h[0], oy = (r.y + r.h / 2) - h[1];
    const ol = Math.hypot(ox, oy) || 1;
    ox /= ol;
    oy /= ol;
    out.inward = [ox, oy];

    // tap, first: no value is active yet (the arrow red), and the tap
    // makes the radius active (green) -- as a drag's release does too
    out.tapAt = h;
    out.greenBeforeTap = await greenNear(page, h);
    await tap(h[0], h[1]);
    await sleep(1200);
    out.greenAfterTap = await greenNear(page, h);
    await shot(page, '2-tapped');
    phase('tapped');

    // drag
    out.cameraBeforeDrag = await hudCamera(page);
    await drag(h[0], h[1], 4 * ox, 4 * oy, 10);
    await sleep(1500);
    out.cameraAfterDrag = await hudCamera(page);
    await shot(page, '3-dragged');
    phase('dragged');

    // hold: the loupe picks above the finger, so the finger goes below the
    // arrow by the lift
    h = await findHandle(page, 'hold');
    const lift = parseFloat(process.env.TOUCH_LIFT || '0');
    out.holdAt = h;
    if (h) {
      await touch('touchStart', h[0], h[1] + lift);
      await sleep(1400);
      out.menuWhileHeld = await menuRows(page);
      await shot(page, '4-held');
      await touch('touchEnd', 0, 0);
      await sleep(500);
    }
    out.menu = await menuRows(page);
    phase('held');
    out.tappedSettings = await tapRow(page, 'Settings');
    out.settingsRows = await menuRows(page);
    out.tappedDefaultDrag = await tapRow(page, 'Default drag');
    out.defaultDragRows = await menuRows(page);
    out.tappedFine = await tapRow(page, 'Fine');
    await shot(page, '5-fine');
    phase('fine');
    // close the menu as a phone does: a tap outside it, on the empty
    // background of the canvas's bottom right
    await tap(r.x + r.w * 0.92, r.y + r.h * 0.9);
    await sleep(700);
    out.menuAfterClose = await menuRows(page);

    // the fine drag, the same as the first
    h = await findHandle(page, 'fine');
    out.fineAt = h;
    if (h) {
      await drag(h[0], h[1], 4 * ox, 4 * oy, 10);
      await sleep(1500);
    }
    await shot(page, '6-finedragged');
    phase('finedragged');

    // the label: a tap on it opens its box in the page, which takes the
    // keys -- the phone's keyboard, here CDP's. The top-middle label is
    // looked for in the band under the tool bars.
    const top = (await handleHits(page, 4)).filter(([x, y]) => y < r.y + 90);
    out.labelHits = top.length;
    // its middle: the hits fill its box evenly, so the densest is a corner
    const lab = top.length
        ? [top.reduce((a, p) => a + p[0], 0) / top.length,
           top.reduce((a, p) => a + p[1], 0) / top.length]
        : null;
    out.labelAt = lab;
    if (lab) {
      await tap(lab[0], lab[1]);
      for (let i = 0; i < 20 && !(await page.$('.fc-onview-box')); ++i)
        await sleep(250);
      out.boxShown = !!(await page.$('.fc-onview-box'));
      out.boxFocused = await page.evaluate(() =>
          !!document.activeElement
          && document.activeElement.classList.contains('fc-onview-box'));
      if (out.boxShown && !out.boxFocused) {
        // as a user would: a tap on the box
        const b = await page.evaluate(() => {
          const r = document.querySelector('.fc-onview-box').getBoundingClientRect();
          return [r.left + r.width / 2, r.top + r.height / 2];
        });
        await tap(b[0], b[1]);
        await sleep(600);
      }
      out.boxText = await page.evaluate(() => {
        const e = document.querySelector('.fc-onview-box');
        return e ? e.value : null;
      });
      await shot(page, '7-label');
      await page.keyboard.type('4');
      await sleep(300);
      await page.keyboard.press('Enter');
      await sleep(2000);
      out.boxTextAfter = await page.evaluate(() => {
        const e = document.querySelector('.fc-onview-box');
        return e ? e.value : null;
      });
    }
    await shot(page, '8-typed');
    phase('typed');

    // orbit: a finger off the handles, at the bottom left quarter
    out.cameraBeforeOrbit = await hudCamera(page);
    const off = [r.x + r.w * 0.25, r.y + r.h * 0.75];
    out.offOnHandle = await page.evaluate((p) => window.fcviewerEditHandleAt(p[0], p[1]), off);
    await drag(off[0], off[1], 6, 0, 10);
    await sleep(1500);
    out.cameraAfterOrbit = await hudCamera(page);
    await shot(page, '9-orbited');
    phase('orbited');

    out.left = await page.evaluate(() => window.fcviewerResetEdit());
    await sleep(1500);
    phase('done');
  }
  catch (e) {
    out.error = String(e && e.stack || e);
    console.error(out.error);
  }
  finally {
    if (process.env.TOUCH_RESULT)
      fs.writeFileSync(process.env.TOUCH_RESULT, JSON.stringify(out, null, 1));
    await browser.close();
  }
})();
