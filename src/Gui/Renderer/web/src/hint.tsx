// A handle's hint (docs/OnViewValueEditing.md sec 9.2 #6): on the desktop a
// Qt tool tip at the pointer, after the style's wait, over an edit's handle
// ("right click for the menu"). The host keeps the wait and says which hint
// shows, if any (the `hint` push, Gizmo::setHovered); the page puts it at
// its own pointer, which is where the hover the host judged came from.
import { Show, createSignal, onCleanup } from 'solid-js';
import { onPush } from './control';

/// Below and right of the pointer, as a tool tip sits clear of the cursor
const OFFSET_X = 14;
const OFFSET_Y = 20;

export function HandleHint() {
  const [text, setText] = createSignal('');
  const [pos, setPos] = createSignal({ x: 0, y: 0 });
  let el: HTMLDivElement | undefined;

  const off = onPush('hint', (msg) => {
    setText(typeof msg.text === 'string' ? msg.text : '');
  });
  const move = (e: PointerEvent) => setPos({ x: e.clientX, y: e.clientY });
  // A press hides it, as a desktop tool tip goes at a click; the host's
  // next hint brings it back
  const hide = () => setText('');
  window.addEventListener('pointermove', move, true);
  window.addEventListener('pointerdown', hide, true);
  window.addEventListener('contextmenu', hide, true);
  document.documentElement.addEventListener('pointerleave', hide);
  onCleanup(() => {
    off();
    window.removeEventListener('pointermove', move, true);
    window.removeEventListener('pointerdown', hide, true);
    window.removeEventListener('contextmenu', hide, true);
    document.documentElement.removeEventListener('pointerleave', hide);
  });

  // Kept inside the window: flipped to the pointer's other side at an edge
  const style = () => {
    const p = pos();
    const w = el ? el.offsetWidth : 0;
    const h = el ? el.offsetHeight : 0;
    let x = p.x + OFFSET_X;
    let y = p.y + OFFSET_Y;
    if (x + w > window.innerWidth - 4) x = Math.max(4, p.x - OFFSET_X - w);
    if (y + h > window.innerHeight - 4) y = Math.max(4, p.y - OFFSET_Y / 2 - h);
    return { left: `${x}px`, top: `${y}px` };
  };

  return (
    <Show when={text()}>
      <div ref={el} class="fc-hint" role="tooltip" style={style()}>{text()}</div>
    </Show>
  );
}
