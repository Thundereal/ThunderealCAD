// Drive a real browser viewer through the surface analysis matcaps
// (docs/Surface.md sec 7, U4): curvature, draft and zebra, drawn by the
// wasm viewer from the settings the snapshot carries (SceneDump v90).
//
// tests/gui/serve-matcap-browser.py serves a sphere and a box and sets the
// matcap at each phase -- studio, then curvature, draft and zebra -- and
// touches the document so the next publish carries it. The page's canvas
// is read here, its pixels counted by colour: red, green, yellow, blue,
// dark, light, and how often a row turns between dark and light.
//
// Usage:  node scripts/matcap-drive.js <url> <settle_ms>
// MATCAP_PHASES and MATCAP_RESULT name files (node buffers stdout to a
// pipe); MATCAP_SHOTS a directory each counted canvas is written to.
const puppeteer = require(process.env.PUPPETEER_PATH || 'puppeteer-core');
const fs = require('fs');

const sleep = ms => new Promise(r => setTimeout(r, ms));

const phaseFile = process.env.MATCAP_PHASES;
async function phase(name) {
  console.log('PHASE ' + name);
  if (phaseFile) {
    try { fs.appendFileSync(phaseFile, name + '\n'); }
    catch (e) { console.error('phase file:', e.message); }
  }
  await sleep(700);
}

const HELPERS = () => {
  window.__fcControl = [];
  window.addEventListener('fc:control', e => {
    const d = e.detail;
    window.__fcControl.push(typeof d === 'string' ? d : JSON.stringify(d));
  });
  // One op through the viewer's own control socket, and its answer
  window.__fcOp = (id, fields) => new Promise(resolve => {
    const want = JSON.stringify(id);
    const t0 = Date.now();
    const poll = () => {
      // the reply: this id, and ok or an error; not a message that only
      // has the same text in it
      const hit = window.__fcControl.find(s => {
        if (s.indexOf(want) < 0)
          return false;
        try {
          const o = JSON.parse(s);
          return o.id === id && ('ok' in o || 'error' in o);
        }
        catch (e) { return false; }
      });
      if (hit || Date.now() - t0 > 10000)
        return resolve(hit || null);
      setTimeout(poll, 50);
    };
    window.fcviewerControlSend(JSON.stringify(Object.assign({id: id}, fields)));
    poll();
  });
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
    const n = {red: 0, green: 0, yellow: 0, blue: 0, dark: 0, light: 0, turns: 0};
    for (let i = 0; i < d.length; i += 4) {
      const r = d[i], gr = d[i + 1], bl = d[i + 2];
      const hi = Math.max(r, gr, bl), lo = Math.min(r, gr, bl);
      // warm: red to orange, as curvature saturates towards red
      if (r > 150 && r > gr + 40 && r > bl + 40) ++n.red;
      else if (gr > 150 && gr > r + 40 && gr > bl + 40) ++n.green;
      else if (r > 170 && gr > 160 && bl < 0.7 * Math.min(r, gr)) ++n.yellow;
      else if (bl > 150 && r < 0.55 * bl && gr < 0.75 * bl) ++n.blue;
      if (hi < 45) ++n.dark;
      else if (lo > 215) ++n.light;
    }
    // turns between dark and light along the middle rows: the stripes
    for (const fy of [0.4, 0.5, 0.6]) {
      const y = Math.floor(c.height * fy);
      let last = 0;
      for (let x = 0; x < c.width; ++x) {
        const i = 4 * (y * c.width + x);
        const v = (d[i] + d[i + 1] + d[i + 2]) / 3;
        const s = v < 60 ? -1 : v > 200 ? 1 : 0;
        if (s && last && s !== last) ++n.turns;
        if (s) last = s;
      }
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
  if (process.env.MATCAP_SHOTS && label) {
    try {
      fs.writeFileSync(process.env.MATCAP_SHOTS + '/' + label + '.png',
                       Buffer.from(b64, 'base64'));
    }
    catch (e) { console.error('shot file:', e.message); }
  }
  return page.evaluate(d => window.__fcCount(d), b64);
}

(async () => {
  const url = process.argv[2];
  const settleMs = parseInt(process.argv[3] || '9000', 10);
  if (!url) {
    console.error('usage: node matcap-drive.js <url> [settle_ms]');
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
    page.on('pageerror', e => console.log('  PAGEERROR: ' + (e.stack || e.message)));
    // the viewer's own account of its streaming (?stream), for a scene slow
    // to come in: each assembly says its draws and how many of them are
    // still boxes. A commit's "scene update" line is not enough: the
    // arrivals after a commit complete the scene without another one.
    out.draws = 0;
    out.whole = 0;
    page.on('console', m => {
      const t = m.text();
      const d = /scene at (\d+) draws, (\d+) of them coarse/.exec(t);
      if (d) {
        out.draws = Math.max(out.draws, parseInt(d[1], 10));
        if (parseInt(d[2], 10) === 0)
          out.whole = Math.max(out.whole, parseInt(d[1], 10));
      }
      if (/scene update|scene at|box|never arrived|grace/i.test(t))
        console.log('  page: ' + (Date.now() % 1000000) + ' ' + t.slice(0, 200));
    });
    await page.goto(url, {waitUntil: 'domcontentloaded', timeout: 60000});
    await page.evaluate(HELPERS);
    // the whole scene: each object streams in after its bounding box, all
    // the draws (6: each object's faces, edges and vertices) and none of
    // them a box; a scene that never gets there is said, apart from the
    // analyses
    const t0 = Date.now();
    while (out.whole < 6 && Date.now() - t0 < settleMs)
      await sleep(250);
    out.fullScene = out.whole >= 6;
    out.fullSceneMs = Date.now() - t0;
    await sleep(2000);
    // what the served view has, before the page sets anything
    // ids of their own: the page's other requests (icons) number theirs
    let n = 1;
    const nextId = () => 'matcap-' + (n++);
    out.initial = await page.evaluate(
      (i) => window.__fcOp(i, {op: 'getProperties', subject: 'view3d'}), nextId());
    // each analysis set the way a viewer's property editor sets it: on
    // the served view, whose next publish carries it to the page
    const PHASES = {
      studio: [['Render_Matcap', true], ['Render_MatcapPreset', 'Studio'],
               ['Render_MatcapTint', 0.0]],
      curvature: [['Render_MatcapPreset', 'Curvature'], ['Render_MatcapCurvature', 'Mean'],
                  ['Render_MatcapCurvatureRange', 0.15]],
      draft: [['Render_MatcapPreset', 'Draft'], ['Render_MatcapDraftAngle', 1.0]],
      zebra: [['Render_MatcapPreset', 'Zebra'], ['Render_MatcapStripes', 6]],
    };
    out.replies = {};
    for (const name of Object.keys(PHASES)) {
      out.replies[name] = [];
      for (const [prop, value] of PHASES[name]) {
        out.replies[name].push(await page.evaluate(
          (i, n, v) => window.__fcOp(i, {op: 'setProperty', target: 'view3d', name: n, value: v}),
          nextId(), prop, value));
      }
      await phase(name);
      await sleep(5000);
      out[name] = await count(page, name);
    }
  }
  catch (e) {
    out.error = String((e && e.stack) || e);
    console.log('drive failed: ' + out.error);
  }
  if (process.env.MATCAP_RESULT) {
    try { fs.writeFileSync(process.env.MATCAP_RESULT, JSON.stringify(out)); }
    catch (e) { console.error('result file:', e.message); }
  }
  console.log(JSON.stringify(out));
  await phase('done');
  await browser.close();
})();
