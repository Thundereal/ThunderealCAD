// Drive one real browser viewer through a served document's Animation
// (docs/Animation.md sec 9, "Streaming").
//
// tests/gui/serve-animation-browser.py serves the document and plays,
// pauses and stops its Animation as this script's phases pass. What only a
// browser can show is the playing: the host sends a clip and its clock and
// then publishes nothing, and the viewer moves the box by itself.
//
// The reading is pixels by colour: the box that moves is red, the one that
// stays green, so a centroid says where each is drawn without knowing
// where the viewer put its camera. With each reading goes what the viewer
// says it is playing (window.fcviewerAnimationState).
//
// Usage:  node scripts/animation-drive.js <url> <settle_ms>
// ANIMATION_PHASES and ANIMATION_RESULT name files (node buffers stdout to
// a pipe): the phases as they pass, and the readings as JSON.
// ANIMATION_SHOTS names a folder to keep each reading's canvas in, as
// <name>.png; ANIMATION_SAMPLES and ANIMATION_GAP_MS say how many readings
// are taken while it plays and how long is waited between them (8, 200);
// ANIMATION_SIZE is the window, as WxH (900x760).
const ppPath = process.env.PUPPETEER_PATH || 'puppeteer-core';
const puppeteer = require(ppPath);
const fs = require('fs');

const sleep = ms => new Promise(r => setTimeout(r, ms));

const phaseFile = process.env.ANIMATION_PHASES;
function phase(name) {
  console.log('PHASE ' + name);
  if (phaseFile) {
    try { fs.appendFileSync(phaseFile, name + '\n'); }
    catch (e) { console.error('phase file:', e.message); }
  }
}

// Injected: screenshots are decoded in the page (scripts/peersel-drive.js
// says why).
const HELPERS = () => {
  window.__fcRect = () => {
    const c = document.getElementById('canvas');
    const r = c.getBoundingClientRect();
    return {x: r.left, y: r.top, w: r.width, h: r.height};
  };
  window.__fcShots = {};
  window.__fcCapture = async (name, b64) => {
    const img = new Image();
    img.src = 'data:image/png;base64,' + b64;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    const g = c.getContext('2d', {willReadFrequently: true});
    g.drawImage(img, 0, 0);
    window.__fcShots[name] = g.getImageData(0, 0, c.width, c.height);
    return {w: c.width, h: c.height};
  };
  // Pixel counts of the three colours and each one's centroid
  window.__fcColours = (name) => {
    const s = window.__fcShots[name];
    if (!s)
      return null;
    // No blue: the background's gradient is blue
    const out = {red: {n: 0, x: 0, y: 0}, green: {n: 0, x: 0, y: 0},
                 yellow: {n: 0, x: 0, y: 0}};
    for (let i = 0, p = 0; i < s.data.length; i += 4, ++p) {
      const r = s.data[i], g = s.data[i + 1], b = s.data[i + 2];
      let k = null;
      if (r > 110 && g > 110 && b < 0.5 * Math.min(r, g) && Math.abs(r - g) < 0.3 * r)
        k = 'yellow';
      else if (r > 110 && r > 1.8 * g && r > 1.8 * b)
        k = 'red';
      else if (g > 110 && g > 1.8 * r && g > 1.8 * b)
        k = 'green';
      if (!k)
        continue;
      const o = out[k];
      ++o.n;
      o.x += p % s.width;
      o.y += Math.floor(p / s.width);
    }
    for (const k in out) {
      const o = out[k];
      if (o.n) {
        o.x = Math.round(10 * o.x / o.n) / 10;
        o.y = Math.round(10 * o.y / o.n) / 10;
      }
    }
    return out;
  };
};

// The state first: what it says is of the frame the shot then catches,
// give or take one
async function reading(page, name) {
  const state = await page.evaluate(
      () => window.fcviewerAnimationState ? window.fcviewerAnimationState() : null);
  const rect = await page.evaluate(() => window.__fcRect());
  const b64 = await page.screenshot({
    encoding: 'base64',
    clip: {x: Math.round(rect.x), y: Math.round(rect.y),
           width: Math.round(rect.w), height: Math.round(rect.h)},
  });
  if (process.env.ANIMATION_SHOTS) {
    try {
      fs.writeFileSync(process.env.ANIMATION_SHOTS + '/' + name + '.png',
                       Buffer.from(b64, 'base64'));
    }
    catch (e) { console.error('shot file:', e.message); }
  }
  await page.evaluate((n, d) => window.__fcCapture(n, d), name, b64);
  const colours = await page.evaluate(n => window.__fcColours(n), name);
  // And after: a screenshot can take seconds, and a reading whose two
  // states disagree says nothing of which frame was caught
  const after = await page.evaluate(
      () => window.fcviewerAnimationState ? window.fcviewerAnimationState() : null);
  return {state: state, after: after, colours: colours, at: Date.now()};
}

async function until(page, test, timeoutMs) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    const state = await page.evaluate(
        () => window.fcviewerAnimationState ? window.fcviewerAnimationState() : null);
    if (state && test(state))
      return state;
    await sleep(100);
  }
  return null;
}

(async () => {
  const url = process.argv[2];
  const settleMs = parseInt(process.argv[3] || '9000', 10);
  if (!url) {
    console.error('usage: node animation-drive.js <url> [settle_ms]');
    process.exit(2);
  }
  const samples = parseInt(process.env.ANIMATION_SAMPLES || '8', 10);
  const gapMs = parseInt(process.env.ANIMATION_GAP_MS || '200', 10);
  const size = (process.env.ANIMATION_SIZE || '900x760').split('x').map(Number);
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    protocolTimeout: 240000,
    args: ['--no-sandbox', '--enable-unsafe-swiftshader',
           '--use-angle=swiftshader', '--window-size=' + size[0] + ',' + size[1],
           '--disable-background-timer-throttling',
           '--disable-backgrounding-occluded-windows',
           '--disable-renderer-backgrounding'],
  });
  const out = {};
  try {
    const page = (await browser.pages())[0] || await browser.newPage();
    page.setDefaultTimeout(60000);
    await page.setViewport({width: size[0], height: size[1]});
    let kept = 0;
    page.on('console', m => {
      const t = m.text();
      if (kept < 80 && /clip|scene update|refused|websocket|error/i.test(t)) {
        ++kept;
        console.log('page:', t);
      }
    });
    page.on('pageerror', e => console.log('page ERROR: ' + e.message));
    await page.goto(url, {waitUntil: 'domcontentloaded'});
    await page.evaluate(HELPERS);
    await sleep(settleMs);
    out.rest = await reading(page, 'rest');
    phase('ready');

    // 1. The host plays. The viewer is sent the clip and the clock, and
    // from there moves the box itself.
    out.playing = await until(page, s => s.plays && s.playing, 40000);
    out.samples = [];
    for (let i = 0; i < samples; ++i) {
      out.samples.push(await reading(page, 'play' + i));
      await sleep(gapMs);
    }
    phase('played');

    // 2. The host pauses: the pose it stopped in is published.
    out.pausedState = await until(page, s => s.active && !s.playing, 20000);
    await sleep(2000);
    out.paused1 = await reading(page, 'paused1');
    await sleep(600);
    out.paused2 = await reading(page, 'paused2');
    phase('paused');

    // 3. The host stops: the document as designed.
    out.stoppedState = await until(page, s => !s.active, 20000);
    await sleep(2000);
    out.stopped = await reading(page, 'stopped');
    phase('stopped');
  }
  catch (e) {
    out.error = (e && e.stack) || String(e);
    console.error(out.error);
  }
  finally {
    try { await browser.close(); } catch (e) { /* best effort */ }
  }
  if (process.env.ANIMATION_RESULT) {
    try {
      fs.writeFileSync(process.env.ANIMATION_RESULT, JSON.stringify(out, null, 1));
    }
    catch (e) {
      console.error('result file:', e.message);
    }
  }
  phase('done');
  process.exit(out.error ? 1 : 0);
})();
