<script lang="ts">
  // Decisions: the questions the markets are about. Each takes a slot of a period (its listing fee), is voted on by the
  // holders of Votecoin in the next period, and is resolved by the first block after that (its markets pay out once all
  // their decisions are). The calendar: the period now, the periods that take decisions and their cheapest slot.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import '../hm/hm.css';
  import { hmApi } from '../hm/api';
  import Countdown from '../hm/Countdown.svelte';
  import { periodEnd, resolvedText, type Decision, type DecisionState, type OpenPeriod } from '../hm/model';
  import { hm, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const TABS = [
    { id: 'list', label: 'Decisions' },
    { id: 'calendar', label: 'Calendar' },
  ];
  const tab = $derived(nav.tab === 'calendar' ? 'calendar' : 'list');
  let filter = $state<DecisionState | 'all'>('all');
  let search = $state('');
  let periods = $state<OpenPeriod[]>([]);
  $effect(() => {
    void hm.version;
    hmApi
      .openPeriods()
      .then((p) => (periods = p))
      .catch(() => (periods = []));
  });

  const clock = $derived(hm.status?.period ?? null);
  const counts = $derived({
    claimed: hm.decisions.filter((d) => d.state === 'claimed').length,
    voting: hm.decisions.filter((d) => d.state === 'voting').length,
    resolved: hm.decisions.filter((d) => d.state === 'resolved').length,
    all: hm.decisions.length,
  });
  const shown = $derived.by(() => {
    const q = search.trim().toLowerCase();
    const order: Record<string, number> = { voting: 0, claimed: 1, resolved: 2, invalid: 3, created: 4 };
    return hm.decisions
      .filter((d) => (filter === 'all' || d.state === filter) && (!q || d.header.toLowerCase().includes(q) || d.id.startsWith(q)))
      .sort((a, b) => (order[a.state] ?? 5) - (order[b.state] ?? 5) || a.votingPeriod - b.votingPeriod || a.index - b.index);
  });

  function kindWords(d: Decision): string {
    if (d.type.kind === 'scaled') return t('%1 to %2', d.type.min ?? 0, d.type.max ?? 0);
    if (d.type.kind === 'category') return t('%1 options', d.type.options?.length ?? 0);
    return t('Yes or no');
  }
  function stateWords(s: DecisionState): string {
    return s === 'voting' ? t('Voting') : s === 'resolved' ? t('Resolved') : s === 'invalid' ? t('Invalid') : t('Open');
  }
  /** The decisions voted on in a period. */
  const votedIn = (p: number) => hm.decisions.filter((d) => d.votingPeriod === p);
</script>

<div class="page decisions">
  <header class="page-head head">
    <div>
      <h1>{t('Decisions')}</h1>
      {#if clock}<p class="muted sub">{t('Period %1', clock.current)} · {t('it')} <Countdown {clock} target={periodEnd(clock, clock.current)} phrase="ends" /></p>{/if}
    </div>
    <button type="button" class="btn primary" onclick={() => go('create', 'decision')}><Icon name="plus" size={15} /> {t('List a decision')}</button>
  </header>

  <TabRow label="Decisions" tabs={TABS} current={tab} select={(id) => (nav.tab = id === 'calendar' ? 'calendar' : '')} />

  {#if tab === 'calendar'}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">{t('Period now')}</span><strong class="num">{clock?.current ?? '–'}</strong><span class="faint small">{clock ? (clock.unit === 'blocks' ? t('%1 blocks a period', clock.length) : t('a day a period')) : ''}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Voted on now')}</span><strong class="num">{counts.voting}</strong><span class="faint small">{t('decisions of period %1', (clock?.current ?? 1) - 1)}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Open')}</span><strong class="num">{counts.claimed}</strong><span class="faint small">{t('voted on later')}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Resolved')}</span><strong class="num">{counts.resolved}</strong><span class="faint small">{t('by the votes')}</span></div>
    </section>
    <section class="panel">
      <div class="panel-head"><h2>{t('Periods')}</h2><span class="muted small">{t('A decision of a period is voted on in the next one.')}</span></div>
      {#if !clock}
        <p class="hm-empty">{t('Asking the node…')}</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label={t('Periods')}>
            <thead><tr><th>{t('Period')}</th><th>{t('When')}</th><th class="end">{t('Decisions voted on')}</th><th class="end">{t('Cheapest slot')}</th><th class="end">{t('Slots open')}</th></tr></thead>
            <tbody>
              {#each Array.from({ length: 8 }, (_, i) => clock.current - 1 + i) as p (p)}
                {@const open = periods.find((x) => x.period_index === p)}
                <tr class:now={p === clock.current}>
                  <td class="num">{p}{#if p === clock.current} <span class="hm-stage live">{t('now')}</span>{/if}</td>
                  <td>{#if p < clock.current}{t('over')}{:else if p === clock.current}<Countdown {clock} target={periodEnd(clock, p)} phrase="ends" />{:else}<Countdown {clock} target={periodEnd(clock, p - 1)} phrase="starts" />{/if}</td>
                  <td class="end num">{votedIn(p).length}</td>
                  <td class="end">{#if open}<Amount sats={open.cheapest_available_slot_sats} />{:else}<span class="faint">–</span>{/if}</td>
                  <td class="end num">{open ? open.slots_available_by_tier.reduce((n, x) => n + x, 0) : '–'}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {:else}
    <section class="panel filters">
      <div class="searchbox">
        <Icon name="search" size={16} />
        <input type="search" placeholder={t('Search decisions: words of the question, an id')} bind:value={search} aria-label={t('Search decisions')} />
      </div>
      <div class="hm-chips" role="group" aria-label={t('State')}>
        <button type="button" class="hm-chip" aria-pressed={filter === 'all'} onclick={() => (filter = 'all')}>{t('All')} <span class="count">{counts.all}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'voting'} onclick={() => (filter = 'voting')}>{t('Voting')} <span class="count">{counts.voting}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'claimed'} onclick={() => (filter = 'claimed')}>{t('Open')} <span class="count">{counts.claimed}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'resolved'} onclick={() => (filter = 'resolved')}>{t('Resolved')} <span class="count">{counts.resolved}</span></button>
      </div>
    </section>
    <section class="panel">
      {#if !hm.loaded}
        <p class="hm-empty">{t('Asking the node…')}</p>
      {:else if shown.length === 0}
        <p class="hm-empty">{hm.decisions.length ? t('No decision matches.') : t('No decision listed yet.')}</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label={t('Decisions')}>
            <thead><tr><th>{t('State')}</th><th>{t('Question')}</th><th>{t('Answer')}</th><th class="end">{t('Known in')}</th><th class="end">{t('Voted in')}</th><th>{t('Result')}</th><th class="end">{t('Markets')}</th></tr></thead>
            <tbody>
              {#each shown as d (d.id)}
                <tr>
                  <td><span class="hm-stage" class:live={d.state === 'claimed'} class:vote={d.state === 'voting'} class:done={d.state === 'resolved'}>{stateWords(d.state)}</span></td>
                  <td class="q"><SafeText text={d.header} />{#if d.mine} <span class="badge accent">{t('yours')}</span>{/if}<span class="faint small mono id" translate="no">{d.id}</span></td>
                  <td>{kindWords(d)}</td>
                  <td class="end num">{d.period}</td>
                  <td class="end num">{d.votingPeriod}</td>
                  <td>{#if d.state === 'resolved'}<strong><SafeText text={resolvedText(d)} /></strong>{:else}<span class="faint">–</span>{/if}</td>
                  <td class="end">{#if d.markets.length}<button type="button" class="btn small ghost" onclick={() => go('markets', `m-${d.markets[0]}`)}>{d.markets.length}</button>{:else}<span class="faint">0</span>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {/if}
</div>

<style>
  .decisions {
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
  .q {
    overflow-wrap: anywhere;
  }
  .id {
    display: block;
    margin-top: 0.15rem;
  }
  tr.now td {
    background: var(--surface-2);
  }
  .small {
    font-size: 0.82rem;
  }
</style>
