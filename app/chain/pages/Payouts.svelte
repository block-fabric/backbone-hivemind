<script lang="ts">
  // Payouts: what the markets paid. A market settles in the block that resolves the last of its decisions: each share
  // gets what its outcome came to (its whole sat for the outcome that won), the trading fees go half to the voters who
  // voted with the consensus, a quarter to the authors of its decisions, a quarter to its author, and what is left of
  // its liquidity back to its author. Each settled market with its result bars, and what it paid this wallet, as the
  // chain recorded it in the block that paid.
  import Amount from '../../components/Amount.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import '../hm/hm.css';
  import MarketOdds from '../hm/MarketOdds.svelte';
  import { formatShares, marketStage, titleWithoutCategory } from '../hm/model';
  import { hm, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const settled = $derived(hm.markets.filter((m) => marketStage(m) === 'paid').sort((a, b) => (b.settledHeight ?? 0) - (a.settledHeight ?? 0)));
  const waiting = $derived(hm.markets.filter((m) => marketStage(m) === 'voting' || marketStage(m) === 'partly'));
  const paidMe = $derived(hm.payouts.reduce((n, p) => n + p.total, 0));
  const byMarket = $derived(new Map(hm.payouts.map((p) => [p.market, p])));
  const roleWords = (r: string) => (r === 'voter' ? t('as a voter with the consensus') : r === 'decision-author' ? t('as the author of a decision') : t('as the author of the market'));
</script>

<div class="page payouts">
  <header class="page-head"><h1>{t('Payouts')}</h1></header>

  <section class="hm-figures">
    <div class="panel hm-figure"><span class="muted">{t('Paid to you')}</span><strong><Amount sats={paidMe} /></strong><span class="faint small">{t('by %1 market(s)', hm.payouts.filter((p) => p.total > 0).length)}</span></div>
    <div class="panel hm-figure"><span class="muted">{t('Markets paid out')}</span><strong class="num">{settled.length}</strong><span class="faint small">{t('settled by the votes')}</span></div>
    <div class="panel hm-figure"><span class="muted">{t('Being voted on')}</span><strong class="num">{waiting.length}</strong><span class="faint small">{t('pay out once resolved')}</span></div>
  </section>

  {#if !hm.loaded}
    <p class="hm-empty">{t('Asking the node…')}</p>
  {:else if settled.length === 0}
    <section class="panel"><p class="hm-empty">{t('No market has paid out yet. A market pays out in the block that resolves the last of its decisions.')}</p></section>
  {:else}
    <ul class="hm-grid list" aria-label={t('Markets that paid out')}>
      {#each settled as m (m.id)}
        {@const p = byMarket.get(m.id)}
        <li>
          <article class="panel card" data-market={m.id}>
            <span class="top">
              <span class="hm-stage done">{t('Paid out')}</span>
              {#if m.settledHeight !== undefined}<span class="faint small num">{t('block %1', m.settledHeight)}</span>{/if}
              {#if p && p.total > 0}<span class="badge accent">{t('Paid you')} <Amount sats={p.total} /></span>{/if}
            </span>
            <button type="button" class="title" onclick={() => go('markets', `m-${m.id}`)}><SafeText text={titleWithoutCategory(m.title)} /></button>
            <MarketOdds market={m} limit={4} />
            {#if p && (p.shares.length || p.fees.length || p.refund)}
              <div class="hm-wrap">
                <table class="hm-table" use:stacked aria-label={t('What it paid you')}>
                  <tbody>
                    {#each p.shares as s (s.address + s.outcome)}
                      {@const o = m.outcomes.find((x) => x.index === s.outcome)}
                      <tr><td>{t('%1 shares of', formatShares(s.shares))} <SafeText text={o ? (m.decisions.length === 1 ? (o.parts[0] ?? o.label) : o.label) : String(s.outcome)} /></td><td class="end"><Amount sats={s.paid} /></td></tr>
                    {/each}
                    {#each p.fees as f (f.address + f.role)}<tr><td>{t('Fees')} {roleWords(f.role)}</td><td class="end"><Amount sats={f.paid} /></td></tr>{/each}
                    {#if p.refund}<tr><td>{t('Liquidity left, back to you')}</td><td class="end"><Amount sats={p.refund} /></td></tr>{/if}
                  </tbody>
                </table>
              </div>
            {:else}
              <span class="faint small">{t('It paid this wallet nothing.')}</span>
            {/if}
          </article>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .payouts {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .list > li {
    display: flex;
    min-width: 0;
  }
  .card {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem;
    min-width: 0;
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .title {
    padding: 0;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    font-size: 1.02rem;
    font-weight: 700;
    text-align: left;
    cursor: pointer;
    overflow-wrap: anywhere;
  }
  .title:hover {
    color: var(--accent);
  }
  .small {
    font-size: 0.82rem;
  }
</style>
