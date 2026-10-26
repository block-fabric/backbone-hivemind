<script lang="ts">
  // Portfolio: the shares this wallet holds, by market and outcome (the chain keeps them by address), what they are worth
  // at the chance now and what they pay if right; a sale opens the market on that outcome. Markets that paid out: Payouts.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { drop } from '../dropin/store.svelte';
  import '../hm/hm.css';
  import { colour, formatChance, formatShares, marketStage, outcomeName, stageText, titleWithoutCategory } from '../hm/model';
  import { heldByOutcome, hm, openTrade, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const rows = $derived(
    heldByOutcome(hm.positions)
      .map((h) => {
        const m = hm.markets.find((x) => x.id === h.market) ?? null;
        const o = m?.outcomes.find((x) => x.index === h.outcome) ?? null;
        return { ...h, m, o };
      })
      .sort((a, b) => b.value - a.value),
  );
  const worth = $derived(rows.reduce((n, r) => n + r.value, 0));
  const ifRight = $derived(rows.reduce((n, r) => n + r.shares, 0));
  const markets = $derived(new Set(rows.map((r) => r.market)).size);
  const paidTotal = $derived(hm.payouts.reduce((n, p) => n + p.total, 0));

  function sell(market: string, outcome: number) {
    openTrade(market, outcome, 'sell');
    go('markets', `m-${market}`);
  }
</script>

<div class="page portfolio">
  <header class="page-head head">
    <h1>{t('Portfolio')}</h1>
    <button type="button" class="btn" onclick={() => go('markets')}><Icon name="market" size={15} /> {t('Markets')}</button>
  </header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">{t('Worth now')}</span><strong><Amount sats={worth} /></strong><span class="faint small">{t('at the chances now')}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Pays if all right')}</span><strong><Amount sats={ifRight} /></strong><span class="faint small">{t('1 sat a share')}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Positions')}</span><strong class="num">{rows.length}</strong><span class="faint small">{t('in %1 market(s)', markets)}</span></div>
      <div class="panel hm-figure"><span class="muted">{t('Paid out to you')}</span><strong><Amount sats={paidTotal} /></strong><span class="faint small"><button type="button" class="linkish" onclick={() => go('payouts')}>{t('Payouts')}</button></span></div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>{t('Your positions')}</h2></div>
      {#if !hm.loaded}
        <p class="hm-empty">{t('Asking the node…')}</p>
      {:else if rows.length === 0}
        <div class="none">
          <p class="hm-empty">{t('No shares yet. Pick a market and buy the outcome you think comes true.')}</p>
          <button type="button" class="btn primary" onclick={() => go('markets')}>{t('See the markets')}</button>
        </div>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label={t('Your positions')}>
            <thead><tr><th>{t('Market')}</th><th>{t('Outcome')}</th><th class="end">{t('Shares')}</th><th class="end">{t('Chance')}</th><th class="end">{t('Worth now')}</th><th class="end">{t('Pays if right')}</th><th>{t('Stage')}</th><th><span class="sr-only">{t('Trade')}</span></th></tr></thead>
            <tbody>
              {#each rows as r (r.market + ':' + r.outcome)}
                <tr class="click" tabindex="0" onclick={() => go('markets', `m-${r.market}`)} onkeydown={(e) => e.key === 'Enter' && go('markets', `m-${r.market}`)}>
                  <td class="cut"><SafeText text={r.m ? titleWithoutCategory(r.m.title) : r.market} /></td>
                  <td>{#if r.m && r.o}<span class="hm-dot hm-c{colour(r.m, r.o)}"></span> <SafeText text={outcomeName(r.m, r.o)} />{:else}{t('Outcome %1', r.outcome)}{/if}</td>
                  <td class="end num">{formatShares(r.shares)}</td>
                  <td class="end num">{formatChance(r.o?.price)}</td>
                  <td class="end"><Amount sats={r.value} /></td>
                  <td class="end"><Amount sats={r.shares} /></td>
                  <td>{#if r.m}<span class="hm-stage" class:live={marketStage(r.m) === 'trading'} class:vote={marketStage(r.m) === 'voting'}>{stageText(marketStage(r.m))}</span>{/if}</td>
                  <td class="end">{#if r.m?.state === 'trading'}<button type="button" class="btn small ghost" onclick={(e) => (e.stopPropagation(), sell(r.market, r.outcome))}>{t('Sell…')}</button>{/if}</td>
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
  .portfolio {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.8rem;
  }
  .none {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding-bottom: 1.2rem;
  }
  .linkish {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    font: inherit;
    cursor: pointer;
    text-decoration: underline;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
