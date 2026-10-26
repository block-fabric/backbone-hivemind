// Icons of Hivemind's own pages (components/Icon.svelte): line icons on a 24 grid, one SVG path each.
export const LOCAL_ICONS = {
  /** Markets: a trend over an axis. */
  market: 'M4 20h16M4 4v16M7 15l4-5 3 3 6-7M16 6h4v4',
  /** Portfolio: a case of holdings. */
  portfolio: 'M4 8h16v11H4zM9 8V5h6v3M4 13h16M11 13v2h2v-2',
  /** Decisions: a question in a circle. */
  decision: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM9.6 9.4a2.5 2.5 0 1 1 3.4 2.3c-.6.3-1 .8-1 1.5v.6M12 17h.01',
  /** Voting: a ballot going into its box. */
  ballot: 'M4 13h16v7H4zM8 13V4h8v9M10.5 8.5l1.2 1.2 2.3-2.4M8 16.5h8',
  /** Votecoin: a coin with a check. */
  votecoin: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM8.5 12.2l2.4 2.4 4.6-5',
  /** Payouts: coins coming down onto a stack. */
  payout: 'M12 3v8M8.5 7.5L12 11l3.5-3.5M4 15h16M4 19h16',
  /** Create: a spark. */
  create: 'M12 3v5M12 16v5M3 12h5M16 12h5M6.3 6.3l2.8 2.8M14.9 14.9l2.8 2.8M6.3 17.7l2.8-2.8M14.9 9.1l2.8-2.8',
} as const satisfies Record<string, string>;
