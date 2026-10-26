<script lang="ts">
  // One market: what it is about, the chance of each of its outcomes now and over time (the chain's prices, block by
  // block), the decisions it waits for, the wallet's positions, who holds its shares, and the panel to trade in it.
  // Once it paid out: what it came to (the outcome that won checked, what a share of each paid, the last price of each
  // before it settled: whether the market called it), when it paid, and what it paid the wallet. Titles, descriptions,
  // decisions and outcomes are text of the chain, shown as data.
  import { untrack } from 'svelte';
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import { hmApi, hmSpend } from './api';
  import Countdown from './Countdown.svelte';
  import OutcomeBar from './OutcomeBar.svelte';
  import PriceChart from './PriceChart.svelte';
  import ScaleBar from './ScaleBar.svelte';
  import TradePanel from './TradePanel.svelte';
  import {
    categoryOf,
    colour,
    favourite,
    formatChance,
    formatRate,
    formatShares,
    marketKind,
    outcomeName,
    periodEnd,
    resolvedText,
    resultOf,
    scaleEstimate,
    stageText,
    marketStage,
    titleWithoutCategory,
    type Market,
  } from './model';
  import { decisionMap, heldByOutcome, hm, tradeIntent } from './store.svelte';

  let { id, back }: { id: string; back: () => void } = $props();

  let full = $state<Market | null>(null);
  let error = $state('');
  $effect(() => {
    void hm.version;
    const want = id;
    hmApi
      .market(want)
      .then((m) => {
        if (want === id) {
          full = m;
          error = '';
        }
      })
      .catch((e) => (error = errorText(e)));
  });

  const market = $derived(full ?? hm.markets.find((m) => m.id === id) ?? null);
  const decisions = $derived(decisionMap());
  const kind = $derived(market ? marketKind(market) : 'binary');
  const stage = $derived(market ? marketStage(market) : 'trading');
  const res = $derived(market ? resultOf(market) : null);
  const payOf = (index: number) => (market && res ? (res.pays[market.outcomes.findIndex((o) => o.index === index)] ?? 0) : 0);
  const winner = $derived(market && res && res.winner !== null ? (market.outcomes.find((o) => o.index === res.winner) ?? null) : null);
  const fav = $derived(market ? favourite(market) : null);
  const favOutcome = $derived(market && fav !== null ? (market.outcomes.find((o) => o.index === fav) ?? null) : null);
  const scale = $derived.by(() => {
    const ty = market?.decisions[0]?.type;
    return kind === 'scalar' && ty && ty.min !== undefined && ty.max !== undefined ? { min: ty.min, max: ty.max } : null;
  });
  const estimate = $derived(market ? scaleEstimate(market) : null);
  const answer = $derived(res && scale ? (market?.decisions[0]?.answer ?? null) : null);
  const held = $derived(heldByOutcome(hm.positions.filter((p) => p.market === id)));
  const payout = $derived(hm.payouts.find((p) => p.market === id) ?? null);
  const category = $derived(market ? categoryOf(market) : null);
  /** The last voting period of its decisions: the market settles in the block that resolves the last of them. */
  const lastVote = $derived(market && market.decisions.length ? Math.max(...market.decisions.map((d) => d.votingPeriod)) : null);
  const shown = $derived(market ? [...market.outcomes].sort((a, b) => (res ? payOf(b.index) - payOf(a.index) : 0) || b.price - a.price || a.index - b.index) : []);
  const colours = $derived(market ? market.outcomes.map((o) => colour(market, o)) : []);
  const history = $derived(market?.history ?? []);

  const intent = untrack(() => (tradeIntent.market === id ? { outcome: tradeIntent.outcome, side: tradeIntent.side } : null));
  if (intent) tradeIntent.market = '';
  let chosen = $state(intent?.outcome ?? -1);
  let side = $state<'buy' | 'sell'>(intent?.side ?? 'buy');
  $effect(() => {
    if (chosen < 0 && shown.length) chosen = shown[0].index;
  });
  const chosenAt = $derived(market ? market.outcomes.findIndex((o) => o.index === chosen) : -1);

  // The author adds liquidity (market_amplify_beta).
  let liquidityText = $state('');
  let liqBusy = $state(false);
  async function amplify() {
    if (!market) return;
    const sats = Math.round(Number(liquidityText.replace(',', '.')) * 1e8);
    if (!(sats > 0)) return;
    liqBusy = true;
    try {
      await hmSpend.amplify(market.id, sats);
      toast('success', t('Liquidity added'), t('With the next block of Hivemind.'));
      liquidityText = '';
    } catch (e) {
      if (!declined(e)) toast('error', t('Add liquidity'), errorText(e));
    } finally {
      liqBusy = false;
    }
  }
</script>

{#if !market}
  <div class="page">
    <button type="button" class="btn ghost" onclick={back}><Icon name="receive" size={15} /> {t('All markets')}</button>
    <p class="hm-empty">{error ? error : t('Asking the node…')}</p>
  </div>
{:else}
  <div class="page market-view">
    <nav class="crumbs">
      <button type="button" class="btn ghost small" onclick={back}><Icon name="receive" size={15} /> {t('All markets')}</button>
      {#if category}<span class="badge"><SafeText text={category} /></span>{/if}
      <span class="badge">{kind === 'binary' ? t('Yes or no') : kind === 'scalar' ? t('Scale') : kind === 'categorical' ? t('Options') : t('Several decisions')}</span>
      {#if market.mine}<span class="badge accent">{t('yours')}</span>{/if}
    </nav>

    <header class="head">
      <div class="title">
        <h1 title={titleWithoutCategory(market.title)}><SafeText text={titleWithoutCategory(market.title)} /></h1>
        {#if market.description}<p class="muted desc"><SafeText text={market.description} /></p>{/if}
        <p class="sub">
          <span class="hm-stage" class:live={stage === 'trading'} class:vote={stage === 'voting' || stage === 'partly'} class:done={stage === 'paid'} class:bad={stage === 'cancelled' || stage === 'invalid'}>{stageText(stage)}</span>
          {#if res}
            {#if answer !== null}
              <span class="hm-result"><span class="hm-check" aria-hidden="true"><Icon name="check" size={10} /></span>{t('Resolved: %1', String(answer))}</span>
            {:else if winner}
              <span class="hm-result"><span class="hm-check" aria-hidden="true"><Icon name="check" size={10} /></span>{t('Resolved')}: <SafeText text={outcomeName(market, winner)} /></span>
            {:else if res.split}
              <span class="hm-result tie">{t('Resolved: a split')}</span>
            {/if}
            {#if market.settledHeight !== undefined}<span class="muted num">{t('Paid out at block %1', market.settledHeight)}</span>{/if}
          {:else if hm.status && lastVote !== null}
            <span class="muted">{t('Trades until it settles: its decisions are voted on in period %1, which', lastVote)} <Countdown clock={hm.status.period} target={periodEnd(hm.status.period, lastVote)} phrase="ends" /></span>
          {/if}
        </p>
      </div>
      <div class="id"><span class="mono faint" translate="no">{market.id}</span><CopyButton text={market.id} label={t('Copy the id of the market')} /></div>
    </header>

    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">{t('Traded')}</span><strong><Amount sats={market.volume} /></strong></div>
      <div class="panel hm-figure"><span class="muted">{t('Liquidity')}</span><strong><Amount sats={market.liquidity} /></strong></div>
      <div class="panel hm-figure"><span class="muted">{t('The market holds')}</span><strong><Amount sats={market.treasury} /></strong></div>
      <div class="panel hm-figure"><span class="muted">{t('Fee · fees taken')}</span><strong class="num">{formatRate(market.feeRate)} · <Amount sats={market.fees} showUnit={false} /></strong></div>
    </section>

    <div class="cols" class:solo={market.state !== 'trading'}>
      <div class="main">
        <section class="panel" data-market-stage={stage}>
          <div class="panel-head">
            <h2>{res ? t('Result') : t('Chances')}</h2>
            {#if !res && estimate !== null && scale}<span class="muted">{t('The market puts it at')} <strong class="num">{estimate.toFixed(2)}</strong> {t('on the scale from %1 to %2', scale.min, scale.max)}</span>{/if}
          </div>
          <div class="panel-body">
            {#if res}
              <div class="verdict">
                {#if answer !== null && scale}
                  <ScaleBar min={scale.min} max={scale.max} value={answer} {estimate} />
                {:else}
                  <p class="answer">
                    {#if winner}<span class="hm-check big" aria-hidden="true"><Icon name="check" size={15} /></span><strong><SafeText text={outcomeName(market, winner)} /></strong>{:else}<strong>{t('A split')}</strong>{/if}
                  </p>
                  <p class="muted">
                    {#if winner && fav === winner.index}
                      {t('The market called it: it was at %1 before it settled.', formatChance(winner.price))}
                    {:else if winner && favOutcome}
                      {t('An upset: the market had another outcome at %1 before it settled; this one was at %2.', formatChance(favOutcome.price), formatChance(winner.price))}
                    {:else if !winner}
                      {t('The decisions resolved between the outcomes: each share paid its part.')}
                    {/if}
                  </p>
                {/if}
              </div>
            {:else if scale}
              <div class="verdict"><ScaleBar min={scale.min} max={scale.max} {estimate} /></div>
            {/if}
            <PriceChart points={history} now={market.outcomes.map((o) => o.price)} height={res ? (market.settledHeight ?? market.updatedHeight) : (hm.status?.period.height ?? market.updatedHeight)} {colours} chosen={chosenAt} label={t('The chance of each outcome over time')} />
            <p class="hm-help">{res ? t('The chain’s prices, block by block, until it settled.') : t('The chain’s prices, block by block, from its creation (even odds).')}</p>
            <ul class="outcomes">
              {#each shown as o (o.index)}
                {@const won = !!res && o.index === res.winner}
                {@const lost = !!res && !won && payOf(o.index) === 0}
                <li>
                  <button type="button" class="outcome hm-c{won ? 6 : colour(market, o)}" class:chosen={chosen === o.index && !res} class:won class:hm-row-lost={lost} onclick={() => (chosen = o.index)}>
                    {#if won}<span class="hm-check" aria-hidden="true"><Icon name="check" size={11} /></span>{:else}<span class="hm-dot hm-c{colour(market, o)}"></span>{/if}
                    <span class="name"><SafeText text={outcomeName(market, o)} />{#if won} <span class="hm-result">{t('Won')}</span>{/if}</span>
                    {#if res}
                      <span class="num strong">{t('%1 of its sat', formatChance(payOf(o.index)))}</span>
                    {:else}
                      <span class="num strong">{formatChance(o.price)}</span>
                    {/if}
                    <OutcomeBar fill={res ? payOf(o.index) : o.price} mark={res ? o.price : null} tone={won ? 'won' : lost ? 'lost' : ''} />
                    <span class="faint small num">{res ? t('%1 before it settled · %2 shares out', formatChance(o.price), formatShares(o.shares)) : t('%1 shares out', formatShares(o.shares))}</span>
                  </button>
                  {#if market.state === 'trading'}
                    <span class="acts">
                      <button type="button" class="btn small" onclick={() => ((chosen = o.index), (side = 'buy'))}>{t('Buy')}</button>
                      {#if held.some((h) => h.outcome === o.index)}<button type="button" class="btn small ghost" onclick={() => ((chosen = o.index), (side = 'sell'))}>{t('Sell')}</button>{/if}
                    </span>
                  {/if}
                </li>
              {/each}
            </ul>
            {#if res && winner}<p class="faint small key"><span class="hm-mark-key" aria-hidden="true"></span>{t('The faint line on each bar: its last price before it settled.')}</p>{/if}
          </div>
        </section>

        {#if payout}
          <section class="panel" data-your-result={payout.total > 0 ? 'paid' : 'none'}>
            <div class="panel-head">
              <h2>{t('What it paid you')}</h2>
              <span class="badge {payout.total > 0 ? 'accent' : ''}"><Amount sats={payout.total} /></span>
            </div>
            <div class="hm-wrap">
              <table class="hm-table" use:stacked aria-label={t('What your shares paid')}>
                <thead><tr><th>{t('For')}</th><th class="end">{t('Shares')}</th><th class="end">{t('A share paid')}</th><th class="end">{t('Paid')}</th></tr></thead>
                <tbody>
                  {#each payout.shares as s (s.address + s.outcome)}
                    {@const o = market.outcomes.find((x) => x.index === s.outcome)}
                    <tr>
                      <td>{#if o}<SafeText text={outcomeName(market, o)} />{:else}{t('Outcome %1', s.outcome)}{/if}</td>
                      <td class="end num">{formatShares(s.shares)}</td>
                      <td class="end num">{formatChance(s.finalPrice)}</td>
                      <td class="end"><Amount sats={s.paid} /></td>
                    </tr>
                  {/each}
                  {#each payout.fees as f (f.address + f.role)}
                    <tr><td>{f.role === 'voter' ? t('Fees: voted with the consensus') : f.role === 'decision-author' ? t('Fees: author of a decision') : t('Fees: author of the market')}</td><td class="end">–</td><td class="end">–</td><td class="end"><Amount sats={f.paid} /></td></tr>
                  {/each}
                  {#if payout.refund}<tr><td>{t('Liquidity left, back to the author')}</td><td class="end">–</td><td class="end">–</td><td class="end"><Amount sats={payout.refund} /></td></tr>{/if}
                </tbody>
              </table>
            </div>
            <p class="panel-body muted small result-sum">{t('Paid to your addresses at block %1, by the block that settled the market.', payout.height)}</p>
          </section>
        {/if}

        {#if held.length}
          <section class="panel">
            <div class="panel-head"><h2>{t('Your positions')}</h2><button type="button" class="btn ghost small" onclick={() => go('portfolio')}>{t('Portfolio')}</button></div>
            <div class="hm-wrap">
              <table class="hm-table" use:stacked aria-label={t('Your positions in this market')}>
                <thead><tr><th>{t('Outcome')}</th><th class="end">{t('Shares')}</th><th class="end">{t('Chance')}</th><th class="end">{t('Worth now')}</th><th class="end">{t('Pays if right')}</th><th><span class="sr-only">{t('Trade')}</span></th></tr></thead>
                <tbody>
                  {#each held as h (h.outcome)}
                    {@const o = market.outcomes.find((x) => x.index === h.outcome)}
                    <tr>
                      <td>{#if o}<SafeText text={outcomeName(market, o)} />{/if}{#if h.addresses.length > 1}<span class="faint small"> · {t('at %1 addresses', h.addresses.length)}</span>{/if}</td>
                      <td class="end num">{formatShares(h.shares)}</td>
                      <td class="end num">{formatChance(o?.price)}</td>
                      <td class="end"><Amount sats={h.value} /></td>
                      <td class="end"><Amount sats={h.shares} /></td>
                      <td class="end">{#if market.state === 'trading'}<button type="button" class="btn small ghost" onclick={() => ((chosen = h.outcome), (side = 'sell'))}>{t('Sell')}</button>{/if}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          </section>
        {/if}

        <section class="panel">
          <div class="panel-head"><h2>{t('Decisions')}</h2><span class="muted small">{t('It pays out once they are all resolved.')}</span></div>
          <ul class="decisions">
            {#each market.decisions as d (d.id)}
              {@const full = decisions.get(d.id)}
              <li>
                <span class="hm-stage" class:live={d.state === 'claimed'} class:vote={d.state === 'voting'} class:done={d.state === 'resolved'}>{d.state === 'voting' ? t('Voting') : d.state === 'resolved' ? t('Resolved') : t('Open')}</span>
                <span class="q"><SafeText text={d.header} /></span>
                <span class="faint small">
                  {d.type?.kind === 'scaled' ? t('A number from %1 to %2', d.type.min ?? 0, d.type.max ?? 0) : d.type?.kind === 'category' ? t('One of %1 options', d.type.options?.length ?? 0) : t('Yes or no')}
                  · {#if d.state === 'resolved' && full}{t('answer:')} <SafeText text={resolvedText(full)} />{:else}{t('voted on in period %1', d.votingPeriod)}{/if}
                </span>
              </li>
            {/each}
          </ul>
        </section>

        {#if market.holders?.length}
          <section class="panel">
            <div class="panel-head"><h2>{t('Who holds shares')}</h2><span class="muted small">{t('%1 position(s)', market.holders.length)}</span></div>
            <div class="hm-wrap">
              <table class="hm-table" use:stacked aria-label={t('Who holds shares')}>
                <thead><tr><th>{t('Address')}</th><th>{t('Outcome')}</th><th class="end">{t('Shares')}</th></tr></thead>
                <tbody>
                  {#each [...market.holders].sort((a, b) => b.shares - a.shares).slice(0, 12) as h (h.address + h.outcome)}
                    {@const o = market.outcomes.find((x) => x.index === h.outcome)}
                    <tr>
                      <td class="cut mono"><span translate="no">{h.address}</span>{#if h.mine} <span class="badge accent">{t('yours')}</span>{/if}</td>
                      <td>{#if o}<SafeText text={outcomeName(market, o)} />{/if}</td>
                      <td class="end num">{formatShares(h.shares)}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          </section>
        {/if}
      </div>

      {#if market.state === 'trading'}
        <aside class="side">
          {#if chosen >= 0}<TradePanel {market} bind:chosen bind:side {held} />{/if}
          {#if market.mine}
            <section class="panel amplify">
              <div class="panel-head"><h2>{t('Add liquidity')}</h2></div>
              <div class="panel-body stack">
                <p class="hm-help">{t('Your market: more liquidity makes its prices move less with each trade.')}</p>
                <input type="text" inputmode="decimal" placeholder="0.00000000" bind:value={liquidityText} aria-label={t('ECX to add')} />
                <button type="button" class="btn" disabled={liqBusy || !(Number(liquidityText) > 0)} onclick={amplify}>{t('Review and add…')}</button>
              </div>
            </section>
          {/if}
        </aside>
      {/if}
    </div>
  </div>
{/if}

<style>
  .market-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    container-type: inline-size;
  }
  .crumbs {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    justify-content: space-between;
    gap: 0.6rem 1rem;
  }
  .title {
    min-width: 0;
    flex: 1 1 24rem;
  }
  .title h1 {
    font-size: 1.55rem;
    line-height: 1.25;
    overflow-wrap: anywhere;
    display: -webkit-box;
    -webkit-line-clamp: 4;
    line-clamp: 4;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }
  .desc {
    margin: 0.35rem 0 0;
    overflow-wrap: anywhere;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }
  .sub {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    margin: 0.5rem 0 0;
  }
  .id {
    display: flex;
    align-items: center;
    gap: 0.3rem;
  }
  .cols {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(18rem, 22rem);
    gap: 1rem;
    align-items: start;
  }
  .cols.solo {
    grid-template-columns: minmax(0, 1fr);
  }
  .main,
  .side {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .side {
    position: sticky;
    top: 0;
  }
  .outcomes,
  .decisions {
    list-style: none;
    margin: 0.8rem 0 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .outcomes li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .outcome {
    flex: 1;
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    grid-template-areas: 'dot name pct' 'bar bar bar' 'sold sold sold';
    align-items: center;
    gap: 0.25rem 0.55rem;
    padding: 0.55rem 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
    min-width: 0;
  }
  .outcome.chosen {
    border-color: var(--accent);
  }
  .outcome.won {
    border-color: var(--positive);
  }
  .outcome .hm-dot,
  .outcome .hm-check {
    grid-area: dot;
  }
  .outcome .name {
    grid-area: name;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.25rem 0.45rem;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .outcome .strong {
    grid-area: pct;
    font-weight: 700;
  }
  .outcome :global(.hm-barwrap) {
    grid-area: bar;
  }
  .outcome .small {
    grid-area: sold;
  }
  .verdict {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    margin-bottom: 0.9rem;
  }
  .verdict p {
    margin: 0;
  }
  .answer {
    display: flex;
    align-items: center;
    gap: 0.55rem;
    font-size: 1.3rem;
  }
  .hm-check.big {
    width: 1.6rem;
    height: 1.6rem;
  }
  .key {
    display: flex;
    align-items: center;
    gap: 0.2rem;
    margin: 0.6rem 0 0;
  }
  .result-sum {
    margin: 0;
    padding-top: 0.6rem;
  }
  .acts {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .decisions {
    padding: 0 1rem 1rem;
  }
  .decisions li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 0.2rem 0.6rem;
    align-items: center;
  }
  .decisions .q {
    overflow-wrap: anywhere;
  }
  .decisions .small {
    grid-column: 2;
  }
  .small {
    font-size: 0.82rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  @container (max-width: 820px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
    }
    .side {
      position: static;
    }
  }
</style>
