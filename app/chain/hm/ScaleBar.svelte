<script lang="ts">
  // A market on a scale, as its scale: from its low end to its high end, the answer the vote gave (a filled mark, its
  // number above it) and where the market put the answer (a hollow mark: its estimate from the last price of High before
  // it settled, or now while it trades). Before the vote, the estimate alone, filled.
  import { t } from '../../lib/i18n.svelte';
  import { barLeft, barWidth, labelAt } from './actions';
  import { scalePlace } from './model';

  let {
    min,
    max,
    value = null,
    estimate = null,
    legend = true,
  }: {
    min: number;
    max: number;
    /** The answer of the vote, once resolved. */
    value?: number | null;
    /** The market's estimate. */
    estimate?: number | null;
    legend?: boolean;
  } = $props();

  const fmt = (x: number) => (Number.isInteger(x) ? String(x) : x.toFixed(2));
  const shown = $derived(value ?? estimate);
  const words = $derived(
    value !== null && estimate !== null
      ? t('On a scale from %1 to %2: the vote said %3, the market %4', fmt(min), fmt(max), fmt(value), fmt(estimate))
      : value !== null
        ? t('On a scale from %1 to %2: the vote said %3', fmt(min), fmt(max), fmt(value))
        : estimate !== null
          ? t('On a scale from %1 to %2: the market says %3', fmt(min), fmt(max), fmt(estimate))
          : t('On a scale from %1 to %2', fmt(min), fmt(max)),
  );
</script>

<div class="hm-scale" class:decided={value !== null} role="img" aria-label={words} title={words}>
  <div class="above">{#if shown !== null}<strong class="num at" use:labelAt={scalePlace(shown, min, max)}>{fmt(shown)}</strong>{/if}</div>
  <div class="track">
    {#if shown !== null}<span class="fill" use:barWidth={scalePlace(shown, min, max)}></span>{/if}
    {#if value !== null && estimate !== null}<span class="dot est" use:barLeft={scalePlace(estimate, min, max)}></span>{/if}
    {#if shown !== null}<span class="dot val" use:barLeft={scalePlace(shown, min, max)}></span>{/if}
  </div>
  <div class="ends faint num"><span>{fmt(min)}</span><span>{fmt(max)}</span></div>
  {#if legend && value !== null}
    <div class="legend small">
      <span class="key"><span class="dot val"></span>{t('Resolved: %1', fmt(value))}</span>
      {#if estimate !== null}<span class="key muted"><span class="dot est"></span>{t('The market said %1 before it settled', fmt(estimate))}</span>{/if}
    </div>
  {/if}
</div>

<style>
  .hm-scale {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    min-width: 0;
  }
  .above {
    position: relative;
    height: 1.25rem;
  }
  .at {
    position: absolute;
    bottom: 0;
    font-size: 0.95rem;
    white-space: nowrap;
  }
  .decided .at {
    color: var(--positive);
  }
  .track {
    position: relative;
    height: 0.42rem;
    margin: 0.2rem 0;
    border-radius: 999px;
    background: var(--raised);
  }
  .fill {
    position: absolute;
    inset: 0 auto 0 0;
    border-radius: 999px;
    background: var(--accent);
    opacity: 0.45;
  }
  .decided .fill {
    background: var(--positive);
  }
  .dot {
    display: inline-block;
    width: 0.9rem;
    height: 0.9rem;
    border-radius: 50%;
    box-sizing: border-box;
  }
  .track .dot {
    position: absolute;
    top: 50%;
    margin: -0.45rem 0 0 -0.45rem;
  }
  .dot.val {
    background: var(--accent);
    border: 2px solid var(--surface);
    box-shadow: 0 0 0 1px var(--accent);
  }
  .decided .dot.val {
    background: var(--positive);
    box-shadow: 0 0 0 1px var(--positive);
  }
  .dot.est {
    width: 0.75rem;
    height: 0.75rem;
    background: var(--surface);
    border: 2px solid var(--text-faint);
  }
  .track .dot.est {
    margin: -0.375rem 0 0 -0.375rem;
  }
  .ends {
    display: flex;
    justify-content: space-between;
    font-size: 0.78rem;
  }
  .legend {
    display: flex;
    flex-wrap: wrap;
    gap: 0.2rem 1rem;
    margin-top: 0.15rem;
  }
  .key {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
  }
  .key .dot {
    width: 0.7rem;
    height: 0.7rem;
  }
  .key .dot.est {
    width: 0.62rem;
    height: 0.62rem;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
