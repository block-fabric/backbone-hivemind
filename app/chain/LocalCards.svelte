<script lang="ts">
  // Hivemind's cards on Home: the wallet's bets (what they are worth, what they pay if right), the markets (the most
  // traded live ones, the chance of their favourite), and the vote (the period, the decisions to vote on, the wallet's
  // Votecoin). Each opens its page.
  import Amount from '../components/Amount.svelte';
  import Icon from '../components/Icon.svelte';
  import SafeText from '../components/SafeText.svelte';
  import Card from '../views/home/Card.svelte';
  import { t } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import './hm/hm.css';
  import Countdown from './hm/Countdown.svelte';
  import { formatChance, formatVotecoin, leader, marketStage, outcomeName, periodEnd, titleWithoutCategory } from './hm/model';
  import { hm, startHivemind } from './hm/store.svelte';

  $effect(() => startHivemind());

  const worth = $derived(hm.positions.reduce((n, p) => n + p.value, 0));
  const ifRight = $derived(hm.positions.reduce((n, p) => n + p.shares, 0));
  const live = $derived(
    hm.markets
      .filter((m) => marketStage(m) === 'trading' || marketStage(m) === 'voting')
      .sort((a, b) => b.volume - a.volume || b.createdHeight - a.createdHeight)
      .slice(0, 3),
  );
  const voted = $derived(new Set(hm.ballots.map((b) => b.decision)));
  const toVote = $derived(hm.decisions.filter((d) => d.state === 'voting' && !voted.has(d.id)).length);
  const clock = $derived(hm.status?.period ?? null);
  const total = $derived(hm.status?.votecoinTotal ?? 0);
</script>

<Card title={t('My bets')} icon="portfolio" open={() => go('portfolio')} openLabel={t('Open the portfolio')}>
  <div class="big"><Amount sats={worth} /></div>
  <dl class="pairs">
    <dt>{t('Positions')}</dt><dd class="num">{hm.positions.length}</dd>
    <dt>{t('Pays if right')}</dt><dd><Amount sats={ifRight} /></dd>
  </dl>
  {#snippet actions()}
    <button type="button" class="btn small" onclick={() => go('markets')}><Icon name="market" size={15} /> {t('Trade')}</button>
    <button type="button" class="btn small ghost" onclick={() => go('payouts')}>{t('Payouts')}</button>
  {/snippet}
</Card>

<Card title={t('Markets')} icon="market" open={() => go('markets')} openLabel={t('Open the markets')}>
  {#if live.length === 0}
    <p class="muted small">{hm.loaded ? t('No market is trading yet.') : t('Asking the node…')}</p>
  {:else}
    <ul class="mk">
      {#each live as m (m.id)}
        {@const top = leader(m)}
        <li>
          <button type="button" class="link" onclick={() => go('markets', `m-${m.id}`)}><SafeText text={titleWithoutCategory(m.title)} /></button>
          {#if top}<span class="faint small"><SafeText text={outcomeName(m, top)} /> · {formatChance(top.price)}</span>{/if}
        </li>
      {/each}
    </ul>
  {/if}
  {#snippet actions()}
    <button type="button" class="btn small" onclick={() => go('create', 'market')}><Icon name="create" size={15} /> {t('Create a market')}</button>
  {/snippet}
</Card>

<Card title={t('Vote')} icon="ballot" open={() => go('voting')} openLabel={t('Open the voting center')}>
  <div class="big num">{toVote}<span class="unit">{t('to vote on')}</span></div>
  <dl class="pairs">
    <dt>{t('Your Votecoin')}</dt><dd class="num">{formatVotecoin(hm.votecoin?.votecoin ?? 0)}{#if total > 0} <span class="faint">({formatChance((hm.votecoin?.votecoin ?? 0) / total)})</span>{/if}</dd>
    <dt>{t('Period')}</dt><dd class="num">{#if clock}{clock.current} · <Countdown {clock} target={periodEnd(clock, clock.current)} phrase="ends" />{:else}–{/if}</dd>
  </dl>
  {#snippet actions()}
    <button type="button" class="btn small" class:primary={toVote > 0} onclick={() => go('voting')}><Icon name="ballot" size={15} /> {t('Vote')}</button>
    <button type="button" class="btn small ghost" onclick={() => go('votecoin')}>{t('Votecoin')}</button>
  {/snippet}
</Card>

<style>
  .big {
    font-size: 1.65rem;
    font-weight: 650;
    margin-bottom: 0.4rem;
  }
  .big .unit {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
    margin-inline-start: 0.4rem;
  }
  .pairs {
    display: grid;
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.3rem 1rem;
    margin: 0;
    font-size: 0.9rem;
  }
  .pairs dt {
    color: var(--text-muted);
  }
  .pairs dd {
    margin: 0;
    text-align: right;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .mk {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .mk li {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    min-width: 0;
  }
  .link {
    border: 0;
    padding: 0;
    background: none;
    color: var(--text);
    font: inherit;
    font-weight: 600;
    text-align: left;
    cursor: pointer;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .link:hover {
    text-decoration: underline;
  }
  .small {
    font-size: 0.85rem;
    margin: 0;
  }
</style>
