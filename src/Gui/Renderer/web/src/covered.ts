// What this page's own interface covers of the 3D view -- its tool bars --
// told to the host, so that what the host places on the view goes clear of
// it: an edit's on-view labels at the top middle
// (docs/OnViewValueEditing.md sec 9.5a, the `view.covered` op). Sent while
// this client edits, on entering the edit and whenever the bars or the
// window change size.
import { sendOp } from './control';

let timer: number | undefined;
let last = '';

function measure(): number[][] {
  const canvas = document.querySelector('canvas');
  if (!canvas) return [];
  const c = canvas.getBoundingClientRect();
  const out: number[][] = [];
  for (const bar of Array.from(document.querySelectorAll('.fc-tb-bar'))) {
    const r = bar.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) continue;
    out.push([Math.round(r.left - c.left), Math.round(r.top - c.top),
              Math.round(r.width), Math.round(r.height)]);
  }
  return out;
}

function send(force = false) {
  if (!window.fcviewerEditing) return;
  const rects = measure();
  const key = JSON.stringify(rects);
  if (!force && key === last) return;
  last = key;
  sendOp('view.covered', { rects }).catch(() => { last = ''; });
}

function later(force = false) {
  if (timer !== undefined) window.clearTimeout(timer);
  timer = window.setTimeout(() => { timer = undefined; send(force); }, 200);
}

export function installCovered() {
  window.addEventListener('fc:edit', () => later(true));
  window.addEventListener('resize', () => later());
  const observer = new ResizeObserver(() => later());
  const watch = () => {
    const bars = document.querySelector('.fc-toolbars');
    if (bars) observer.observe(bars);
    else window.setTimeout(watch, 1000);
  };
  watch();
}
