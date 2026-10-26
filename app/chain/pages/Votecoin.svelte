<script lang="ts">
  // Votecoin, a tab of the voting center: the reputation that votes on the decisions. What this wallet holds (at its
  // voter address, the first address of the wallet; any other address of it that holds some), moving it to another
  // address (transfer_votecoin: its fee paid by a coin of the voter address, the chain's replay protection), and every
  // holder. Voting with the consensus keeps and earns Votecoin; against it loses some.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import AddressField from '../dropin/AddressField.svelte';
  import { refresh } from '../dropin/store.svelte';
  import { hmApi, hmSpend } from '../hm/api';
  import { formatChance, formatVotecoin, type Holder } from '../hm/model';
  import { hm, refreshHivemind } from '../hm/store.svelte';

  const FEE = 1000;
  let holders = $state<Holder[]>([]);
  $effect(() => {
    void hm.version;
    hmApi
      .holders()
      .then((h) => (holders = [...h].sort((a, b) => b.votecoin_balance - a.votecoin_balance)))
      .catch(() => (holders = []));
  });
  const vc = $derived(hm.votecoin);
  const total = $derived(holders.reduce((n, h) => n + h.votecoin_balance, 0));
  const mine = $derived(new Set([vc?.voter ?? '', ...(vc?.addresses.map((a) => a.address) ?? [])]));

  let to = $state('');
  let toValid = $state(false);
  let amountText = $state('');
  let busy = $state(false);
  let error = $state('');
  const amount = $derived(Number(amountText.replace(',', '.')));
  const problem = $derived.by(() => {
    if (!amountText.trim()) return '';
    if (!Number.isFinite(amount) || !(amount > 0)) return t('Votecoin to move: more than nothing.');
    if (amount > (vc?.votecoin ?? 0)) return t('Your voter address holds %1.', formatVotecoin(vc?.votecoin ?? 0));
    if (Math.abs(Number(amount.toFixed(10)) - amount) > 0) return t('At most 10 decimals.');
    if ((vc?.voterCoins ?? 0) < FEE) return t('The fee is paid from the voter address: send it a little ECX first.');
    return '';
  });

  async function move() {
    if (problem || !toValid || !(amount > 0)) return;
    busy = true;
    error = '';
    try {
      await hmSpend.moveVotecoin(to.trim(), Number(amount.toFixed(10)), FEE);
      toast('success', t('Votecoin moved'), t('With the next block of Hivemind.'));
      amountText = '';
      to = '';
      await refresh();
      void refreshHivemind();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="votecoin">
  <section class="hm-figures">
    <div class="panel hm-figure"><span class="muted">{t('At your voter address')}</span><strong class="num">{formatVotecoin(vc?.votecoin ?? 0)}</strong><span class="faint small">{total > 0 ? t('%1 of all Votecoin', formatChance((vc?.votecoin ?? 0) / total)) : t('Votecoin')}</span></div>
    <div class="panel hm-figure"><span class="muted">{t('Holders')}</span><strong class="num">{holders.length}</strong><span class="faint small">{t('%1 in all', formatVotecoin(total))}</span></div>
    <div class="panel hm-figure"><span class="muted">{t('ECX at the voter address')}</span><strong><Amount sats={vc?.voterCoins ?? 0} /></strong><span class="faint small">{t('pays the fee of a move')}</span></div>
  </section>

  <div class="cols">
    <section class="panel">
      <div class="panel-head"><h2>{t('Your voter address')}</h2></div>
      <div class="panel-body stack">
        {#if vc?.voter}<div class="row addr"><span class="mono break" translate="no">{vc.voter}</span><CopyButton text={vc.voter} label={t('Copy the voter address')} /></div>{/if}
        <p class="hm-help">{t('The first address of this wallet: its Votecoin casts the wallet’s ballots. Votecoin sent to it counts at once.')}</p>
        {#if vc?.addresses.length}
          <p class="hm-help">{t('Other addresses of this wallet hold Votecoin too: they do not vote from this app.')}</p>
          <ul class="others">{#each vc.addresses as a (a.address)}<li><span class="mono cut" translate="no">{a.address}</span><span class="num">{formatVotecoin(a.votecoin)}</span></li>{/each}</ul>
        {/if}
      </div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>{t('Move Votecoin')}</h2></div>
      <form
        class="panel-body stack"
        onsubmit={(e) => {
          e.preventDefault();
          move();
        }}
      >
        <AddressField id="vc-to" label={t('To a Hivemind address')} bind:value={to} bind:valid={toValid} placeholder="4XrMw8bmnhchFspSyP4XRUuB5vaR" />
        <label class="field"><span>{t('Votecoin')}</span><input id="vc-amount" type="text" inputmode="decimal" placeholder="0.25" bind:value={amountText} /></label>
        <p class="hm-help">{t('Fee')} <Amount sats={FEE} />, {t('paid from the voter address.')}</p>
        {#if problem}<p class="hm-problems" role="alert"><SafeText text={problem} /></p>{/if}
        {#if error}<p class="hm-problems" role="alert"><SafeText text={error} /></p>{/if}
        <div class="row"><button type="submit" class="btn primary" disabled={busy || !toValid || !!problem || !amountText.trim()}>{t('Review and move…')}</button></div>
      </form>
    </section>
  </div>

  <section class="panel">
    <div class="panel-head"><h2>{t('Every holder')}</h2></div>
    {#if holders.length === 0}
      <p class="hm-empty">{t('Nobody holds Votecoin yet: the first block of the chain gives it all to the voter of its producer.')}</p>
    {:else}
      <div class="hm-wrap">
        <table class="hm-table" use:stacked aria-label={t('Holders of Votecoin')}>
          <thead><tr><th>{t('Address')}</th><th class="end">{t('Votecoin')}</th><th class="end">{t('Share')}</th><th class="end">{t('Votes')}</th></tr></thead>
          <tbody>
            {#each holders.slice(0, 50) as h (h.address)}
              <tr>
                <td class="cut mono"><span translate="no">{h.address}</span>{#if mine.has(h.address)} <span class="badge accent">{t('yours')}</span>{/if}</td>
                <td class="end num">{formatVotecoin(h.votecoin_balance)}</td>
                <td class="end num">{formatChance(total > 0 ? h.votecoin_balance / total : 0)}</td>
                <td class="end num">{h.total_votes}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>
</div>

<style>
  .votecoin {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 1rem;
    align-items: start;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .addr {
    align-items: center;
    gap: 0.4rem;
  }
  .break {
    word-break: break-all;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .field > span {
    font-size: 0.88rem;
    color: var(--text-muted);
  }
  .others {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .others li {
    display: flex;
    justify-content: space-between;
    gap: 1rem;
  }
  .cut {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
