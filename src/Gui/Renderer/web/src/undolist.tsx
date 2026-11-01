// The undo list (docs/MultiViewEdit.md sec 12.11): what this user's undo and
// redo would take, a corner button beside the viewer's menus. The steps of
// an edit ended with OK are one row, as one undo takes them; the row opens
// to the edit's steps, to undo it down to one of them. An undo refused for
// steps made since that depend on it says them, and offers the undo with
// them -- asked for when one is another user's -- or with what they changed
// kept. And the other half: this user asked to let steps of theirs be
// undone with another's, and told how what they asked went.
import { For, Show, createSignal, onCleanup } from 'solid-js';
import { sendOp } from './control';

interface Step {
  id: number;
  name: string;
  group?: number;
  author?: string;
}

/// One row: a step, or a group's steps in the order listed
interface Row {
  steps: Step[];
}

/// The rows of a list: a step each, a group's steps in one, at the place
/// of its first
export function rowsOf(steps: Step[]): Row[] {
  const rows: Row[] = [];
  const byGroup = new Map<number, Row>();
  for (const s of steps) {
    if (s.group) {
      const row = byGroup.get(s.group);
      if (row) {
        row.steps.push(s);
        continue;
      }
    }
    const row = { steps: [s] };
    if (s.group) byGroup.set(s.group, row);
    rows.push(row);
  }
  return rows;
}

interface Refusal {
  code: string;
  message?: string;
  what?: string;
  steps?: Step[];
  keepable?: boolean;
}

export function UndoLauncher(props: { hidden?: () => boolean; viewOnly: () => boolean }) {
  const [open, setOpen] = createSignal(false);
  const [undos, setUndos] = createSignal<Step[]>([]);
  const [redos, setRedos] = createSignal<Step[]>([]);
  const [expanded, setExpanded] = createSignal<string | null>(null);
  const [inTheWay, setInTheWay] = createSignal<Refusal | null>(null);
  const [note, setNote] = createSignal('');
  let root!: HTMLDivElement;

  const stacks = (d: any) => {
    setUndos(d?.undoSteps ?? []);
    setRedos(d?.redoSteps ?? []);
  };
  const refresh = () => sendOp('undo.list').then(stacks).catch(() => {});
  const take = (op: 'undo' | 'redo', fields: Record<string, unknown>) => {
    setNote('');
    return sendOp(op, fields)
      .then((d) => {
        stacks(d);
        setInTheWay(null);
        setExpanded(null);
      })
      .catch((e: Refusal) => {
        if (e.code === 'InTheWay') setInTheWay(e);
        else if (e.code === 'Asked') {
          setInTheWay(null);
          setNote('Asked. The undo goes once they agree.');
        }
        else setNote(e.message || e.code);
        refresh();
      });
  };

  const onDown = (e: PointerEvent) => {
    if (!root.contains(e.target as Node)) setOpen(false);
  };
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape') setOpen(false);
  };
  // the list follows what another's answer did to it
  const onControl = (e: Event) => {
    const d = (e as CustomEvent).detail;
    if (d && d.op === 'consent.result' && open()) refresh();
  };
  document.addEventListener('pointerdown', onDown, true);
  window.addEventListener('keydown', onKey);
  window.addEventListener('fc:control', onControl);
  onCleanup(() => {
    document.removeEventListener('pointerdown', onDown, true);
    window.removeEventListener('keydown', onKey);
    window.removeEventListener('fc:control', onControl);
  });

  const toggle = () => {
    const now = !open();
    setOpen(now);
    setInTheWay(null);
    setNote('');
    if (now) refresh();
  };

  // A row taken: down to its last step -- a group's whole edit
  const takeRow = (op: 'undo' | 'redo', row: Row) =>
    take(op, { to: row.steps[row.steps.length - 1].id });

  const list = (op: 'undo' | 'redo', steps: () => Step[]) => (
    <For each={rowsOf(steps())}>
      {(row, i) => {
        const key = `${op}${i()}`;
        const group = row.steps.length > 1;
        return (
          <div class="fc-undo-row">
            <div class="fc-undo-line">
              <Show when={group} fallback={<span class="fc-undo-exp" />}>
                <button class="fc-undo-exp" aria-label="Its steps"
                        aria-expanded={expanded() === key}
                        onClick={() => setExpanded(expanded() === key ? null : key)}>
                  <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true">
                    <path d={expanded() === key ? 'M1 3l4 4 4-4' : 'M3 1l4 4-4 4'}
                          fill="none" stroke="currentColor" stroke-width="1.6" />
                  </svg>
                </button>
              </Show>
              <button class="fc-menu-item fc-undo-take" role="menuitem"
                      disabled={props.viewOnly()}
                      onClick={() => takeRow(op, row)}>
                {group
                  ? `${row.steps[op === 'undo' ? row.steps.length - 1 : 0].name}`
                    + ` (${row.steps.length} steps)`
                  : row.steps[0].name}
              </button>
            </div>
            <Show when={group && expanded() === key}>
              <For each={row.steps}>
                {(s) => (
                  <button class="fc-menu-item fc-undo-step" role="menuitem"
                          disabled={props.viewOnly()}
                          onClick={() => take(op, { to: s.id })}>
                    {s.name}
                  </button>
                )}
              </For>
            </Show>
          </div>
        );
      }}
    </For>
  );

  const others = () => [...new Set((inTheWay()?.steps ?? [])
    .map((s) => s.author).filter((a): a is string => !!a))];

  return (
    <Show when={!props.hidden?.()}>
      <div class="fc-launcher fc-launcher-undo" ref={root!}>
        <Show when={open()}>
          <div class="fc-menu fc-undo-panel" role="menu" aria-label="Undo">
            <Show when={inTheWay()}>
              <div class="fc-undo-card" role="alertdialog">
                <div class="fc-undo-says">
                  '{inTheWay()!.what}' cannot be undone on its own: steps made
                  since depend on it.
                </div>
                <For each={inTheWay()!.steps ?? []}>
                  {(s) => (
                    <div class="fc-undo-dep">
                      {s.name}{s.author ? `, by ${s.author}` : ''}
                    </div>
                  )}
                </For>
                <div class="fc-undo-acts">
                  <button class="fc-undo-act fc-undo-with"
                          onClick={() => take('undo', {
                            with: (inTheWay()!.steps ?? []).map((s) => s.id) })}>
                    {others().length ? `Ask ${others().join(', ')}` : 'Undo with them'}
                  </button>
                  <Show when={inTheWay()!.keepable}>
                    <button class="fc-undo-act fc-undo-keep"
                            onClick={() => take('undo', { keep: true })}>
                      Keep theirs
                    </button>
                  </Show>
                  <button class="fc-undo-act" onClick={() => setInTheWay(null)}>
                    Cancel
                  </button>
                </div>
              </div>
            </Show>
            <Show when={note()}>
              <div class="fc-undo-note" role="status">{note()}</div>
            </Show>
            <div class="fc-menu-header">Undo</div>
            <Show when={undos().length}
                  fallback={<div class="fc-undo-none">Nothing to undo</div>}>
              {list('undo', undos)}
            </Show>
            <div class="fc-menu-header">Redo</div>
            <Show when={redos().length}
                  fallback={<div class="fc-undo-none">Nothing to redo</div>}>
              {list('redo', redos)}
            </Show>
          </div>
        </Show>
        <button class="fc-launch" aria-haspopup="menu" aria-expanded={open()}
                onClick={toggle} aria-label="Undo" title="Undo">
          <svg width="17" height="17" viewBox="0 0 24 24" fill="none"
               stroke="currentColor" stroke-width="2" stroke-linecap="round"
               stroke-linejoin="round" aria-hidden="true">
            <path d="M9 14L4 9l5-5" />
            <path d="M4 9h10.5a5.5 5.5 0 0 1 0 11H11" />
          </svg>
        </button>
      </div>
    </Show>
  );
}

interface Question {
  request: number;
  from: string;
  what: string;
  steps: string[];
}

/// This user asked to let steps of theirs be undone with another's, and
/// told how what they asked went (sec 12.11)
export function ConsentCards() {
  const [questions, setQuestions] = createSignal<Question[]>([]);
  const [said, setSaid] = createSignal('');
  let sayTimer = 0;

  const say = (text: string) => {
    setSaid(text);
    clearTimeout(sayTimer);
    sayTimer = window.setTimeout(() => setSaid(''), 8000);
  };
  const onControl = (e: Event) => {
    const d = (e as CustomEvent).detail;
    if (!d) return;
    if (d.op === 'consent.ask')
      setQuestions([...questions(), d as Question]);
    else if (d.op === 'consent.result') {
      if (!d.allowed) say(`${d.by} did not agree: '${d.what}' is not undone.`);
      else if (d.done) say(`${d.by} agreed: '${d.what}' is undone.`);
      else say(`${d.by} agreed, but '${d.what}' did not go: ${d.why ?? ''}`);
    }
  };
  window.addEventListener('fc:control', onControl);
  onCleanup(() => {
    window.removeEventListener('fc:control', onControl);
    clearTimeout(sayTimer);
  });

  const answer = (q: Question, allow: boolean) => {
    setQuestions(questions().filter((x) => x.request !== q.request));
    sendOp('consent.answer', { request: q.request, allow })
      .catch((e: { message?: string; code: string }) => say(e.message || e.code));
  };

  return (
    <>
      <For each={questions()}>
        {(q) => (
          <div class="fc-consent" role="alertdialog" aria-label="Undo asked">
            <div class="fc-undo-says">
              {q.from} asks to undo '{q.what}', which needs these steps of yours
              undone with it:
            </div>
            <For each={q.steps}>{(s) => <div class="fc-undo-dep">{s}</div>}</For>
            <div class="fc-undo-acts">
              <button class="fc-undo-act fc-consent-allow" onClick={() => answer(q, true)}>
                Allow
              </button>
              <button class="fc-undo-act fc-consent-deny" onClick={() => answer(q, false)}>
                Deny
              </button>
            </div>
          </div>
        )}
      </For>
      <Show when={said()}>
        <div class="fc-consent fc-consent-said" role="status">{said()}</div>
      </Show>
    </>
  );
}
