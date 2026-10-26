// Hivemind's own pages over a pretend node: every spend only through rpc.confirmed with LayerTwo's method and its
// request (market_buy with the most it pays, market_sell from the address that holds the shares, market_create,
// decision_claim, transfer_votecoin), the ballot as their single vote_submit (no seal step: the pages map our voting
// center's seal-then-reveal onto it), the result bars of a market that paid out, and no request outside the manifest.
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { answerValue, decisionProblems, formatVotecoin, maxCost, minProceeds, resultOf, type Decision, type Market } from './hm/model';

const manifest = JSON.parse(readFileSync(join(__dirname, 'manifest.json'), 'utf8')) as { reads: string[]; confirmed: string[]; host: string[] };
const APP_METHODS = new Set(['app.hello', 'app.info', 'app.locale', 'app.languages', 'prefs.get', 'prefs.set', 'node.state', 'rpc.confirmed', 'rpc.confirmedMethods', 'window.state', 'clipboard.write', 'app.notify']);
const ADDR = '4XrMw8bmnhchFspSyP4XRUuB5vaR';
// The charts measure their width (bind:clientWidth): jsdom has no ResizeObserver.
globalThis.ResizeObserver ??= class {
  observe() {}
  unobserve() {}
  disconnect() {}
} as unknown as typeof ResizeObserver;
const OTHER = '4TXCaVb28BDdZbpF93Gg4GDMhxd9';

const binary = { kind: 'binary', noLabel: 'No', yesLabel: 'Yes' } as const;
const decision = (over: Partial<Decision> = {}): Decision => ({
  id: '800001',
  period: 1,
  index: 1,
  votingPeriod: 2,
  standard: true,
  header: 'Will it rain?',
  description: '',
  tags: [],
  type: { ...binary },
  author: ADDR,
  claimTxid: '00'.repeat(32),
  state: 'claimed',
  markets: ['0102030405ab'],
  ...over,
});
const market = (over: Partial<Market> = {}): Market => ({
  id: '0102030405ab',
  title: '[Weather] Rain in Lisbon',
  description: 'binary market',
  tags: [],
  creator: ADDR,
  createdHeight: 3,
  expiresHeight: null,
  state: 'trading',
  updatedHeight: 4,
  beta: 144269.5,
  liquidity: 100000,
  treasury: 100000,
  fees: 0,
  volume: 25000,
  feeRate: 0.005,
  txPow: false,
  decisions: [{ id: '800001', header: 'Will it rain?', type: { ...binary }, state: 'claimed', votingPeriod: 2 }],
  outcomes: [
    { index: 0, label: 'No', parts: ['No'], coordinates: [0], price: 0.4, shares: 0, volume: 0 },
    { index: 1, label: 'Yes', parts: ['Yes'], coordinates: [1], price: 0.6, shares: 40000, volume: 25000 },
  ],
  history: [{ height: 3, block_hash: 'aa'.repeat(32), timestamp: 0, prices: [0.5, 0.5] }],
  holders: [],
  ...over,
});

let calls: BridgeRequest[] = [];
let answers: Record<string, (r: BridgeRequest) => unknown> = {};
let markets: Market[] = [];
let decisions: Decision[] = [];

beforeEach(async () => {
  calls = [];
  markets = [market()];
  decisions = [decision()];
  answers = {
    'dropin.overview': () => ({ wallet: { hasSeed: true, available: 250_000_000, withdrawing: 0, pendingIn: 0, pendingOut: 0 }, chain: { height: 5, peers: 0, slot: 13 }, mainchain: { slot: 13, connected: true, state: 'ok' } }),
    'dropin.seed.status': () => ({ hasSeed: true, phraseKept: true, from: 'app', fingerprint: '00', derivation: 'truthcoin 0.20.0' }),
    'dropin.checkAddress': (r) => ({ valid: [ADDR, OTHER].includes((r.params as { address: string }).address) }),
    'hivemind.status': () => ({ period: { current: 2, name: 'P2', unit: 'blocks', length: 6, start: 12, end: 18, now: 14, height: 14 }, counts: { markets: 1, trading: 1, settled: 0, decisions: 1, voting: 1, voters: 1 }, votecoinTotal: 1, minerFee: 1000 }),
    'hivemind.markets': () => markets,
    'hivemind.market': (r) => markets.find((m) => m.id === (r.params as { id: string }).id),
    'hivemind.decisions': () => decisions,
    'hivemind.quote': (r) => {
      const p = r.params as { shares: number; side: string };
      return p.side === 'buy'
        ? { side: 'buy', shares: p.shares, cost: 6200, tradingFee: 31, minerFee: 1000, priceBefore: 0.6, priceAfter: 0.62 }
        : { side: 'sell', shares: p.shares, cost: 5900, tradingFee: 29, net: 5871, minerFee: 1000, priceBefore: 0.6, priceAfter: 0.58 };
    },
    'hivemind.positions': () => [{ market: '0102030405ab', outcome: 1, address: OTHER, shares: 40000, price: 0.6, value: 24000 }],
    'hivemind.votecoin': () => ({ voter: ADDR, votecoin: 1, voterCoins: 100_000, addresses: [] }),
    'hivemind.ballots': () => [],
    'hivemind.payouts': () => [],
    'rpc:list_open_periods_with_pricing': () => [{ period_index: 2, cheapest_available_slot_sats: 10_000, cheapest_available_tier: 0, slots_available_by_tier: [100, 0, 0, 0, 0] }],
    'rpc:vote_voters': () => [{ address: ADDR, votecoin_balance: 1, total_votes: 0, is_active: false }],
    'rpc.confirmed': () => ({ txid: 'cd'.repeat(32), market_id: '0102030405ab', decision_ids: ['820001'] }),
  };
  setTransport(async (req) => {
    calls.push(req);
    const answer = answers[req.kind === 'host' ? (req.method ?? '') : `rpc:${req.method}`];
    return answer ? answer(req) : null;
  });
  const { refresh } = await import('./dropin/store.svelte');
  await refresh();
  const { resetHivemind, refreshHivemind } = await import('./hm/store.svelte');
  resetHivemind();
  await refreshHivemind();
});
afterEach(() => setTransport(null));

const confirmedCalls = () => calls.filter((c) => c.kind === 'host' && c.method === 'rpc.confirmed').map((c) => c.params as { method: string; params: unknown[] });
const button = (root: HTMLElement, words: string) => [...root.querySelectorAll('button')].find((b) => b.textContent?.trim().startsWith(words)) as HTMLButtonElement;

describe('Hivemind pages', () => {
  it('a purchase is market_buy [{market_id, outcome_index, shares_amount, max_cost}]: the node quotes it, the order pays at most the quote with the slippage', async () => {
    const MarketView = (await import('./hm/MarketView.svelte')).default;
    const { container } = render(MarketView, { props: { id: '0102030405ab', back: () => undefined } });
    await waitFor(() => expect(container.querySelector('#trade-amount')).toBeTruthy());
    await fireEvent.click(button(container, 'Shares to buy'));
    await fireEvent.input(container.querySelector('#trade-amount') as HTMLInputElement, { target: { value: '10000' } });
    const place = container.querySelector('#trade-place') as HTMLButtonElement;
    await waitFor(() => expect(place.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(place);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    const outcome = (calls.find((c) => c.method === 'hivemind.quote')?.params as { outcome: number }).outcome;
    expect(confirmedCalls()[0]).toEqual({ method: 'market_buy', params: [{ market_id: '0102030405ab', outcome_index: outcome, shares_amount: 10000, max_cost: maxCost(6200, 1000, 0.01) }] });
  });

  it('a sale is market_sell from the address that holds the shares, at least the quote less the slippage', async () => {
    const MarketView = (await import('./hm/MarketView.svelte')).default;
    const { container } = render(MarketView, { props: { id: '0102030405ab', back: () => undefined } });
    await waitFor(() => expect(container.querySelector('.sides .sell')).toBeTruthy());
    // The outcome held (Yes, index 1), then the Sell side.
    await fireEvent.click([...container.querySelectorAll('.opt')].find((b) => b.textContent?.includes('Yes')) as HTMLElement);
    await fireEvent.click(container.querySelector('.sides .sell') as HTMLElement);
    await fireEvent.input(container.querySelector('#trade-amount') as HTMLInputElement, { target: { value: '15000' } });
    const place = container.querySelector('#trade-place') as HTMLButtonElement;
    await waitFor(() => expect(place.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(place);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({ method: 'market_sell', params: [{ market_id: '0102030405ab', outcome_index: 1, shares_amount: 15000, seller_address: OTHER, min_proceeds: minProceeds(5871, 0.01) }] });
  });

  it('the ballot is their single vote_submit [[{decision_id, vote_value}], fee]: answered decisions cast together, no seal step', async () => {
    decisions = [
      decision({ state: 'voting' }),
      decision({ id: '800002', index: 2, header: 'Who wins?', type: { kind: 'category', options: ['A', 'B', 'C'] } }),
      decision({ id: '800003', index: 3, header: 'How warm?', type: { kind: 'scaled', min: -20, max: 40, increment: 0.5 } }),
    ].map((d) => ({ ...d, state: 'voting' as const }));
    const { refreshHivemind } = await import('./hm/store.svelte');
    await refreshHivemind();
    const Voting = (await import('./pages/Voting.svelte')).default;
    const { container } = render(Voting);
    await waitFor(() => expect(container.querySelectorAll('.ballot').length).toBe(3));
    expect(container.textContent).toContain('there is no sealed vote to reveal later');
    const cards = [...container.querySelectorAll('.ballot')] as HTMLElement[];
    await fireEvent.click(button(cards[0], 'Yes'));
    await fireEvent.click(button(cards[1], 'B'));
    await fireEvent.input(cards[2].querySelector('input') as HTMLInputElement, { target: { value: '21.5' } });
    const cast = container.querySelector('#vote-cast') as HTMLButtonElement;
    await waitFor(() => expect(cast.disabled).toBe(false));
    await fireEvent.click(cast);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({
      method: 'vote_submit',
      params: [[{ decision_id: '800001', vote_value: 1 }, { decision_id: '800002', vote_value: 1 }, { decision_id: '800003', vote_value: 21.5 }], 1000],
    });
    expect(calls.some((c) => /seal|reveal/i.test(c.method ?? ''))).toBe(false);
  });

  it('a market on a new decision is market_create with the decision as a "new" dimension', async () => {
    const { go } = await import('../lib/nav.svelte');
    go('create', 'market');
    const Create = (await import('./pages/Create.svelte')).default;
    const { container } = render(Create);
    await waitFor(() => expect(container.querySelector('#create-question')).toBeTruthy());
    await fireEvent.input(container.querySelector('#create-question') as HTMLInputElement, { target: { value: 'Will it rain?' } });
    const submit = container.querySelector('#create-submit') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({
      method: 'market_create',
      params: [{ title: 'Will it rain?', description: '', dimensions: [{ type: 'new', decision_type: 'binary', period_index: 2, header: 'Will it rain?' }], trading_fee: 0.005, initial_liquidity: 100000, tx_fee_sats: 1000, max_listing_fee_sats: 20000 }],
    });
  });

  it('a decision alone is decision_claim, the ends of a scale at the top of the request', async () => {
    const { go } = await import('../lib/nav.svelte');
    go('create', 'decision');
    const Create = (await import('./pages/Create.svelte')).default;
    const { container } = render(Create);
    await waitFor(() => expect(container.querySelector('#create-question')).toBeTruthy());
    expect(container.querySelector('#create-title')).toBeNull();
    await fireEvent.click(button(container, 'A number'));
    await fireEvent.input(container.querySelector('#create-question') as HTMLInputElement, { target: { value: 'How warm?' } });
    await fireEvent.input(container.querySelector('#create-min') as HTMLInputElement, { target: { value: '-20' } });
    await fireEvent.input(container.querySelector('#create-max') as HTMLInputElement, { target: { value: '40' } });
    const submit = container.querySelector('#create-submit') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({
      method: 'decision_claim',
      params: [{ decision_type: 'scaled', decisions: [{ period_index: 2, header: 'How warm?' }], tx_fee_sats: 1000, max_listing_fee_sats: 20000, min: -20, max: 40, increment: 1 }],
    });
  });

  it('Votecoin moves with transfer_votecoin [dest, amount, fee]', async () => {
    const Votecoin = (await import('./pages/Votecoin.svelte')).default;
    const { container } = render(Votecoin);
    await fireEvent.input(container.querySelector('#vc-to') as HTMLInputElement, { target: { value: OTHER } });
    await fireEvent.input(container.querySelector('#vc-amount') as HTMLInputElement, { target: { value: '0.25' } });
    const go = container.querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(go.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(go);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({ method: 'transfer_votecoin', params: [OTHER, 0.25, 1000] });
  });

  it('a market that paid out: the outcome that won checked and filled, the others muted, the mark of the last price on each bar', async () => {
    const settled = market({
      state: 'settled',
      settledHeight: 20,
      decisions: [{ id: '800001', header: 'Will it rain?', type: { ...binary }, state: 'resolved', outcome: 1, answer: 1, votingPeriod: 2 }],
      outcomes: [
        { index: 0, label: 'No', parts: ['No'], coordinates: [0], price: 0.3, shares: 0, volume: 0, finalPrice: 0 },
        { index: 1, label: 'Yes', parts: ['Yes'], coordinates: [1], price: 0.7, shares: 40000, volume: 25000, finalPrice: 1 },
      ],
    });
    expect(resultOf(settled)).toEqual({ pays: [0, 1], winner: 1, split: false });
    const MarketOdds = (await import('./hm/MarketOdds.svelte')).default;
    const { container } = render(MarketOdds, { props: { market: settled } });
    const rows = [...container.querySelectorAll('li')];
    expect(rows[0].textContent).toContain('Yes');
    expect(rows[0].textContent).toContain('Won');
    expect(rows[0].querySelector('.hm-check')).toBeTruthy();
    expect(rows[1].classList.contains('hm-row-lost')).toBe(true);
    const fill = rows[0].querySelector('.hm-bar > span') as HTMLElement;
    expect(fill.style.width).toBe('100%');
    const mark = rows[0].querySelector('.hm-mark') as HTMLElement;
    expect(mark.style.left).toBe('70%');
    expect(container.textContent).toContain('the market called it');
  });

  it('call nothing of the node outside the manifest', async () => {
    for (const page of ['Markets', 'Portfolio', 'Voting', 'Decisions', 'Payouts', 'Votecoin', 'Create']) render((await import(`./pages/${page}.svelte`)).default);
    render((await import('./LocalCards.svelte')).default);
    render((await import('./hm/MarketView.svelte')).default, { props: { id: '0102030405ab', back: () => undefined } });
    await new Promise((r) => setTimeout(r, 500));
    const allowed = new Set([...manifest.host, ...APP_METHODS]);
    const bad = calls.filter((c) => (c.kind === 'host' ? !allowed.has(c.method ?? '') : !manifest.reads.includes(c.method ?? '')));
    expect(bad.map((c) => `${c.kind}:${c.method}`)).toEqual([]);
    for (const c of confirmedCalls()) expect(manifest.confirmed).toContain(c.method);
  });

  it('answers, results and amounts in words', () => {
    const d = decision();
    expect(answerValue(d, 'yes')).toEqual({ ok: true, value: 1 });
    expect(answerValue(d, 'unsure')).toEqual({ ok: true, value: 0.5 });
    expect(answerValue(d, '')).toMatchObject({ ok: false });
    const cat = decision({ type: { kind: 'category', options: ['A', 'B'] } });
    expect(answerValue(cat, '1')).toEqual({ ok: true, value: 1 });
    expect(answerValue(cat, 'inconclusive')).toEqual({ ok: true, value: 2 });
    expect(answerValue(cat, '2')).toMatchObject({ ok: false });
    const sc = decision({ type: { kind: 'scaled', min: -20, max: 40, increment: 0.5 } });
    expect(answerValue(sc, '21.5')).toEqual({ ok: true, value: 21.5 });
    expect(answerValue(sc, '21.3')).toMatchObject({ ok: false });
    expect(answerValue(sc, '41')).toMatchObject({ ok: false });
    expect(answerValue(sc, 'NaN')).toMatchObject({ ok: false });
    expect(formatVotecoin(0.25)).toBe('0.25');
    expect(formatVotecoin(1)).toBe('1');
    expect(maxCost(6200, 1000, 0.01)).toBe(7272);
    expect(minProceeds(5871, 0.01)).toBe(5812);
    expect(decisionProblems({ kind: 'binary', header: '', description: '', period: 1, min: '', max: '', increment: '', options: [], noLabel: '', yesLabel: '' }, 2).length).toBe(2);
    expect(resultOf(market({ state: 'settled', outcomes: market().outcomes.map((o) => ({ ...o, finalPrice: 0.5 })) }))).toEqual({ pays: [0.5, 0.5], winner: null, split: true });
  });
});
