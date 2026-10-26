<script lang="ts">
  // Trading in a market: buy or sell shares of one of its outcomes. A share pays 1 sat if its outcome is what the
  // decisions resolve to. The node quotes the trade (hivemind.quote: the market maker's own arithmetic) as the amount is
  // typed; the order carries the most it pays (or the least it takes) with the slippage chosen, so that a price that
  // moved before the block takes it cannot cost more. Asked in the app's trusted dialog (market_buy, market_sell).
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import { drop, refresh } from '../dropin/store.svelte';
  import { hmApi, hmSpend } from './api';
  import { colour, formatChance, formatShares, maxCost, minProceeds, outcomeName, type Market, type Quote } from './model';
  import { hm, refreshHivemind, type Held } from './store.svelte';

  let {
    market,
    chosen = $bindable(0),
    side = $bindable<'buy' | 'sell'>('buy'),
    held,
  }: { market: Market; chosen?: number; side?: 'buy' | 'sell'; held: Held[] } = $props();

  const SLIPPAGES = [0.005, 0.01, 0.02, 0.05];
  let slippage = $state(0.01);
  let mode = $state<'coins' | 'shares'>('coins');
  let amountText = $state('');
  let busy = $state(false);
  let error = $state('');
  let quote = $state<{ key: string; value: Quote | null; error: string } | null>(null);

  const trading = $derived(market.state === 'trading');
  const amount = $derived(Number(amountText.replace(',', '.').replace(/\s/g, '')));
  const valid = $derived(amountText.trim() !== '' && Number.isFinite(amount) && amount > 0);
  const mine = $derived(held.find((h) => h.market === market.id && h.outcome === chosen) ?? null);
  const holding = $derived(mine?.shares ?? 0);
  let address = $state('');
  $effect(() => {
    if (!mine?.addresses.some((a) => a.address === address)) address = mine?.addresses[0]?.address ?? '';
  });
  const atAddress = $derived(mine?.addresses.find((a) => a.address === address)?.shares ?? 0);

  /** The market maker's cost of `n` more shares of the outcome (LMSR, b = beta, in sats), its fee included: to turn coins
   *  into shares; the node's quote is what the order carries. */
  function lmsrCost(n: number): number {
    const b = market.beta ?? 0;
    if (!(b > 0)) return Infinity;
    const q = market.outcomes.map((o) => o.shares);
    const at = market.outcomes.findIndex((o) => o.index === chosen);
    const lse = (xs: number[]) => {
      const top = Math.max(...xs.map((x) => x / b));
      return b * (top + Math.log(xs.reduce((s, x) => s + Math.exp(x / b - top), 0)));
    };
    const after = q.slice();
    after[at] += n;
    return (lse(after) - lse(q)) * (1 + (market.feeRate ?? 0));
  }

  /** The shares of the order: typed, or what a budget in ECX buys (bisection over the market maker's cost). */
  const shares = $derived.by(() => {
    if (!valid) return 0;
    if (side === 'sell') return Math.floor(Math.min(amount, atAddress));
    if (mode === 'shares') return Math.floor(amount);
    const budget = Math.floor(amount * 1e8) - (hm.status?.minerFee ?? 1000);
    if (budget <= 0) return 0;
    let lo = 0;
    let hi = Math.max(1, budget * 4);
    for (let i = 0; i < 70 && lo < hi; i++) {
      const mid = Math.ceil((lo + hi) / 2);
      if (lmsrCost(mid) <= budget) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  });

  // The node's quote, a moment after the last keystroke.
  const key = $derived(`${market.id}|${chosen}|${side}|${shares}`);
  $effect(() => {
    const k = key;
    if (!trading || shares <= 0) {
      quote = null;
      return;
    }
    const timer = setTimeout(() => {
      hmApi
        .quote(market.id, chosen, shares, side)
        .then((v) => (quote = { key: k, value: v, error: '' }))
        .catch((e) => (quote = { key: k, value: null, error: errorText(e) }));
    }, 250);
    return () => clearTimeout(timer);
  });
  const quoted = $derived(quote && quote.key === key ? quote : null);
  const q = $derived(quoted?.value ?? null);
  const bound = $derived(q ? (side === 'buy' ? maxCost(q.cost, q.minerFee, slippage) : minProceeds(q.net ?? 0, slippage)) : 0);
  const available = $derived(drop.overview?.wallet.available ?? 0);

  const problem = $derived.by(() => {
    if (!trading) return '';
    if (amountText.trim() !== '' && !valid) return t('Enter an amount above zero.');
    if (!valid) return '';
    if (side === 'sell' && holding <= 0) return t('This wallet holds no shares of this outcome to sell.');
    if (side === 'sell' && amount > atAddress) return t('That address holds %1 shares of this outcome.', formatShares(atAddress));
    if (shares <= 0) return t('That buys no share.');
    if (quoted?.error) return quoted.error;
    if (side === 'buy' && q && bound > available) return t('The order puts up %1 ECX at most; the wallet has less available.', (bound / 1e8).toFixed(8));
    return '';
  });

  function chips(): { display: string; value: string }[] {
    if (side === 'sell') return [0.25, 0.5, 1].map((f) => ({ display: f === 1 ? t('All') : `${f * 100}%`, value: String(Math.floor(atAddress * f)) }));
    return (mode === 'coins' ? [0.001, 0.01, 0.1, 1] : [1000, 10000, 100000, 1000000]).map((v) => ({ display: mode === 'coins' ? String(v) : formatShares(v), value: String(v) }));
  }

  async function place() {
    if (!q || problem) return;
    busy = true;
    error = '';
    try {
      if (side === 'buy') {
        await hmSpend.buy(market.id, chosen, shares, bound);
        toast('success', t('Order placed'), t('%1 shares, taken with the next block of Hivemind; what it does not cost comes back then.', formatShares(shares)));
      } else {
        await hmSpend.sell(market.id, chosen, shares, address, bound);
        toast('success', t('Order placed'), t('%1 shares sold with the next block of Hivemind; the proceeds come in it.', formatShares(shares)));
      }
      amountText = '';
      await refresh();
      void refreshHivemind();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel trade" aria-label={t('Trade')}>
  <div class="sides" role="tablist" aria-label={t('Buy or sell')}>
    <button type="button" role="tab" aria-selected={side === 'buy'} class="buy" onclick={() => (side = 'buy')}>{t('Buy')}</button>
    <button type="button" role="tab" aria-selected={side === 'sell'} class="sell" onclick={() => (side = 'sell')}>{t('Sell')}</button>
  </div>

  {#if !trading}
    <p class="hm-help closed">{t('The market has paid out: every share got what its outcome was worth.')}</p>
  {:else}
    <div class="body">
      <span class="caps">{t('Outcome')}</span>
      <div class="pick" role="radiogroup" aria-label={t('Outcome')}>
        {#each market.outcomes as o (o.index)}
          <button type="button" role="radio" aria-checked={chosen === o.index} class="opt" onclick={() => (chosen = o.index)}>
            <span class="hm-dot hm-c{colour(market, o)}"></span>
            <span class="name"><SafeText text={outcomeName(market, o)} /></span>
            <span class="num faint">{formatChance(o.price)}</span>
          </button>
        {/each}
      </div>

      {#if side === 'buy'}
        <div class="modes" role="group" aria-label={t('Amount in')}>
          <button type="button" class="hm-chip" aria-pressed={mode === 'coins'} onclick={() => (mode = 'coins')}>{t('ECX to spend')}</button>
          <button type="button" class="hm-chip" aria-pressed={mode === 'shares'} onclick={() => (mode = 'shares')}>{t('Shares to buy')}</button>
        </div>
      {:else if (mine?.addresses.length ?? 0) > 1}
        <label class="field">
          <span>{t('From the address')}</span>
          <select bind:value={address}>
            {#each mine?.addresses ?? [] as a (a.address)}<option value={a.address} translate="no">{a.address.slice(0, 14)}… · {formatShares(a.shares)}</option>{/each}
          </select>
        </label>
      {/if}

      <label class="field">
        <span>{side === 'sell' ? t('Shares to sell') : mode === 'coins' ? t('ECX to spend') : t('Shares to buy')}</span>
        <input id="trade-amount" type="text" inputmode="decimal" autocomplete="off" placeholder={side === 'buy' && mode === 'coins' ? '0.00000000' : '0'} bind:value={amountText} />
      </label>
      <div class="hm-chips">
        {#each chips() as c (c.display)}<button type="button" class="hm-chip" onclick={() => (amountText = c.value)}>{c.display}</button>{/each}
      </div>
      <p class="hm-help">{holding > 0 ? t('You hold %1 shares of this outcome.', formatShares(holding)) : t('You hold no shares of this outcome.')}</p>

      <div class="slip">
        <span class="caps">{t('Slippage')}</span>
        <div class="hm-chips" role="group" aria-label={t('Slippage')}>
          {#each SLIPPAGES as s (s)}<button type="button" class="hm-chip" aria-pressed={slippage === s} onclick={() => (slippage = s)}>{s * 100}%</button>{/each}
        </div>
      </div>

      {#if q}
        <dl class="facts preview" aria-label={t('The node’s quote')}>
          <dt>{t('Shares')}</dt><dd class="num">{formatShares(shares)}</dd>
          <dt>{t('Chance')}</dt><dd class="num">{formatChance(q.priceBefore)} → {formatChance(q.priceAfter)}</dd>
          <dt>{t('Trading fee')}</dt><dd><Amount sats={q.tradingFee} /></dd>
          <dt>{t('Miner fee')}</dt><dd><Amount sats={q.minerFee} /></dd>
          {#if side === 'buy'}
            <dt class="strong">{t('You pay')}</dt><dd class="strong"><Amount sats={q.cost + q.minerFee} /></dd>
            <dt>{t('At most, with slippage')}</dt><dd><Amount sats={bound} /></dd>
            <dt>{t('Pays if right')}</dt><dd><Amount sats={shares} /></dd>
          {:else}
            <dt class="strong">{t('You get')}</dt><dd class="strong"><Amount sats={q.net ?? 0} /></dd>
            <dt>{t('At least, with slippage')}</dt><dd><Amount sats={bound} /></dd>
          {/if}
        </dl>
        <p class="hm-help quote">{t('Quoted by the node just now; the block that takes the trade sets the price.')}</p>
      {/if}

      {#if problem}<p class="hm-problems" role="alert"><SafeText text={problem} /></p>{/if}
      {#if error}<p class="hm-problems" role="alert"><SafeText text={error} /></p>{/if}

      <button type="button" id="trade-place" class="btn primary place" disabled={busy || !q || !!problem || !drop.seed?.hasSeed} onclick={place}>
        <Icon name={side === 'buy' ? 'plus' : 'up'} size={16} />
        {side === 'buy' ? t('Review and buy…') : t('Review and sell…')}
      </button>
      <p class="hm-help">{t('A share pays 1 sat (0.00000001 ECX) if its outcome comes true, nothing if not.')}</p>
    </div>
  {/if}
</section>

<style>
  .trade {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .sides {
    display: grid;
    grid-template-columns: 1fr 1fr;
    border-bottom: 1px solid var(--line);
  }
  .sides button {
    padding: 0.75rem;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 650;
    cursor: pointer;
  }
  .sides button[aria-selected='true'].buy {
    color: var(--text);
    border-bottom-color: var(--positive);
  }
  .sides button[aria-selected='true'].sell {
    color: var(--text);
    border-bottom-color: var(--negative);
  }
  .body {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 1rem;
  }
  .closed {
    padding: 1rem;
  }
  .caps {
    font-size: 0.75rem;
    font-weight: 650;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-muted);
  }
  .pick {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    max-height: 14rem;
    overflow-y: auto;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.45rem 0.65rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .opt[aria-checked='true'] {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .opt .name {
    flex: 1;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .modes {
    display: flex;
    gap: 0.35rem;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .field > span {
    font-size: 0.85rem;
    color: var(--text-muted);
  }
  .slip {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.4rem;
  }
  .preview {
    display: grid;
    grid-template-columns: 1fr auto;
    gap: 0.3rem 0.8rem;
    margin: 0;
    padding: 0.7rem 0.8rem;
    border-radius: var(--radius-m);
    background: var(--surface-2);
    font-size: 0.88rem;
  }
  .preview dt {
    color: var(--text-muted);
  }
  .preview dd {
    margin: 0;
    text-align: right;
  }
  .preview .strong {
    font-weight: 700;
    color: var(--text);
  }
  .quote {
    margin-top: -0.4rem;
    font-size: 0.78rem;
  }
  .place {
    justify-content: center;
    min-height: 2.6rem;
    font-size: 1rem;
  }
</style>
