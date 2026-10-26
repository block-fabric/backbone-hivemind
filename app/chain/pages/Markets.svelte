<script lang="ts">
  // The markets: every market of the chain as a card, to search, filter (stage, kind, the wallet's) and sort; a card
  // opens the market (hm/MarketView.svelte: its chances over time and the panel to trade in it). Its second tab is
  // Create (claim a decision, open a market on decisions); go('create', …) leads there.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav, ownTab, setOwnTab } from '../../lib/nav.svelte';
  import '../hm/hm.css';
  import Countdown from '../hm/Countdown.svelte';
  import MarketOdds from '../hm/MarketOdds.svelte';
  import MarketView from '../hm/MarketView.svelte';
  import { categoryOf, kindText, marketKind, marketStage, periodEnd, stageText, titleWithoutCategory, type Market, type MarketKind } from '../hm/model';
  import { hm, startHivemind } from '../hm/store.svelte';
  import Create from './Create.svelte';

  $effect(() => startHivemind());

  const openId = $derived(nav.tab.startsWith('m-') ? nav.tab.slice(2) : '');
  const open = (id: string) => go('markets', `m-${id}`);

  const TABS = [
    { id: 'all', label: 'All markets' },
    { id: 'create', label: 'Create' },
  ];
  const creating = $derived(nav.tab === 'create' || nav.tab.startsWith('create/'));
  const createRow = $derived(
    creating
      ? {
          label: 'What to create',
          tabs: [
            { id: 'market', label: 'A market' },
            { id: 'decision', label: 'A decision alone' },
          ],
          current: ownTab('create').startsWith('decision') ? 'decision' : 'market',
          select: (id: string) => setOwnTab('create', id),
        }
      : null,
  );

  type StatusFilter = 'live' | 'voting' | 'paid' | 'all';
  let chosenStatus = $state<StatusFilter | null>(null);
  $effect(() => {
    const routed = /^status\/(live|voting|paid|all)$/.exec(nav.tab);
    if (routed) chosenStatus = routed[1] as StatusFilter;
  });
  let kind = $state<MarketKind | 'all'>('all');
  let onlyMine = $state(false);
  let search = $state('');
  let sort = $state<'newest' | 'volume' | 'liquidity' | 'uncertain'>('newest');

  const mineIds = $derived(new Set([...hm.positions.map((p) => p.market), ...hm.markets.filter((m) => m.mine).map((m) => m.id)]));
  const stageOf = (m: Market) => marketStage(m);
  const counts = $derived({
    live: hm.markets.filter((m) => stageOf(m) === 'trading').length,
    voting: hm.markets.filter((m) => stageOf(m) === 'voting' || stageOf(m) === 'partly').length,
    paid: hm.markets.filter((m) => stageOf(m) === 'paid').length,
    all: hm.markets.length,
  });
  const status = $derived<StatusFilter>(chosenStatus ?? (counts.live + counts.voting || !counts.all ? 'live' : 'all'));

  function uncertainty(m: Market): number {
    const ps = m.outcomes.map((o) => o.price);
    return -ps.reduce((n, p) => n + (p > 0 ? p * Math.log(p) : 0), 0) / Math.log(Math.max(2, ps.length));
  }

  const shown = $derived.by(() => {
    const q = search.trim().toLowerCase();
    const list = hm.markets.filter((m) => {
      const s = stageOf(m);
      if (status === 'live' && s !== 'trading' && s !== 'voting' && s !== 'partly') return false;
      if (status === 'voting' && s !== 'voting' && s !== 'partly') return false;
      if (status === 'paid' && s !== 'paid') return false;
      if (kind !== 'all' && marketKind(m) !== kind) return false;
      if (onlyMine && !mineIds.has(m.id)) return false;
      if (q && !m.title.toLowerCase().includes(q) && !m.id.startsWith(q) && !m.decisions.some((d) => d.header.toLowerCase().includes(q))) return false;
      return true;
    });
    const by: Record<typeof sort, (a: Market, b: Market) => number> = {
      newest: (a, b) => b.createdHeight - a.createdHeight,
      volume: (a, b) => b.volume - a.volume,
      liquidity: (a, b) => b.liquidity - a.liquidity,
      uncertain: (a, b) => uncertainty(b) - uncertainty(a),
    };
    return list.sort(by[sort]);
  });

  const totalVolume = $derived(hm.markets.reduce((n, m) => n + m.volume, 0));
  const worth = $derived(hm.positions.reduce((n, p) => n + p.value, 0));
  const clock = $derived(hm.status?.period ?? null);
</script>

{#if openId}
  {#key openId}<MarketView id={openId} back={() => go('markets')} />{/key}
{:else}
  <div class="page markets">
    <header class="page-head head">
      <div>
        <h1>{t('Markets')}</h1>
        {#if clock}
          <p class="muted sub">{t('Period %1', clock.current)} · {t('it')} <Countdown {clock} target={periodEnd(clock, clock.current)} phrase="ends" /></p>
        {/if}
      </div>
      <div class="row">
        <button type="button" class="btn" onclick={() => go('portfolio')}><Icon name="wallet" size={15} /> {t('Portfolio')}</button>
        {#if !creating}<button type="button" class="btn primary" onclick={() => go('create', 'market')}><Icon name="plus" size={15} /> {t('Create a market')}</button>{/if}
      </div>
    </header>

    <TabRow label="Markets" tabs={TABS} current={creating ? 'create' : 'all'} select={(id) => (id === 'create' ? go('create', 'market') : (nav.tab = ''))} sub={createRow} />

    {#if hm.error}<p class="side-note side-bad" role="alert"><SafeText text={hm.error} /></p>{/if}

    {#if creating}
      <Create />
    {:else}
      <section class="hm-figures">
        <div class="panel hm-figure"><span class="muted">{t('Trading')}</span><strong class="num">{counts.live}</strong><span class="faint small">{t('of %1 markets', counts.all)}</span></div>
        <div class="panel hm-figure"><span class="muted">{t('Traded')}</span><strong><Amount sats={totalVolume} /></strong><span class="faint small">{t('all markets')}</span></div>
        <div class="panel hm-figure"><span class="muted">{t('Paid out')}</span><strong class="num">{counts.paid}</strong><span class="faint small">{t('markets settled by the vote')}</span></div>
        <div class="panel hm-figure"><span class="muted">{t('Your shares are worth')}</span><strong><Amount sats={worth} /></strong><span class="faint small">{t('%1 position(s)', hm.positions.length)}</span></div>
      </section>

      <section class="panel filters" aria-label={t('Find a market')}>
        <div class="searchbox">
          <Icon name="search" size={16} />
          <input type="search" placeholder={t('Search markets: words of the title or of a decision, an id')} bind:value={search} aria-label={t('Search markets')} />
        </div>
        <div class="hm-chips" role="group" aria-label={t('Stage')}>
          <button type="button" class="hm-chip" aria-pressed={status === 'live'} onclick={() => (chosenStatus = 'live')}>{t('Trading')} <span class="count">{counts.live + counts.voting}</span></button>
          <button type="button" class="hm-chip" aria-pressed={status === 'voting'} onclick={() => (chosenStatus = 'voting')}>{t('Being voted on')} <span class="count">{counts.voting}</span></button>
          <button type="button" class="hm-chip" aria-pressed={status === 'paid'} onclick={() => (chosenStatus = 'paid')}>{t('Paid out')} <span class="count">{counts.paid}</span></button>
          <button type="button" class="hm-chip" aria-pressed={status === 'all'} onclick={() => (chosenStatus = 'all')}>{t('All')} <span class="count">{counts.all}</span></button>
        </div>
        <div class="hm-chips" role="group" aria-label={t('Kind')}>
          <button type="button" class="hm-chip" aria-pressed={kind === 'all'} onclick={() => (kind = 'all')}>{t('Every kind')}</button>
          <button type="button" class="hm-chip" aria-pressed={kind === 'binary'} onclick={() => (kind = 'binary')}>{t('Yes or no')}</button>
          <button type="button" class="hm-chip" aria-pressed={kind === 'scalar'} onclick={() => (kind = 'scalar')}>{t('Scale')}</button>
          <button type="button" class="hm-chip" aria-pressed={kind === 'categorical'} onclick={() => (kind = 'categorical')}>{t('Options')}</button>
          <button type="button" class="hm-chip" aria-pressed={kind === 'combo'} onclick={() => (kind = 'combo')}>{t('Several decisions')}</button>
          <button type="button" class="hm-chip" aria-pressed={onlyMine} onclick={() => (onlyMine = !onlyMine)}><Icon name="wallet" size={13} /> {t('Mine')}</button>
        </div>
        <label class="pick">
          <span class="muted">{t('Sort')}</span>
          <select class="pill" bind:value={sort}>
            <option value="newest">{t('Newest')}</option>
            <option value="volume">{t('Most traded')}</option>
            <option value="liquidity">{t('Deepest')}</option>
            <option value="uncertain">{t('Most uncertain')}</option>
          </select>
        </label>
      </section>

      {#if !hm.loaded}
        <p class="hm-empty">{t('Asking the node…')}</p>
      {:else if shown.length === 0}
        <div class="panel none">
          <p class="hm-empty">{hm.markets.length === 0 ? t('No market yet. Open the first one: on a decision listed for a period, or on a new one.') : t('No market matches.')}</p>
          {#if hm.markets.length === 0}<button type="button" class="btn primary" onclick={() => go('create', 'market')}><Icon name="plus" size={15} /> {t('Create a market')}</button>{/if}
        </div>
      {:else}
        <p class="muted small count-line">{t('%1 market(s)', shown.length)}</p>
        <ul class="hm-grid cards" aria-label={t('Markets')}>
          {#each shown as m (m.id)}
            {@const stage = stageOf(m)}
            {@const cat = categoryOf(m)}
            {@const paid = hm.payouts.find((p) => p.market === m.id)}
            <li><button type="button" class="panel card" class:settled={stage === 'paid'} data-market={m.id} onclick={() => open(m.id)}>
              <span class="top">
                <span class="hm-stage" class:live={stage === 'trading'} class:vote={stage === 'voting' || stage === 'partly'} class:done={stage === 'paid'} class:bad={stage === 'cancelled' || stage === 'invalid'}>{stageText(stage)}</span>
                <span class="badge">{kindText(marketKind(m), m.outcomes.length)}</span>
                {#if cat}<span class="badge"><SafeText text={cat} /></span>{/if}
                {#if paid && paid.total > 0}<span class="badge accent">{t('Paid you')} <Amount sats={paid.total} /></span>
                {:else if mineIds.has(m.id)}<span class="badge accent">{t('yours')}</span>{/if}
              </span>
              <strong class="title"><SafeText text={titleWithoutCategory(m.title)} /></strong>
              <MarketOdds market={m} />
              <span class="foot faint small">
                <span>{t('traded')} <Amount sats={m.volume} /></span>
                {#if stage !== 'paid'}<span>{t('liquidity')} <Amount sats={m.liquidity} /></span>{/if}
                {#if m.settledHeight !== undefined}<span class="num">{t('Paid out at block %1', m.settledHeight)}</span>{/if}
              </span>
            </button></li>
          {/each}
        </ul>
      {/if}
    {/if}
  </div>
{/if}

<style>
  .markets {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    justify-content: space-between;
    gap: 0.8rem;
  }
  .sub {
    margin: 0.25rem 0 0;
  }
  .filters {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 0.9rem 1rem;
  }
  .searchbox {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text-muted);
  }
  .searchbox input {
    flex: 1;
    min-width: 0;
    border: 0;
    background: none;
    box-shadow: none;
  }
  .pick {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    font-size: 0.88rem;
  }
  .none {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding-bottom: 1rem;
  }
  .none .hm-empty {
    align-self: stretch;
  }
  .count-line {
    margin: 0;
  }
  .cards {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .cards > li {
    display: flex;
    min-width: 0;
  }
  .card {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
    min-width: 0;
    transition: border-color 160ms;
  }
  .card:hover,
  .card:focus-visible {
    border-color: var(--accent);
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
    align-items: center;
  }
  .title {
    font-size: 1.02rem;
    line-height: 1.35;
    overflow-wrap: anywhere;
  }
  .foot {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.9rem;
    margin-top: auto;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
