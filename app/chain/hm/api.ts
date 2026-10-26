// Hivemind's host methods (hivemind.*, chains/hivemind/src/appui.cpp) and the LayerTwo reads its pages make. The
// spends go through rpc.confirmed with LayerTwo's own method names and parameters (asked in the app's trusted dialog):
// decision_claim, market_create, market_buy, market_sell, market_amplify_beta, vote_submit, transfer_votecoin.
import { request } from '../../lib/bridge';
import { confirmed, l2l } from '../dropin/api';
import type { Ballot, Decision, Holder, Market, OpenPeriod, Payout, Position, Quote, Status, Votecoin } from './model';

const host = <T>(method: string, params: Record<string, unknown> = {}) => request<T>('host', method, params);

export const hmApi = {
  status: () => host<Status>('hivemind.status'),
  markets: () => host<Market[]>('hivemind.markets'),
  market: (id: string) => host<Market>('hivemind.market', { id }),
  decisions: () => host<Decision[]>('hivemind.decisions'),
  quote: (market: string, outcome: number, shares: number, side: 'buy' | 'sell') => host<Quote>('hivemind.quote', { market, outcome, shares, side }),
  positions: () => host<Position[]>('hivemind.positions'),
  votecoin: () => host<Votecoin>('hivemind.votecoin'),
  ballots: () => host<Ballot[]>('hivemind.ballots'),
  payouts: () => host<Payout[]>('hivemind.payouts'),
  holders: () => l2l<Holder[]>('vote_voters'),
  openPeriods: () => l2l<OpenPeriod[]>('list_open_periods_with_pricing'),
};

/** The spends, LayerTwo's names and parameters. */
export const hmSpend = {
  claim: (request: Record<string, unknown>) => confirmed<{ txid: string; decision_ids: string[]; listing_fee_paid_sats: number }>('decision_claim', [request]),
  createMarket: (request: Record<string, unknown>) => confirmed<{ txid: string; market_id: string }>('market_create', [request]),
  buy: (market: string, outcome: number, shares: number, maxCost: number) =>
    confirmed<{ txid: string; cost_sats: number }>('market_buy', [{ market_id: market, outcome_index: outcome, shares_amount: shares, max_cost: maxCost }]),
  sell: (market: string, outcome: number, shares: number, seller: string, minProceeds: number) =>
    confirmed<{ txid: string; net_proceeds_sats: number }>('market_sell', [{ market_id: market, outcome_index: outcome, shares_amount: shares, seller_address: seller, min_proceeds: minProceeds }]),
  amplify: (market: string, amount: number) => confirmed<string>('market_amplify_beta', [{ market_id: market, amount_sats: amount }]),
  /** One ballot, counted as cast (truthcoin has no sealed votes). */
  vote: (votes: { decision_id: string; vote_value: number }[], fee: number) => confirmed<string>('vote_submit', [votes, fee]),
  moveVotecoin: (dest: string, amount: number, fee: number) => confirmed<string>('transfer_votecoin', [dest, amount, fee]),
};
