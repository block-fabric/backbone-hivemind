// Svelte actions of the Hivemind pages.

const clamp = (f: number) => Math.max(0, Math.min(1, Number.isFinite(f) ? f : 0));

/** The width of a bar as a fraction of its track, set through the CSS object model (no inline style: the content security policy). */
export function barWidth(node: HTMLElement, fraction: number) {
  const set = (f: number) => node.style.setProperty('width', `${clamp(f) * 100}%`);
  set(fraction);
  return { update: set };
}

/** Where a mark stands along its track, as a fraction of it (the same way: no inline style). */
export function barLeft(node: HTMLElement, fraction: number) {
  const set = (f: number) => node.style.setProperty('left', `${clamp(f) * 100}%`);
  set(fraction);
  return { update: set };
}

/**
 * A label over a point of a track that stays inside it: its left at the point, moved back by as much of its own width
 * as the point is along the track (at the start its left edge is there, at the end its right edge, in the middle its
 * middle).
 */
export function labelAt(node: HTMLElement, fraction: number) {
  const set = (f: number) => {
    const p = clamp(f) * 100;
    node.style.setProperty('left', `${p}%`);
    node.style.setProperty('transform', `translateX(-${p}%)`);
  };
  set(fraction);
  return { update: set };
}
