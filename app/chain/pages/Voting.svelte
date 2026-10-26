<script lang="ts">
  // The voting center: what this wallet's Votecoin votes on. The decisions being voted on now (those of the last period:
  // a decision is voted on in the period after the one its answer becomes known in), answered here and cast together in
  // one ballot; the decisions that come up next; and the wallet's votes, with how they fared once resolved.
  // Hivemind (truthcoin) has no sealed votes: a ballot counts as cast and is public once in a block. Our voting center's
  // two steps, sealing then revealing, are this one (vote_submit), and the page says so. Votecoin, the reputation that
  // votes, is its second tab; go('votecoin') leads there.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { drop, refresh } from '../dropin/store.svelte';
  import '../hm/hm.css';
  import { hmSpend } from '../hm/api';
  import Countdown from '../hm/Countdown.svelte';
  import { answerValue, formatChance, formatVotecoin, periodEnd, resolvedText, type Decision } from '../hm/model';
  import { decisionMap, hm, refreshHivemind, startHivemind } from '../hm/store.svelte';
  import Votecoin from './Votecoin.svelte';

  $effect(() => startHivemind());

  const TABS = [
    { id: 'vote', label: 'Vote' },
    { id: 'votecoin', label: 'Votecoin' },
  ];
  const tab = $derived(nav.tab === 'votecoin' || nav.tab.startsWith('votecoin/') ? 'votecoin' : 'vote');
  const FEE = 1000;

  const clock = $derived(hm.status?.period ?? null);
  const power = $derived(hm.votecoin?.votecoin ?? 0);
  const total = $derived(hm.status?.votecoinTotal ?? 0);
  const votedOn = $derived(new Set(hm.ballots.map((b) => b.decision)));
  const toVote = $derived(hm.decisions.filter((d) => d.state === 'voting').sort((a, b) => a.index - b.index));
  const open = $derived(toVote.filter((d) => !votedOn.has(d.id)));
  const upcoming = $derived(hm.decisions.filter((d) => d.state === 'claimed').sort((a, b) => a.votingPeriod - b.votingPeriod).slice(0, 20));
  const decisions = $derived(decisionMap());
  const past = $derived([...hm.ballots].sort((a, b) => b.height - a.height));

  /** The answers being written, by decision: yes, no, unsure; an option's index or "inconclusive"; a number. */
  let answers = $state<Record<string, string>>({});
  let busy = $state(false);
  let error = $state('');
  const ready = $derived(open.filter((d) => answerValue(d, answers[d.id] ?? '').ok));

  /** What the market on a decision says of it now (its first market's chance of yes, for a yes-or-no decision). */
  function marketSays(d: Decision): number | null {
    const m = hm.markets.find((x) => d.markets.includes(x.id) && x.decisions.length === 1);
    if (!m || d.type.kind !== 'binary') return null;
    return m.outcomes.find((o) => o.coordinates[0] === 1)?.price ?? null;
  }

  async function cast() {
    const votes = ready.map((d) => {
      const v = answerValue(d, answers[d.id] ?? '');
      return { decision_id: d.id, vote_value: v.ok ? v.value : 0 };
    });
    if (!votes.length) return;
    busy = true;
    error = '';
    try {
      await hmSpend.vote(votes, FEE);
      toast('success', votes.length === 1 ? t('Vote cast') : t('Ballot cast'), t('It counts once it is in a block of Hivemind.'));
      answers = {};
      await refresh();
      void refreshHivemind();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page voting">
  <header class="page-head head">
    <div>
      <h1>{t('Voting center')}</h1>
      {#if clock}<p class="muted sub">{t('Period %1: the decisions of period %2 are voted on; it', clock.current, clock.current - 1)} <Countdown {clock} target={periodEnd(clock, clock.current)} phrase="ends" /></p>{/if}
    </div>
    <div class="power">
      <span class="muted small">{t('Your Votecoin')}</span>
      <strong class="num">{formatVotecoin(power)}</strong>
      {#if total > 0}<span class="faint small">{t('%1 of all', formatChance(power / total))}</span>{/if}
    </div>
  </header>

  <TabRow label="Voting center" tabs={TABS} current={tab} select={(id) => (nav.tab = id === 'votecoin' ? 'votecoin' : '')} />

  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else if tab === 'votecoin'}
    <Votecoin />
  {:else}
    <p class="side-note">
      <Icon name="info" size={15} />
      {t('A ballot counts as it is cast: on Hivemind there is no sealed vote to reveal later, and your answers are public once the ballot is in a block. Answer the decisions below, then cast them together.')}
    </p>
    {#if power <= 0}
      <p class="side-note side-warn" role="status">
        {t('This wallet’s voter address holds no Votecoin, so it cannot vote: the chain refuses a ballot without it. Votecoin comes from another holder (a transfer to your voter address).')}
        <button type="button" class="linkish" onclick={() => (nav.tab = 'votecoin')}>{t('About Votecoin')}</button>
      </p>
    {/if}

    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">{t('To vote on now')}</span><strong class="num">{open.length}</strong><span class="faint small">{t('%1 voted already', toVote.length - open.length)}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Coming up')}</span><strong class="num">{upcoming.length}</strong><span class="faint small">{t('voted on in later periods')}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Your votes')}</span><strong class="num">{hm.ballots.length}</strong><span class="faint small">{t('on the chain')}</span></div>
    </section>

    <section class="panel">
      <div class="panel-head">
        <h2>{t('To vote on')}</h2>
        <button type="button" class="btn small primary" id="vote-cast" disabled={busy || ready.length === 0 || power <= 0} onclick={cast}><Icon name="check" size={14} /> {ready.length > 1 ? t('Cast %1 votes…', ready.length) : t('Cast vote…')}</button>
      </div>
      {#if error}<p class="hm-problems pad" role="alert"><SafeText text={error} /></p>{/if}
      {#if open.length === 0}
        <p class="hm-empty">{toVote.length ? t('You voted on every decision of this period.') : t('No decision is being voted on. A decision comes up in the period after the one its answer becomes known in.')}</p>
      {:else}
        <div class="hm-grid pad">
          {#each open as d (d.id)}
            {@const says = marketSays(d)}
            {@const a = answers[d.id] ?? ''}
            <article class="panel ballot" aria-label={t('A decision to vote on')} data-decision={d.id}>
              <span class="top">
                <span class="badge">{d.type.kind === 'scaled' ? t('%1 to %2', d.type.min ?? 0, d.type.max ?? 0) : d.type.kind === 'category' ? t('%1 options', d.type.options?.length ?? 0) : t('Yes or no')}</span>
                <span class="hm-stage {answerValue(d, a).ok ? 'live' : 'vote'}">{answerValue(d, a).ok ? t('Answered') : t('Not voted')}</span>
              </span>
              <strong class="q"><SafeText text={d.header} /></strong>
              {#if d.description}<span class="muted small q"><SafeText text={d.description} /></span>{/if}
              {#if says !== null}<span class="faint small">{t('The market says %1', formatChance(says))}</span>{/if}
              <div class="answers" role="radiogroup" aria-label={t('Your answer')}>
                {#if d.type.kind === 'binary'}
                  <button type="button" role="radio" class="hm-chip" aria-checked={a === 'yes'} onclick={() => (answers[d.id] = 'yes')}><SafeText text={d.type.yesLabel ?? 'Yes'} /></button>
                  <button type="button" role="radio" class="hm-chip" aria-checked={a === 'no'} onclick={() => (answers[d.id] = 'no')}><SafeText text={d.type.noLabel ?? 'No'} /></button>
                  <button type="button" role="radio" class="hm-chip" aria-checked={a === 'unsure'} title={t('Neither: the answer is not clear (counted as a half)')} onclick={() => (answers[d.id] = 'unsure')}>{t('Unsure')}</button>
                {:else if d.type.kind === 'category'}
                  {#each d.type.options ?? [] as o, i (i)}<button type="button" role="radio" class="hm-chip" aria-checked={a === String(i)} onclick={() => (answers[d.id] = String(i))}><SafeText text={o} /></button>{/each}
                  <button type="button" role="radio" class="hm-chip" aria-checked={a === 'inconclusive'} onclick={() => (answers[d.id] = 'inconclusive')}>{t('Inconclusive')}</button>
                {:else}
                  <input type="text" inputmode="decimal" placeholder={t('%1 to %2', d.type.min ?? 0, d.type.max ?? 0)} value={a} oninput={(e) => (answers[d.id] = (e.currentTarget as HTMLInputElement).value)} aria-label={t('The answer')} />
                {/if}
              </div>
              {#if a && !answerValue(d, a).ok}<span class="hm-down small">{(answerValue(d, a) as { ok: false; error: string }).error}</span>{/if}
              {#if d.markets.length}<button type="button" class="btn small ghost self" onclick={() => go('markets', `m-${d.markets[0]}`)}>{t('Its market')}</button>{/if}
            </article>
          {/each}
        </div>
      {/if}
    </section>

    <div class="cols">
      <section class="panel">
        <div class="panel-head"><h2>{t('Coming up')}</h2></div>
        {#if upcoming.length === 0}
          <p class="hm-empty">{t('Nothing comes up yet.')}</p>
        {:else}
          <ul class="list">
            {#each upcoming as d (d.id)}
              <li><div class="q"><SafeText text={d.header} /></div><span class="muted small">{t('voted on in period %1', d.votingPeriod)}{#if clock}, <Countdown {clock} target={periodEnd(clock, d.votingPeriod - 1)} phrase="starts" />{/if}</span></li>
            {/each}
          </ul>
        {/if}
      </section>
      <section class="panel">
        <div class="panel-head"><h2>{t('Your votes')}</h2></div>
        {#if past.length === 0}
          <p class="hm-empty">{t('No vote yet.')}</p>
        {:else}
          <div class="hm-wrap">
            <table class="hm-table" use:stacked aria-label={t('Your votes')}>
              <thead><tr><th>{t('Period')}</th><th>{t('Decision')}</th><th>{t('Your answer')}</th><th>{t('Outcome')}</th></tr></thead>
              <tbody>
                {#each past as v (v.decision + v.voter)}
                  {@const d = decisions.get(v.decision)}
                  <tr>
                    <td class="num">{v.period}</td>
                    <td class="cut"><SafeText text={d?.header ?? v.decision} /></td>
                    <td><SafeText text={v.answer} /></td>
                    <td>{#if d && d.state === 'resolved'}<SafeText text={resolvedText(d)} />{:else}<span class="faint">{t('not resolved yet')}</span>{/if}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </section>
    </div>
  {/if}
</div>

<style>
  .voting {
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
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.35rem;
    margin: 0.25rem 0 0;
  }
  .power {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
  }
  .power strong {
    font-size: 1.5rem;
    font-weight: 650;
  }
  .side-note {
    display: flex;
    align-items: flex-start;
    gap: 0.5rem;
    margin: 0;
  }
  .pad {
    padding: 0 1rem 1rem;
  }
  .ballot {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    padding: 0.9rem;
    background: var(--surface-2);
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .q {
    overflow-wrap: anywhere;
  }
  .answers {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .answers input {
    width: 10rem;
  }
  .self {
    align-self: flex-start;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 1rem;
    align-items: start;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0 1rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .list li {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
  }
  .linkish {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    font: inherit;
    text-decoration: underline;
    cursor: pointer;
  }
  .small {
    font-size: 0.83rem;
  }
</style>
