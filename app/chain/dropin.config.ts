// What the drop-in pages (chain/dropin) need to know of Hivemind (truthcoin): its slot, its fees, an address, and the
// words of its own kinds of activity rows (dropin.history: trades, claims, markets, ballots, Votecoin, payouts).
export const DROPIN = {
  /** Its slot on eCash. */
  slot: 13,
  /** The fee a payment proposes (satoshis). */
  defaultFee: 1000,
  /** The fee on eCash a deposit or a withdrawal proposes. */
  defaultMainFee: 1000,
  /** What an address looks like, for the placeholder. */
  addressExample: '4XrMw8bmnhchFspSyP4XRUuB5vaR',
  kinds: {
    buy: { text: 'Shares bought', icon: 'market' },
    sell: { text: 'Shares sold', icon: 'market' },
    claim: { text: 'Decision listed', icon: 'decision' },
    market: { text: 'Market opened', icon: 'create' },
    vote: { text: 'Ballot cast', icon: 'ballot' },
    'votecoin-sent': { text: 'Votecoin moved', icon: 'votecoin' },
    'votecoin-received': { text: 'Votecoin received', icon: 'votecoin' },
    amplify: { text: 'Liquidity added', icon: 'market' },
    payout: { text: 'Market payout', icon: 'payout' },
  },
} as const;
