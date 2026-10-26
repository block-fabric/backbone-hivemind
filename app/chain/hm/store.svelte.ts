// The state of Hivemind's pages, read again when the node says something changed (a new tip, the wallet) and every
// 15 seconds while the app runs: the period clock, the markets, the decisions; and, with a wallet, its positions,
// Votecoin, ballots and payouts. One refresh at a time. The chain keeps what the pages show (positions, votes, what a
// settled market paid): no journal of the app's own.
import { on } from '../../lib/bridge';
import { errorText } from '../../lib/dialogs.svelte';
import { drop, follow } from '../dropin/store.svelte';
import { hmApi } from './api';
import type { Ballot, Decision, Market, Payout, Position, Status, Votecoin } from './model';

export const hm = $state({
  status: null as Status | null,
  markets: [] as Market[],
  decisions: [] as Decision[],
  positions: [] as Position[],
  votecoin: null as Votecoin | null,
  ballots: [] as Ballot[],
  payouts: [] as Payout[],
  loaded: false,
  error: '',
  /** Bumped by every refresh: the pages that read more read it again. */
  version: 0,
});

/** A trade to open a market on (Portfolio's "Sell…"): that outcome and side. */
export const tradeIntent = $state({ market: '', outcome: -1, side: 'buy' as 'buy' | 'sell' });

export function openTrade(market: string, outcome: number, side: 'buy' | 'sell'): void {
  tradeIntent.market = market;
  tradeIntent.outcome = outcome;
  tradeIntent.side = side;
}

let running: Promise<void> | null = null;
let again = false;

export function refreshHivemind(): Promise<void> {
  if (running) {
    again = true;
    return running;
  }
  running = (async () => {
    try {
      do {
        again = false;
        await once();
      } while (again);
    } finally {
      running = null;
    }
  })();
  return running;
}

async function once(): Promise<void> {
  try {
    const [status, markets, decisions] = await Promise.all([hmApi.status(), hmApi.markets(), hmApi.decisions()]);
    // Lists, whatever a node that is not Hivemind's answers.
    hm.status = status && typeof status === 'object' && status.period ? status : null;
    hm.markets = Array.isArray(markets) ? markets : [];
    hm.decisions = Array.isArray(decisions) ? decisions : [];
    if (drop.seed?.hasSeed) {
      const [positions, votecoin, ballots, payouts] = await Promise.all([
        hmApi.positions().catch(() => [] as Position[]),
        hmApi.votecoin().catch(() => null),
        hmApi.ballots().catch(() => [] as Ballot[]),
        hmApi.payouts().catch(() => [] as Payout[]),
      ]);
      hm.positions = Array.isArray(positions) ? positions : [];
      hm.votecoin = votecoin && typeof votecoin === 'object' ? votecoin : null;
      hm.ballots = Array.isArray(ballots) ? ballots : [];
      hm.payouts = Array.isArray(payouts) ? payouts : [];
    } else {
      hm.positions = [];
      hm.votecoin = null;
      hm.ballots = [];
      hm.payouts = [];
    }
    hm.error = '';
  } catch (e) {
    hm.error = errorText(e);
  }
  hm.loaded = true;
  hm.version++;
}

let started = false;

/** Follow the chain (a new tip, the wallet's transactions and balance: the node's events); the pages and cards call it. */
export function startHivemind(): void {
  follow();
  if (started) return;
  started = true;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const soon = () => {
    if (timer) return;
    timer = setTimeout(() => {
      timer = undefined;
      void refreshHivemind();
    }, 300);
  };
  for (const type of ['tip', 'wallet.balance', 'wallet.tx', 'wallet.loaded', 'wallet.unloaded']) on(type, soon);
  setInterval(() => void refreshHivemind(), 15_000);
  void refreshHivemind();
}

/** The decisions by id. */
export function decisionMap(): Map<string, Decision> {
  return new Map(hm.decisions.map((d) => [d.id, d]));
}

/** The wallet's positions by market and outcome (the chain keeps them by address: a sale is made from one of them). */
export interface Held {
  market: string;
  outcome: number;
  shares: number;
  value: number;
  addresses: { address: string; shares: number }[];
}

export function heldByOutcome(positions: readonly Position[]): Held[] {
  const out = new Map<string, Held>();
  for (const p of positions) {
    const key = `${p.market}:${p.outcome}`;
    let h = out.get(key);
    if (!h) out.set(key, (h = { market: p.market, outcome: p.outcome, shares: 0, value: 0, addresses: [] }));
    h.shares += p.shares;
    h.value += p.value;
    h.addresses.push({ address: p.address, shares: p.shares });
  }
  for (const h of out.values()) h.addresses.sort((a, b) => b.shares - a.shares);
  return [...out.values()];
}

/** For the tests. */
export function resetHivemind(): void {
  started = false;
  Object.assign(hm, { status: null, markets: [], decisions: [], positions: [], votecoin: null, ballots: [], payouts: [], loaded: false, error: '', version: 0 });
}
