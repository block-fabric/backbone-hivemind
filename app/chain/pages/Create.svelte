<script lang="ts">
  // Create, a tab of Markets: open a market (on a new decision listed with it, or on decisions already listed), or list
  // a decision alone. A decision takes a slot of a period (its listing fee: the cheapest open one is shown) and is voted
  // on by the holders of Votecoin in the next period; its market trades until the block that resolves its decisions,
  // which pays it out. Each asked in the app's trusted dialog (market_create, decision_claim), with LayerTwo's request.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, ownTab } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { displayTextProblem } from '../dropin/text';
  import { drop, refresh } from '../dropin/store.svelte';
  import '../hm/hm.css';
  import { hmApi, hmSpend } from '../hm/api';
  import { claimEntry, decisionProblems, type DecisionDraft, type OpenPeriod } from '../hm/model';
  import { hm, refreshHivemind } from '../hm/store.svelte';

  const CATEGORIES = ['Crypto', 'Economy', 'Politics', 'Sports', 'Science', 'Weather', 'Culture', 'Other'];
  const TX_FEE = 1000;

  const mode = $derived(ownTab('create').startsWith('decision') ? 'decision' : 'market');
  let source = $state<'new' | 'listed'>('new');
  let picked = $state<string[]>([]);
  let draft = $state<DecisionDraft>({ kind: 'binary', header: '', description: '', period: 0, min: '', max: '', increment: '', options: ['', ''], noLabel: '', yesLabel: '' });
  let title = $state('');
  let titleTouched = $state(false);
  let description = $state('');
  let category = $state('');
  let liquidity = $state('0.001');
  let feePct = $state('0.5');
  let busy = $state(false);
  let error = $state('');
  let periods = $state<OpenPeriod[]>([]);

  $effect(() => {
    void hm.version;
    hmApi
      .openPeriods()
      .then((p) => (periods = p))
      .catch(() => (periods = []));
  });
  const current = $derived(hm.status?.period.current ?? 0);
  $effect(() => {
    if (draft.period < current) draft.period = periods[0]?.period_index ?? current;
  });
  const pricing = $derived(periods.find((p) => p.period_index === draft.period) ?? null);
  // The market's title follows the question until the user writes one.
  $effect(() => {
    if (!titleTouched && source === 'new') title = draft.header;
  });

  const listed = $derived(hm.decisions.filter((d) => d.state === 'claimed').sort((a, b) => a.period - b.period || a.index - b.index));
  const withNew = $derived(mode === 'decision' || source === 'new');
  const listing = $derived(withNew ? (pricing?.cheapest_available_slot_sats ?? 0) : 0);
  /** The most the listing may cost (the cheapest slot may be taken before the claim's block: room for the next tier). */
  const cap = $derived(listing * 2);
  const liquiditySats = $derived(Math.round(Number(liquidity.replace(',', '.')) * 1e8));

  const problems = $derived.by(() => {
    const out: string[] = [];
    if (withNew) {
      out.push(...decisionProblems(draft, current));
      if (!pricing) out.push(t('No slot is open in period %1: choose another period.', String(draft.period)));
    } else if (picked.length === 0) out.push(t('Choose the decisions the market is about.'));
    if (mode === 'market') {
      if (!title.trim()) out.push(t('Give the market a title.'));
      const odd = displayTextProblem(title) || displayTextProblem(description.replace(/\n/g, ' '));
      if (odd) out.push(t(odd));
      if (!(liquiditySats >= 1000)) out.push(t('The liquidity is at least 0.00001 ECX.'));
      const f = Number(feePct.replace(',', '.'));
      if (!(f >= 0 && f < 100)) out.push(t('The trading fee is from 0% to under 100%.'));
      if (!withNew && picked.length > 4) out.push(t('A market is about 4 decisions at most here.'));
    }
    return [...new Set(out)];
  });

  function toggle(id: string) {
    picked = picked.includes(id) ? picked.filter((x) => x !== id) : [...picked, id];
  }

  function reset() {
    draft = { kind: 'binary', header: '', description: '', period: current, min: '', max: '', increment: '', options: ['', ''], noLabel: '', yesLabel: '' };
    title = '';
    titleTouched = false;
    description = '';
    picked = [];
  }

  async function submit() {
    if (problems.length) return;
    busy = true;
    error = '';
    try {
      if (mode === 'decision') {
        const req: Record<string, unknown> = { decision_type: draft.kind, decisions: [claimEntry(draft, false)], tx_fee_sats: TX_FEE, max_listing_fee_sats: cap };
        if (draft.kind === 'scaled') {
          req.min = Number(draft.min);
          req.max = Number(draft.max);
          req.increment = draft.increment.trim() === '' ? 1 : Number(draft.increment);
        }
        const r = await hmSpend.claim(req);
        toast('success', t('Decision listed'), t('In the next block of Hivemind: %1', r.decision_ids.join(', ')));
      } else {
        const fullTitle = (category ? `[${category}] ` : '') + title.trim();
        const dims = withNew ? [claimEntry(draft, true)] : picked.map((id) => ({ type: 'existing', id }));
        const req: Record<string, unknown> = {
          title: fullTitle,
          description: description.trim(),
          dimensions: dims,
          trading_fee: Number(feePct.replace(',', '.')) / 100,
          initial_liquidity: liquiditySats,
          tx_fee_sats: TX_FEE,
        };
        if (withNew) req.max_listing_fee_sats = cap;
        const r = await hmSpend.createMarket(req);
        toast('success', t('Market opened'), t('It trades from the next block of Hivemind.'));
        reset();
        await refresh();
        await refreshHivemind();
        go('markets', `m-${r.market_id}`);
        return;
      }
      reset();
      await refresh();
      void refreshHivemind();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="create">
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    {#if mode === 'market'}
      <section class="panel">
        <div class="panel-head"><h2>{t('What the market is about')}</h2></div>
        <div class="panel-body stack">
          <div class="hm-chips" role="radiogroup" aria-label={t('Decisions of the market')}>
            <button type="button" role="radio" class="hm-chip" aria-checked={source === 'new'} onclick={() => (source = 'new')}>{t('A new decision, listed with it')}</button>
            <button type="button" role="radio" class="hm-chip" aria-checked={source === 'listed'} onclick={() => (source = 'listed')}>{t('Decisions already listed')} <span class="count">{listed.length}</span></button>
          </div>
          {#if source === 'listed'}
            {#if listed.length === 0}
              <p class="hm-empty">{t('No decision is listed and open yet: list one with the market.')}</p>
            {:else}
              <ul class="picks">
                {#each listed as d (d.id)}
                  <li>
                    <label class="pickrow" class:on={picked.includes(d.id)}>
                      <input type="checkbox" checked={picked.includes(d.id)} onchange={() => toggle(d.id)} />
                      <span class="grow"><SafeText text={d.header} /></span>
                      <span class="badge">{d.type.kind === 'binary' ? t('Yes or no') : d.type.kind === 'scaled' ? t('Scale') : t('%1 options', d.type.options?.length ?? 0)}</span>
                      <span class="faint small">{t('voted on in period %1', d.votingPeriod)}</span>
                    </label>
                  </li>
                {/each}
              </ul>
              <p class="hm-help">{t('Several decisions make a market on their combinations: an outcome for each.')}</p>
            {/if}
          {/if}
        </div>
      </section>
    {/if}

    {#if withNew}
      <section class="panel">
        <div class="panel-head"><h2>{t('The question')}</h2></div>
        <div class="panel-body stack">
          <div class="kinds" role="radiogroup" aria-label={t('Kind of answer')}>
            <button type="button" role="radio" class="kind" aria-checked={draft.kind === 'binary'} onclick={() => (draft.kind = 'binary')}><strong>{t('Yes or no')}</strong><span class="muted small">{t('Will it happen?')}</span></button>
            <button type="button" role="radio" class="kind" aria-checked={draft.kind === 'scaled'} onclick={() => (draft.kind = 'scaled')}><strong>{t('A number')}</strong><span class="muted small">{t('How much, on a scale you set')}</span></button>
            <button type="button" role="radio" class="kind" aria-checked={draft.kind === 'category'} onclick={() => (draft.kind = 'category')}><strong>{t('One of several options')}</strong><span class="muted small">{t('Who or which, of a list')}</span></button>
          </div>
          <label class="field"><span>{t('Question')}</span><input id="create-question" type="text" maxlength="100" placeholder={t('Will it rain in Lisbon on 1 December?')} bind:value={draft.header} /></label>
          <label class="field"><span>{t('How it is decided (optional)')}</span><textarea id="create-description" rows="2" maxlength="2000" bind:value={draft.description}></textarea></label>
          {#if draft.kind === 'binary'}
            <div class="pair">
              <label class="field"><span>{t('Word for yes (optional)')}</span><input type="text" maxlength="40" placeholder={t('Yes')} bind:value={draft.yesLabel} /></label>
              <label class="field"><span>{t('Word for no (optional)')}</span><input type="text" maxlength="40" placeholder={t('No')} bind:value={draft.noLabel} /></label>
            </div>
          {:else if draft.kind === 'scaled'}
            <div class="pair three">
              <label class="field"><span>{t('Low end')}</span><input id="create-min" type="text" inputmode="decimal" bind:value={draft.min} /></label>
              <label class="field"><span>{t('High end')}</span><input id="create-max" type="text" inputmode="decimal" bind:value={draft.max} /></label>
              <label class="field"><span>{t('Step')}</span><input type="text" inputmode="decimal" placeholder="1" bind:value={draft.increment} /></label>
            </div>
          {:else}
            <div class="options">
              {#each draft.options as _, i (i)}
                <div class="opt">
                  <span class="hm-dot hm-c{(i + 1) % 8}"></span>
                  <input type="text" maxlength="60" placeholder={t('Option %1', i + 1)} bind:value={draft.options[i]} />
                  {#if draft.options.length > 2}<button type="button" class="btn ghost small" aria-label={t('Remove this option')} onclick={() => (draft.options = draft.options.filter((_, j) => j !== i))}><Icon name="x" size={14} /></button>{/if}
                </div>
              {/each}
              {#if draft.options.length < 16}<button type="button" class="btn small" onclick={() => (draft.options = [...draft.options, ''])}><Icon name="plus" size={14} /> {t('Another option')}</button>{/if}
            </div>
          {/if}
          <label class="field">
            <span>{t('Period its answer is known in')}</span>
            <span class="inline">
              <select id="create-period" class="pill" bind:value={draft.period}>
                {#each periods as p (p.period_index)}<option value={p.period_index}>{t('Period %1', p.period_index)}{p.period_index === current ? ` (${t('now')})` : ''}</option>{/each}
                {#if periods.length === 0}<option value={current}>{t('Period %1', current)}</option>{/if}
              </select>
              {#if pricing}<span class="muted small">{t('listing fee from')} <Amount sats={pricing.cheapest_available_slot_sats} /></span>{/if}
            </span>
            <span class="hm-help">{t('Voted on in period %1 by the holders of Votecoin; resolved by the first block after it.', draft.period + 1)}</span>
          </label>
        </div>
      </section>
    {/if}

    {#if mode === 'market'}
      <section class="panel">
        <div class="panel-head"><h2>{t('The market')}</h2></div>
        <div class="panel-body stack">
          <label class="field"><span>{t('Title')}</span><input id="create-title" type="text" maxlength="400" bind:value={title} oninput={() => (titleTouched = true)} /></label>
          <label class="field"><span>{t('Description (optional)')}</span><textarea rows="2" maxlength="2000" bind:value={description}></textarea></label>
          <label class="field">
            <span>{t('Category')}</span>
            <span class="inline">
              <select class="pill" bind:value={category} aria-label={t('Category')}>
                <option value="">{t('None')}</option>
                {#each CATEGORIES as c (c)}<option value={c}>{t(c)}</option>{/each}
              </select>
              <span class="faint small">{t('Written at the start of the title, for the filters of the markets.')}</span>
            </span>
          </label>
          <div class="pair">
            <label class="field">
              <span>{t('Liquidity, ECX')}</span>
              <input id="create-liquidity" type="text" inputmode="decimal" bind:value={liquidity} />
              <span class="hm-help">{t('The larger, the less a trade moves the chances. It funds the market maker; what is left comes back to you when it settles.')}</span>
            </label>
            <label class="field">
              <span>{t('Trading fee, %')}</span>
              <input id="create-fee" type="text" inputmode="decimal" bind:value={feePct} />
              <span class="hm-help">{t('Taken from every trade: half to the voters who vote with the consensus, a quarter to those who listed the decisions, a quarter to you.')}</span>
            </label>
          </div>
        </div>
      </section>
    {/if}

    <section class="panel">
      <div class="panel-head"><h2>{t('Review')}</h2></div>
      <div class="panel-body stack">
        <dl class="facts">
          {#if mode === 'market'}<dt>{t('Market')}</dt><dd><SafeText text={(category ? `[${category}] ` : '') + (title.trim() || '–')} /></dd>{/if}
          {#if withNew}
            <dt>{t('Decision')}</dt><dd><SafeText text={draft.header.trim() || '–'} /></dd>
            <dt>{t('Known in')}</dt><dd>{t('period %1, voted on in period %2', draft.period, draft.period + 1)}</dd>
            <dt>{t('Listing fee')}</dt><dd><Amount sats={listing} /> <span class="faint small">{t('(at most %1 ECX)', (cap / 1e8).toFixed(8))}</span></dd>
          {:else}
            <dt>{t('Decisions')}</dt><dd>{#each picked as id (id)}<div><SafeText text={hm.decisions.find((d) => d.id === id)?.header ?? id} /></div>{/each}</dd>
          {/if}
          {#if mode === 'market'}<dt>{t('Liquidity')}</dt><dd><Amount sats={Number.isFinite(liquiditySats) ? liquiditySats : 0} /></dd>{/if}
          <dt>{t('Fee')}</dt><dd><Amount sats={TX_FEE} /></dd>
        </dl>
        {#if problems.length && (draft.header || title || picked.length)}
          <ul class="hm-problems" role="alert">{#each problems as p (p)}<li><SafeText text={p} /></li>{/each}</ul>
        {/if}
        {#if error}<p class="hm-problems" role="alert"><SafeText text={error} /></p>{/if}
        <div class="row">
          <button type="button" class="btn primary" id="create-submit" disabled={busy || problems.length > 0} onclick={submit}><Icon name="check" size={15} /> {mode === 'market' ? t('Review and open the market…') : t('Review and list the decision…')}</button>
          <button type="button" class="btn ghost" onclick={reset}>{t('Start again')}</button>
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .create {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    max-width: 56rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
  .kinds {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(13rem, 100%), 1fr));
    gap: 0.7rem;
  }
  .kind {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    padding: 0.9rem 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .kind[aria-checked='true'] {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    min-width: 0;
  }
  .field > span:first-child {
    font-size: 0.88rem;
    color: var(--text-muted);
  }
  .inline {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
  }
  .pair {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(14rem, 100%), 1fr));
    gap: 0.8rem;
  }
  .pair.three {
    grid-template-columns: repeat(auto-fit, minmax(min(9rem, 100%), 1fr));
  }
  .options {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    align-items: flex-start;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    width: 100%;
    max-width: 30rem;
  }
  .opt input {
    flex: 1;
  }
  .picks {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 20rem;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .pickrow {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    padding: 0.5rem 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    cursor: pointer;
  }
  .pickrow.on {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .grow {
    flex: 1;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .facts {
    display: grid;
    grid-template-columns: max-content minmax(0, 1fr);
    gap: 0.35rem 1rem;
    margin: 0;
    font-size: 0.92rem;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
