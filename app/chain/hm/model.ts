// Hivemind (Truthcoin) in the app: what the node's app layer says of the markets, decisions, votes and Votecoin
// (chains/hivemind/src/appui.cpp, hivemind.*), as types, and what the pages work out of it: the stage of a market, what
// it came to once it paid out, the clock of the voting periods, the answers of a ballot, amounts in words. Pure
// functions: hivemind.test.ts tries them. Amounts are satoshis; a share pays 1 sat if its outcome comes true.
import { t, tn } from '../../lib/i18n.svelte';
import { niceTimeOffset } from '../../lib/format';
import { displayTextProblem } from '../dropin/text';

// --- What the node says ---

/** The clock of the periods: in blocks of this chain (a test network's --decision-config-testing), else in seconds. */
export interface PeriodClock {
  current: number;
  name: string;
  unit: 'blocks' | 'seconds';
  length: number;
  start: number;
  end: number;
  now: number;
  height: number | null;
}

/** hivemind.status. */
export interface Status {
  period: PeriodClock;
  counts: { markets: number; trading: number; settled: number; decisions: number; voting: number; voters: number };
  votecoinTotal: number;
  minerFee: number;
}

export interface DecisionType {
  kind: 'binary' | 'scaled' | 'category';
  min?: number;
  max?: number;
  increment?: number;
  lowLabel?: string;
  highLabel?: string;
  options?: string[];
  noLabel?: string;
  yesLabel?: string;
}

export type DecisionState = 'created' | 'claimed' | 'voting' | 'resolved' | 'invalid';

/** hivemind.decisions: a claimed decision. Its period is the one its answer becomes known in; it is voted on in the next. */
export interface Decision {
  id: string;
  period: number;
  index: number;
  votingPeriod: number;
  standard: boolean;
  header: string;
  description: string;
  tags: string[];
  type: DecisionType;
  author: string;
  claimTxid: string;
  state: DecisionState;
  resolvedHeight?: number;
  /** Once resolved: the normalized value (0 no .. 1 yes, a place on the scale), the answer in the decision's units. */
  outcome?: { value: number | null; answer: number | null; confidence: number | null; votes: number; winner?: number | null };
  markets: string[];
  /** Listed by this wallet (only for a reader of the wallet). */
  mine?: boolean;
}

export interface Outcome {
  index: number;
  label: string;
  /** The value of each decision (dimension) of the market in this outcome, in words. */
  parts: string[];
  coordinates: number[];
  /** The chance the market gives it now (its last before it settled). */
  price: number;
  shares: number;
  volume: number;
  /** What a share of it paid, once the market settled (0 to 1 of a sat... of its 1 sat). */
  finalPrice?: number;
}

export interface MarketDecision {
  id: string;
  header: string;
  type?: DecisionType;
  state: DecisionState;
  outcome?: number | null;
  answer?: number | null;
  votingPeriod: number;
}

export type MarketState = 'trading' | 'cancelled' | 'invalid' | 'settled';

/** hivemind.markets, hivemind.market. */
export interface Market {
  id: string;
  title: string;
  description: string;
  tags: string[];
  creator: string;
  createdHeight: number;
  expiresHeight: number | null;
  state: MarketState;
  updatedHeight: number;
  settledHeight?: number;
  beta: number | null;
  liquidity: number;
  treasury: number;
  fees: number;
  volume: number;
  feeRate: number | null;
  txPow: boolean;
  decisions: MarketDecision[];
  outcomes: Outcome[];
  mine?: boolean;
  holders?: { address: string; outcome: number; shares: number; mine?: boolean }[];
  history?: PricePoint[];
}

/** market_price_history: the prices after each block that traded in the market, from its creation. */
export interface PricePoint {
  height: number;
  block_hash: string;
  timestamp: number;
  prices: number[];
}

/** hivemind.positions: a position of an address of the wallet. */
export interface Position {
  market: string;
  outcome: number;
  address: string;
  shares: number;
  price: number;
  /** Worth at the price now, in satoshis. */
  value: number;
}

/** hivemind.votecoin. */
export interface Votecoin {
  voter: string | null;
  votecoin: number;
  /** ECX at the voter address: what pays the fee of moving Votecoin. */
  voterCoins: number;
  addresses: { address: string; votecoin: number }[];
}

/** vote_voters (LayerTwo): every holder of Votecoin. */
export interface Holder {
  address: string;
  votecoin_balance: number;
  total_votes: number;
  is_active: boolean;
}

/** hivemind.ballots: a vote of the wallet. */
export interface Ballot {
  decision: string;
  period: number;
  voter: string;
  height: number;
  value: number | null;
  answer: string;
}

/** hivemind.payouts: what a settled market paid the wallet. */
export interface Payout {
  market: string;
  height: number;
  treasuryPaid: number;
  feesPaid: number;
  shareholders: number;
  shares: { address: string; outcome: number; shares: number; finalPrice: number; paid: number }[];
  fees: { address: string; role: 'voter' | 'decision-author' | 'market-author'; paid: number }[];
  refund?: number;
  total: number;
}

/** hivemind.quote. */
export interface Quote {
  side: 'buy' | 'sell';
  shares: number;
  cost: number;
  tradingFee: number;
  net?: number;
  minerFee: number;
  priceBefore: number | null;
  priceAfter: number | null;
}

/** list_open_periods_with_pricing (LayerTwo): the periods that take decisions, their cheapest slot. */
export interface OpenPeriod {
  period_index: number;
  cheapest_available_slot_sats: number;
  cheapest_available_tier: number;
  slots_available_by_tier: number[];
}

// --- What a market is about ---

export type MarketKind = 'binary' | 'scalar' | 'categorical' | 'combo';

export function marketKind(m: Pick<Market, 'decisions'>): MarketKind {
  if (m.decisions.length !== 1) return 'combo';
  const k = m.decisions[0].type?.kind;
  return k === 'scaled' ? 'scalar' : k === 'category' ? 'categorical' : 'binary';
}

export function kindText(kind: MarketKind, outcomes: number): string {
  switch (kind) {
    case 'binary':
      return t('Yes or no');
    case 'scalar':
      return t('Scale');
    case 'categorical':
      return t('%1 options', outcomes);
    default:
      return t('%1 outcomes', outcomes);
  }
}

/** The colour of an outcome (hm.css .hm-cN): a binary market's No in the colour of a loss, its Yes in the accent. */
export function colour(m: Pick<Market, 'decisions'>, o: Pick<Outcome, 'index'>): number {
  return marketKind(m) === 'binary' ? (o.index === 0 ? 0 : 1) : (o.index + 1) % 8;
}

/** What a market is at: trading; its decisions being voted on (it still trades); some of them resolved; paid out. */
export type MarketStage = 'trading' | 'voting' | 'partly' | 'paid' | 'cancelled' | 'invalid';

export function marketStage(m: Pick<Market, 'state' | 'decisions'>): MarketStage {
  if (m.state === 'settled') return 'paid';
  if (m.state === 'cancelled' || m.state === 'invalid') return m.state;
  const resolved = m.decisions.filter((d) => d.state === 'resolved').length;
  if (resolved > 0) return 'partly';
  if (m.decisions.some((d) => d.state === 'voting')) return 'voting';
  return 'trading';
}

export function stageText(s: MarketStage): string {
  switch (s) {
    case 'trading':
      return t('Live');
    case 'voting':
      return t('Being voted on');
    case 'partly':
      return t('Partly decided');
    case 'paid':
      return t('Paid out');
    case 'cancelled':
      return t('Cancelled');
    default:
      return t('Invalid');
  }
}

/** What a settled market came to: what a share of each outcome paid, the one that paid in full (none: a split). */
export interface Result {
  pays: number[];
  winner: number | null;
  split: boolean;
}

export function resultOf(m: Pick<Market, 'state' | 'outcomes'>): Result | null {
  if (m.state !== 'settled') return null;
  const pays = m.outcomes.map((o) => (typeof o.finalPrice === 'number' && Number.isFinite(o.finalPrice) ? Math.max(0, Math.min(1, o.finalPrice)) : 0));
  const full = pays.findIndex((p) => Math.abs(p - 1) < 1e-9);
  return { pays, winner: full >= 0 ? m.outcomes[full].index : null, split: full < 0 && pays.filter((p) => p > 0).length > 1 };
}

/** The outcome the last prices favoured, when one stands out (a tie: none). */
export function favourite(m: Pick<Market, 'outcomes'>): number | null {
  let best: Outcome | null = null;
  let tie = false;
  for (const o of m.outcomes) {
    if (!best || o.price > best.price + 1e-9) {
      best = o;
      tie = false;
    } else if (Math.abs(o.price - best.price) <= 1e-9) tie = true;
  }
  return best && !tie ? best.index : null;
}

/** The likeliest outcome now. */
export function leader(m: Pick<Market, 'outcomes'>): Outcome | null {
  return m.outcomes.reduce<Outcome | null>((b, o) => (!b || o.price > b.price ? o : b), null);
}

/** What a share of an outcome is worth now: its price, or what it paid once the market settled. */
export function shareValue(m: Pick<Market, 'state' | 'outcomes'>, outcome: number): number {
  const o = m.outcomes.find((x) => x.index === outcome);
  if (!o) return 0;
  if (m.state === 'settled') return resultOf(m)?.pays[m.outcomes.indexOf(o)] ?? 0;
  return o.price;
}

/** A market on a scale: where its prices put the answer (min + (max - min) × the chance of its high end). */
export function scaleEstimate(m: Pick<Market, 'decisions' | 'outcomes'>): number | null {
  const d = m.decisions[0]?.type;
  if (marketKind(m) !== 'scalar' || !d || d.min === undefined || d.max === undefined) return null;
  const high = m.outcomes.find((o) => o.coordinates[0] === 1);
  return high ? d.min + (d.max - d.min) * high.price : null;
}

export function scalePlace(value: number, min: number, max: number): number {
  if (!(max > min) || !Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(1, (value - min) / (max - min)));
}

/** The name of an outcome on a card: a single decision's value ("Yes", "Brazil"), or every decision's ("Rain: Yes, Temp: Max"). */
export function outcomeName(m: Pick<Market, 'decisions'>, o: Pick<Outcome, 'label' | 'parts'>): string {
  return m.decisions.length === 1 ? (o.parts[0] ?? o.label) : o.label;
}

// --- The periods ---

/** How far it is to a height or a time of the clock, in words: "3 blocks", "2 hours". */
export function untilIn(clock: PeriodClock, target: number, nowSeconds: number): string {
  if (clock.unit === 'blocks') return tn('%n block(s)', Math.max(0, target - (clock.height ?? clock.now)));
  return niceTimeOffset(Math.max(0, target - nowSeconds));
}

/** The end of a period (a height or a time). */
export function periodEnd(clock: PeriodClock, period: number): number {
  return clock.end + (period - clock.current) * clock.length;
}

/** The start of a period. */
export function periodStart(clock: PeriodClock, period: number): number {
  return clock.start + (period - clock.current) * clock.length;
}

// --- Ballots ---

/** An answer as the chain takes it (vote_submit's vote_value, in the decision's units): binary 0 no, 1 yes, 0.5 unsure;
 *  a category's option index, its count for "inconclusive"; a number of the scale, on its steps. */
export function answerValue(d: Pick<Decision, 'type'>, text: string): { ok: true; value: number } | { ok: false; error: string } {
  const v = text.trim();
  if (d.type.kind === 'binary') {
    if (v === 'yes') return { ok: true, value: 1 };
    if (v === 'no') return { ok: true, value: 0 };
    if (v === 'unsure') return { ok: true, value: 0.5 };
    return { ok: false, error: t('Choose an answer.') };
  }
  if (d.type.kind === 'category') {
    const n = d.type.options?.length ?? 0;
    if (v === 'inconclusive') return { ok: true, value: n };
    const i = Number(v);
    if (v === '' || !Number.isInteger(i) || i < 0 || i >= n) return { ok: false, error: t('Choose an option.') };
    return { ok: true, value: i };
  }
  const min = d.type.min ?? 0;
  const max = d.type.max ?? 0;
  const inc = d.type.increment || 1;
  const x = Number(v.replace(',', '.'));
  if (v === '' || !Number.isFinite(x)) return { ok: false, error: t('A number from %1 to %2.', String(min), String(max)) };
  if (x < min || x > max) return { ok: false, error: t('A number from %1 to %2.', String(min), String(max)) };
  const k = Math.round((x - min) / inc);
  if (Math.abs(x - min - k * inc) > Math.max(Math.abs(inc) * 1e-9, 1e-12)) return { ok: false, error: t('In steps of %1 from %2.', String(inc), String(min)) };
  return { ok: true, value: x };
}

/** A resolved decision's answer in words. */
export function resolvedText(d: Pick<Decision, 'type' | 'outcome' | 'state'>): string {
  if (d.state !== 'resolved' || !d.outcome) return '';
  const o = d.outcome;
  if (d.type.kind === 'binary') {
    if (o.value === 1) return d.type.yesLabel ?? t('Yes');
    if (o.value === 0) return d.type.noLabel ?? t('No');
    return t('Unsure (%1)', formatChance(o.value ?? 0));
  }
  if (d.type.kind === 'category') {
    const w = o.winner;
    return w === null || w === undefined ? t('Inconclusive') : (d.type.options?.[w] ?? String(w));
  }
  return o.answer === null || o.answer === undefined ? '' : formatNumber(o.answer);
}

// --- Amounts ---

/** A count of shares: whole, grouped by thousands. */
export function formatShares(n: number): string {
  if (!Number.isFinite(n)) return '–';
  return Math.trunc(n)
    .toString()
    .replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
}

/** Votecoin: up to 10 decimals (the chain's precision), no trailing zeros. */
export function formatVotecoin(x: number | null | undefined): string {
  if (x === null || x === undefined || !Number.isFinite(x)) return '–';
  return x.toFixed(10).replace(/\.?0+$/, '') || '0';
}

/** A chance: "62.5%". */
export function formatChance(p: number | null | undefined): string {
  if (p === null || p === undefined || !Number.isFinite(p)) return '–';
  const pct = p * 100;
  return `${pct >= 10 || pct === 0 ? pct.toFixed(1) : pct.toFixed(2)}%`;
}

export function formatNumber(x: number): string {
  return Number.isInteger(x) ? String(x) : String(Number(x.toFixed(8)));
}

/** A trading fee rate (0.005) as "0.5%". */
export function formatRate(r: number | null | undefined): string {
  if (r === null || r === undefined || !Number.isFinite(r)) return '–';
  return `${Number((r * 100).toFixed(3))}%`;
}

/** The most to pay for a purchase quoted at `cost` (its miner fee included), with `slippage` of room, in satoshis (whole
 *  basis points: no floating point in the bound). */
export function maxCost(cost: number, minerFee: number, slippage: number): number {
  const total = cost + minerFee;
  const bps = Math.max(0, Math.round(slippage * 10000));
  return total + Math.ceil((total * bps) / 10000);
}

/** The least to get for a sale quoted at `net`, with `slippage` of room, in satoshis. */
export function minProceeds(net: number, slippage: number): number {
  const bps = Math.max(0, Math.round(slippage * 10000));
  return Math.max(0, net - Math.ceil((net * bps) / 10000));
}

// --- Forms ---

export interface DecisionDraft {
  kind: 'binary' | 'scaled' | 'category';
  header: string;
  description: string;
  period: number;
  min: string;
  max: string;
  increment: string;
  options: string[];
  noLabel: string;
  yesLabel: string;
}

/** What is wrong with a decision to list; [] if nothing. */
export function decisionProblems(d: DecisionDraft, current: number): string[] {
  const out: string[] = [];
  const bytes = new TextEncoder().encode(d.header.trim()).length;
  if (!d.header.trim()) out.push(t('Write the question.'));
  else if (bytes > 100) out.push(t('The question is at most 100 bytes.'));
  if (new TextEncoder().encode(d.description).length > 2000) out.push(t('The description is at most 2000 bytes.'));
  // The node's rule for text its trusted dialog shows (bt::appui::display_text_problem): its twin here.
  const odd = [d.header, d.description.replace(/\n/g, ' '), d.yesLabel, d.noLabel, ...d.options].map(displayTextProblem).find((x) => x);
  if (odd) out.push(t(odd));
  if (!Number.isInteger(d.period) || d.period < current) out.push(t('Period %1 at the earliest.', String(current)));
  if (d.period > 127) out.push(t('Periods go to 127.'));
  if (d.kind === 'scaled') {
    const min = Number(d.min);
    const max = Number(d.max);
    const inc = d.increment.trim() === '' ? 1 : Number(d.increment);
    if (!Number.isFinite(min) || !Number.isFinite(max) || d.min.trim() === '' || d.max.trim() === '') out.push(t('Give both ends of the scale.'));
    else if (!(max > min)) out.push(t('The high end is above the low end.'));
    if (!Number.isFinite(inc) || !(inc > 0)) out.push(t('The step is above 0.'));
  }
  if (d.kind === 'category') {
    const opts = d.options.map((o) => o.trim()).filter((o) => o);
    if (opts.length < 2) out.push(t('At least two options.'));
    if (new Set(opts).size !== opts.length) out.push(t('An option is there twice.'));
  }
  return out;
}

/** A decision draft as LayerTwo's request takes it (decision_claim's entry, or market_create's "new" dimension). */
export function claimEntry(d: DecisionDraft, asDimension: boolean): Record<string, unknown> {
  const e: Record<string, unknown> = {};
  if (asDimension) {
    e.type = 'new';
    e.decision_type = d.kind;
  }
  e.period_index = d.period;
  e.header = d.header.trim();
  if (d.description.trim()) e.description = d.description.trim();
  if (d.kind === 'binary') {
    if (d.noLabel.trim()) e.option_0_label = d.noLabel.trim();
    if (d.yesLabel.trim()) e.option_1_label = d.yesLabel.trim();
  }
  if (d.kind === 'category') e.option_labels = d.options.map((o) => o.trim()).filter((o) => o);
  if (asDimension && d.kind === 'scaled') {
    e.min = Number(d.min);
    e.max = Number(d.max);
    e.increment = d.increment.trim() === '' ? 1 : Number(d.increment);
  }
  return e;
}

/** A category of a market: a word in brackets at the start of its title ("[Sports] Who wins…"), or its first tag. */
export function categoryOf(m: Pick<Market, 'title' | 'tags'>): string | null {
  const bracket = /^\s*\[([^\]]{1,24})\]/.exec(m.title);
  if (bracket) return bracket[1].trim();
  return m.tags[0] ?? null;
}

export function titleWithoutCategory(title: string): string {
  return title.replace(/^\s*\[[^\]]{1,24}\]\s*/, '');
}
