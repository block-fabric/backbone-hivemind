<script lang="ts">
  // The outcomes of a market on its card (pages/Markets.svelte). While it trades: the chances of the likeliest. Once it
  // paid out: what it came to, the outcome that won first, checked, marked Won and filled with what a share of it paid
  // (all of it), the others muted at what they paid, each bar with the faint mark of its last price before it settled,
  // and a line saying whether the market called it. A market on a scale: its scale, the answer of the vote and where the
  // market had put it. Outcome names are text of the chain, shown as data.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { t } from '../../lib/i18n.svelte';
  import OutcomeBar from './OutcomeBar.svelte';
  import ScaleBar from './ScaleBar.svelte';
  import { colour, favourite, formatChance, marketKind, outcomeName, resultOf, scaleEstimate, type Market } from './model';

  let { market, limit = 3 }: { market: Market; limit?: number } = $props();

  const kind = $derived(marketKind(market));
  const res = $derived(resultOf(market));
  const payOf = (index: number) => res?.pays[market.outcomes.findIndex((o) => o.index === index)] ?? 0;
  const rows = $derived(
    [...market.outcomes].sort((a, b) => (res ? payOf(b.index) - payOf(a.index) : 0) || b.price - a.price || a.index - b.index).slice(0, limit),
  );
  const fav = $derived(favourite(market));
  const scale = $derived.by(() => {
    const ty = market.decisions[0]?.type;
    return kind === 'scalar' && ty && ty.min !== undefined && ty.max !== undefined ? { min: ty.min, max: ty.max } : null;
  });
  const answer = $derived(res && scale ? (market.decisions[0]?.answer ?? null) : null);
  const estimate = $derived(scaleEstimate(market));
</script>

{#if res && scale && answer !== null}
  <div class="odds scale">
    <ScaleBar min={scale.min} max={scale.max} value={answer} {estimate} legend={false} />
    <span class="note faint">{t('The market said %1 before it settled', estimate === null ? '–' : estimate.toFixed(2))}</span>
  </div>
{:else if scale && !res}
  <div class="odds scale"><ScaleBar min={scale.min} max={scale.max} {estimate} legend={false} /></div>
{:else}
  <ul class="odds" class:settled={!!res}>
    {#each rows as o (o.index)}
      {@const won = !!res && o.index === res.winner}
      {@const lost = !!res && !won && payOf(o.index) === 0}
      <li class="hm-c{won ? 6 : colour(market, o)}" class:hm-row-lost={lost}>
        <span class="name">
          {#if won}<span class="hm-check" aria-hidden="true"><Icon name="check" size={11} /></span>{:else}<span class="hm-dot hm-c{colour(market, o)}"></span>{/if}
          <span class="cut"><SafeText text={outcomeName(market, o)} /></span>
          {#if won}<span class="hm-result">{t('Won')}</span>{/if}
        </span>
        {#if res}
          <span class="num pct" title={t('A share paid %1 of its sat; %2 before it settled', formatChance(payOf(o.index)), formatChance(o.price))}>{formatChance(payOf(o.index))}</span>
          <OutcomeBar fill={payOf(o.index)} mark={o.price} tone={won ? 'won' : lost ? 'lost' : ''} />
        {:else}
          <span class="num pct">{formatChance(o.price)}</span>
          <OutcomeBar fill={o.price} />
        {/if}
      </li>
    {/each}
  </ul>
  {#if res && res.winner !== null}
    <span class="note faint"><span class="hm-mark-key" aria-hidden="true"></span>{#if fav === res.winner}{t('Last price before it settled: the market called it')}{:else}{t('Last price before it settled: an upset')}{/if}</span>
  {:else if res && res.split}
    <span class="note faint"><span class="hm-mark-key" aria-hidden="true"></span>{t('Split: the decisions resolved between the outcomes')}</span>
  {/if}
{/if}

<style>
  .odds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
  }
  .odds.settled {
    gap: 0.5rem;
  }
  li {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 0.25rem 0.6rem;
    align-items: center;
  }
  .name {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .cut {
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .pct {
    font-weight: 700;
  }
  .hm-row-lost .pct {
    font-weight: 600;
  }
  li > :global(.hm-barwrap) {
    grid-column: 1 / -1;
  }
  .note {
    display: flex;
    align-items: center;
    gap: 0.15rem;
    font-size: 0.8rem;
  }
  .scale .note {
    display: block;
  }
</style>
