// Open two viewer pages, two users of one served document, and drive each
// one's own task panel (docs/TaskPanelPerView.md sec 16): each user right-
// clicks their pad in the view and takes "Edit" from the host's menu, sees
// their own panel and not the other's, types a length into it and presses
// OK or Cancel. The tool bars' card is measured against the panel card.
//
// Usage:  node scripts/panel-two-users-drive.js <alice url> <bob url> <out dir> [timeout_ms]
//   each url wants `?doc=<name>&token=<secret>&client=<name>&panel`, the
//   serve started with an invitation for each name; the document has PadA
//   (length 15) and PadB (length 5) side by side.
//
// Prints one line `REPORT <json>` and writes alice.png and bob.png into
// <out dir>, taken while both are in edit. Exits 0 when every step ran.
//
// PUPPETEER_PATH names a puppeteer-core install, CHROME the browser, and
// CHROME_LIBS is prepended to its LD_LIBRARY_PATH, as scripts/undo-drive.js.
const path = require('path');
const puppeteer = require(process.env.PUPPETEER_PATH || 'puppeteer-core');

(async () => {
  const [aliceUrl, bobUrl, outDir, timeoutArg] = process.argv.slice(2);
  if (!aliceUrl || !bobUrl || !outDir) {
    console.error('usage: node panel-two-users-drive.js <alice url> <bob url> <out dir> [timeout_ms]');
    process.exit(2);
  }
  const timeout = +(timeoutArg || 60000);
  const env = Object.assign({}, process.env);
  if (process.env.CHROME_LIBS)
    env.LD_LIBRARY_PATH = process.env.CHROME_LIBS
      + (env.LD_LIBRARY_PATH ? ':' + env.LD_LIBRARY_PATH : '');
  const W = 900, H = 700;
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    args: ['--no-sandbox', '--window-size=1280,1000',
           '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
    env,
    // two viewers render on one CPU: a page answers slowly
    protocolTimeout: timeout * 3,
  });
  const report = {};
  let code = 2;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const step = (what) => console.log('[step] ' + what + ' ' + new Date().toISOString());
  const open = async (url) => {
    const page = await browser.newPage();
    await page.setViewport({ width: W, height: H });
    page.on('pageerror', (e) => console.log('[pageerror] ' + e.message));
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout });
    await page.waitForSelector('canvas', { timeout });
    await page.waitForSelector('.fc-panel', { timeout });
    return page;
  };
  /// What the page's task panel card shows: its title, its fields' values,
  /// and the line it shows when no panel is open
  const panel = (page) => page.evaluate(() => {
    const card = document.querySelector('.fc-panel');
    if (!card) return null;
    const empty = card.querySelector('.fc-panel-empty');
    return {
      title: (card.querySelector('.fc-panel-title')?.textContent || '').trim(),
      fields: [...card.querySelectorAll('.fc-panel-body input.fc-panel-field')]
        .map((el) => el.value),
      empty: empty ? (empty.textContent || '').trim() : null,
    };
  });
  /// How much of the task panel card the tool bars cover, in px^2
  const overlap = (page) => page.evaluate(() => {
    const rect = (sel) => {
      const el = document.querySelector(sel);
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return r.width > 0 && r.height > 0
        ? [Math.round(r.left), Math.round(r.top), Math.round(r.right), Math.round(r.bottom)]
        : null;
    };
    // the card and every bar in it: a bar can run on past its card
    const t = rect('.fc-tbcard'), p = rect('.fc-panel');
    const all = [t].concat([...document.querySelectorAll('.fc-tbcard .fc-tb-bar')].map((el) => {
      const r = el.getBoundingClientRect();
      return [r.left, r.top, r.right, r.bottom];
    }));
    let area = 0;
    for (const b of all) {
      if (!b || !p) continue;
      const w = Math.max(0, Math.min(b[2], p[2]) - Math.max(b[0], p[0]));
      const h = Math.max(0, Math.min(b[3], p[3]) - Math.max(b[1], p[1]));
      area += Math.round(w * h);
    }
    const right = Math.round(Math.max(...all.filter(Boolean).map((b) => b[2])));
    return { bars: t, barsRight: right, panel: p, area };
  });
  const until = async (page, test, what) => {
    const t0 = Date.now();
    let last = null;
    while (Date.now() - t0 < timeout) {
      last = await panel(page);
      if (last && test(last)) return last;
      await sleep(250);
    }
    throw new Error(what + ': not seen, last ' + JSON.stringify(last));
  };
  const lengthIs = (n) => (p) => p.fields.some((v) => parseFloat(v) === n);
  /// Right-click across the view until the host's menu offers "Edit <label>",
  /// and take it
  const editFromMenu = async (page, label) => {
    await page.bringToFront();
    const seen = [];
    for (const fy of [0.5, 0.45, 0.55, 0.4, 0.6]) {
      for (let fx = 0.1; fx <= 0.9; fx += 0.05) {
        const x = Math.round(fx * W), y = Math.round(fy * H);
        await page.mouse.click(x, y, { button: 'right' });
        const menu = await page.waitForSelector('.fc-ctxmenu .fc-ctx-item',
                                                { timeout: 3000 }).catch(() => null);
        if (!menu) continue;
        const textOf = (item) => item.evaluate((el) =>
          (el.querySelector('.fc-ctx-text')?.textContent || '').trim());
        // the object's own entries are a submenu named after it, as the
        // desktop's 3D-view menu has them
        let items = await page.$$('.fc-ctxmenu .fc-ctx-item');
        let path = [];
        for (const item of items) {
          if ((await textOf(item)) === label) {
            await item.click();
            path.push(label);
            await sleep(500);
            items = await page.$$('.fc-ctxmenu .fc-ctx-item');
            break;
          }
        }
        for (const item of items) {
          const text = await textOf(item);
          if (text.startsWith('Edit') && (path.length || text.includes(label))) {
            await item.click();
            return { x, y, path: path.concat([text]) };
          }
        }
        seen.push(await page.evaluate(() => [...document.querySelectorAll(
          '.fc-ctxmenu .fc-ctx-text')].map((el) => el.textContent.trim()).slice(0, 4)));
        await page.keyboard.press('Escape');
        await page.waitForSelector('.fc-ctxmenu', { hidden: true, timeout: 3000 })
          .catch(() => {});
      }
    }
    throw new Error('no "Edit ' + label + '" in any menu, saw ' + JSON.stringify(seen));
  };
  /// Type a length into the field showing `from`, as a user does: click,
  /// select all, the digits, Tab away
  const type = async (page, from, to, who) => {
    for (let attempt = 0; attempt < 5; ++attempt) {
      try {
        const fields = await page.$$('.fc-panel-body input.fc-panel-field');
        for (const f of fields) {
          if (parseFloat(await f.evaluate((el) => el.value)) !== from) continue;
          await f.click({ clickCount: 3 });
          // Select all from the page: on macOS Chrome runs no editing
          // command for a synthesized key, Control+A or Command+A alike,
          // and the digits went in after the old value ("15.0018")
          await f.evaluate((el) => el.select());
          await page.keyboard.type(String(to));
          await page.keyboard.press('Tab');
          return true;
        }
        return false;
      } catch (e) {
        report[who + 'Retries'] = (report[who + 'Retries'] || 0) + 1;
        console.log('[type] ' + who + ' ' + e.message);
        await sleep(500);
      }
    }
    return false;
  };
  /// Whether the field showing `value` is the same element after `ms`:
  /// a page that re-creates it takes a user's focus and typing with it
  const kept = (page, value, ms) => page.evaluate(async (v, wait) => {
    const find = () => [...document.querySelectorAll('.fc-panel-body input.fc-panel-field')]
      .find((el) => parseFloat(el.value) === v);
    const before = find();
    if (!before) return null;
    before.dataset.fcMark = 'kept';
    await new Promise((r) => setTimeout(r, wait));
    const after = find();
    return !!after && after === before && after.dataset.fcMark === 'kept';
  }, value, ms);
  const button = async (page, text) => {
    const buttons = await page.$$('.fc-panel-body .fc-panel-buttons button');
    for (const b of buttons) {
      if ((await b.evaluate((el) => (el.textContent || '').trim())) === text) {
        await b.click();
        return true;
      }
    }
    return false;
  };
  try {
    const alice = await open(aliceUrl);
    const bob = await open(bobUrl);
    step('pages open');
    // the scene in, and the pages told what they may do
    await sleep(+(process.env.FC_PANEL_SETTLE || 8000));
    report.aliceBefore = await panel(alice);
    report.bobBefore = await panel(bob);
    // measured in front: a page behind another runs no rendering steps,
    // where the card is told of the panel (a ResizeObserver)
    await alice.bringToFront();
    await sleep(1000);
    report.overlapOpen = await overlap(alice);
    // every button its icon, as on the desktop; one drawn as its text
    // is one whose icon the host did not send
    report.faces = await alice.evaluate(() => {
      const out = {buttons: 0, icons: 0, text: []};
      for (const b of document.querySelectorAll('.fc-tbcard .fc-tb-btn')) {
        out.buttons++;
        const img = b.querySelector('img.fc-tb-icon');
        if (img && img.complete && img.naturalWidth > 0) out.icons++;
        else out.text.push(b.dataset.command || b.textContent.trim());
      }
      return out;
    });

    report.aliceMenu = await editFromMenu(alice, 'PadA');
    report.alice = await until(alice, lengthIs(15), "alice's panel with PadA's 15");
    step('alice in edit');
    await sleep(2000);
    report.bobWhileAlice = await panel(bob);

    report.bobMenu = await editFromMenu(bob, 'PadB');
    report.bob = await until(bob, lengthIs(5), "bob's panel with PadB's 5");
    step('bob in edit');
    await sleep(2000);
    report.aliceWhileBob = await panel(alice);
    await alice.bringToFront();
    await sleep(500);
    report.overlapAlice = await overlap(alice);
    await bob.bringToFront();
    await sleep(500);
    report.overlapBob = await overlap(bob);
    await alice.screenshot({ path: path.join(outDir, 'alice.png') });
    await bob.screenshot({ path: path.join(outDir, 'bob.png') });

    await alice.bringToFront();
    report.aliceKept = await kept(alice, 15, 3000);
    report.aliceTyped = await type(alice, 15, 18, 'alice');
    report.aliceAfterTyping = await until(alice, lengthIs(18), "alice's 18 in her field");
    await sleep(2000);
    report.bobAfterAliceTyped = await panel(bob);
    // bob's field, marked before her OK: still the same element after it
    await bob.evaluate(() => {
      const f = [...document.querySelectorAll('.fc-panel-body input.fc-panel-field')]
        .find((el) => parseFloat(el.value) === 5);
      if (f) f.dataset.fcMark = 'before-ok';
      // every field element taken out of the page from here on
      window.fcFieldsRemoved = [];
      new MutationObserver((records) => {
        for (const r of records)
          for (const n of r.removedNodes)
            if (n.nodeType === 1 && (n.matches('input.fc-panel-field')
                                     || n.querySelector('input.fc-panel-field')))
              window.fcFieldsRemoved.push(new Date().toISOString() + ' '
                + (n.matches('input') ? n : n.querySelector('input.fc-panel-field')).value
                + ' in ' + (n.className || n.tagName));
      }).observe(document.body, { childList: true, subtree: true });
    });
    report.aliceOk = await button(alice, 'OK');
    report.aliceAfterOk = await until(alice, (p) => p.fields.length === 0,
                                      "alice's panel gone after her OK");
    step('alice OK');
    await sleep(2000);
    report.bobAfterAliceOk = await panel(bob);
    report.bobFieldKeptOverAliceOk = await bob.evaluate(() =>
      [...document.querySelectorAll('.fc-panel-body input.fc-panel-field')]
        .some((el) => el.dataset.fcMark === 'before-ok'));

    await bob.bringToFront();
    report.bobKept = await kept(bob, 5, 3000);
    report.bobFieldsRemoved = await bob.evaluate(() => window.fcFieldsRemoved || []);
    report.bobTyped = await type(bob, 5, 9, 'bob');
    report.bobAfterTyping = await until(bob, lengthIs(9), "bob's 9 in his field");
    await sleep(2000);
    report.bobCancel = await button(bob, 'Cancel');
    report.bobAfterCancel = await until(bob, (p) => p.fields.length === 0,
                                        "bob's panel gone after his Cancel");
    step('bob Cancel');

    // her panel card dragged by its title bar over where the bars start:
    // the bars go beside it
    await alice.bringToFront();
    const head = await alice.$('.fc-panel-head');
    const hb = await head.boundingBox();
    await alice.mouse.move(hb.x + 60, hb.y + hb.height / 2);
    await alice.mouse.down();
    await alice.mouse.move(200, 120, { steps: 12 });
    await alice.mouse.move(70, 20, { steps: 12 });
    await alice.mouse.up();
    await sleep(1000);
    report.overlapDragged = await overlap(alice);
    code = 0;
  } catch (e) {
    console.log('[drive] ' + e.message);
  } finally {
    console.log('REPORT ' + JSON.stringify(report));
    await browser.close();
  }
  process.exit(code);
})();
