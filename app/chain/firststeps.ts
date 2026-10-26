// The three first steps of Hivemind, on Home in Simple mode until dismissed.
import type { FirstStep } from '../lib/firststeps';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring ECX here', text: 'Bets are paid in ECX moved from eCash. Drivechains gives a deposit address to pay from an eCash wallet (the Backbone app: Sidechains → Hivemind). They arrive with the next block of Hivemind.', icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Pick a question and bet', text: 'Markets shows what people bet on, with the chance the bets give each answer. Buy the outcome you think comes true: each share pays 1 sat if right.', icon: 'market', action: { label: 'See the markets', page: 'markets' } },
  { title: 'Sell any time, or wait for the vote', text: 'Portfolio shows what each bet is worth now. Sell it while the market trades, or keep it: a right bet pays once the holders of Votecoin resolve the question.', icon: 'portfolio', action: { label: 'My bets', page: 'portfolio' } },
];
