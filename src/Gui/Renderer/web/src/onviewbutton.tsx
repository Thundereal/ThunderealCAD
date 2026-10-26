// The menu button of an edit's on-view values (entry 173,
// docs/OnViewValueEditing.md sec 9.2 #9): on the desktop a widget at the
// left of the top-middle label; here drawn by the page where the host says
// it stands (the `view.covered` reply, covered.ts), and opening the edit's
// menu -- the same as a right click on a handle, without the handle's
// entries -- through the scene menu (contextmenu.tsx, 'fc:onviewmenu').
import { Show, createSignal } from 'solid-js';

/// {x, y, side} in CSS pixels from the canvas' top left, or null for none
const [place, setPlace] = createSignal<number[] | null>(null);
export const setOnViewMenuButton = setPlace;

export function OnViewMenuButton() {
  const open = () => {
    const p = place();
    if (!p) return;
    const canvas = document.querySelector('canvas');
    const c = canvas ? canvas.getBoundingClientRect() : { left: 0, top: 0 };
    window.dispatchEvent(new CustomEvent('fc:onviewmenu', {
      detail: { x: c.left + p[0], y: c.top + p[1] + p[2] + 2 },
    }));
  };
  const style = () => {
    const p = place()!;
    const canvas = document.querySelector('canvas');
    const c = canvas ? canvas.getBoundingClientRect() : { left: 0, top: 0 };
    return {
      left: `${c.left + p[0]}px`,
      top: `${c.top + p[1]}px`,
      width: `${p[2]}px`,
      height: `${p[2]}px`,
    };
  };
  return (
    <Show when={place()}>
      <button
        class="fc-onview-menubtn"
        style={style()}
        title="Menu of the edit's values"
        aria-label="Menu of the edit's values"
        onPointerDown={(e) => e.stopPropagation()}
        onClick={(e) => { e.stopPropagation(); open(); }}
      >
        <span /><span /><span />
      </button>
    </Show>
  );
}
