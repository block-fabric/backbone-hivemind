// Hivemind's own pages beside the drop-in pages (chain/overrides.ts puts those in the kit's places): the markets (and,
// as their tab, Create), the wallet's portfolio, the voting center (and, as its tab, Votecoin), the decisions with the
// calendar of the periods, and the payouts. Their host methods are hivemind.* (chains/hivemind/src/appui.cpp).
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  id: string;
  title: string;
  icon: IconName;
  summary: string;
  section: 'wallet' | 'chain' | 'tools';
  wallet?: boolean;
  shortcut?: string;
  keywords?: string;
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  simpleTitle?: string;
  advanced?: boolean;
  simple?: boolean;
  simpleSummary?: string;
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  { id: 'markets', title: 'Markets', simpleTitle: 'Questions', simple: true, simpleSummary: 'Bet on what will happen', icon: 'market', section: 'chain', summary: 'Browse the prediction markets and trade in them', keywords: 'hivemind truthcoin predict trade buy sell shares odds', quick: { label: 'Trade', icon: 'market', simpleLabel: 'Bet on a question' } },
  { id: 'portfolio', title: 'Portfolio', simpleTitle: 'My bets', simple: true, simpleSummary: 'Your bets and what they are worth', icon: 'portfolio', section: 'chain', summary: 'Your shares, what they are worth and what they pay if right', wallet: true, keywords: 'positions shares worth' },
  { id: 'voting', title: 'Voting center', icon: 'ballot', section: 'chain', summary: 'Your ballots on the decisions, and the Votecoin that casts them', wallet: true, keywords: 'vote ballot reputation votecoin transfer holders', quick: { label: 'Vote', icon: 'ballot' } },
  { id: 'decisions', title: 'Decisions', icon: 'decision', section: 'chain', summary: 'The questions the markets are about, and the calendar of the periods', keywords: 'questions periods calendar slots listing' },
  { id: 'create', title: 'Create', icon: 'create', section: 'chain', summary: 'List a decision and open a market on it', wallet: true, keywords: 'new market decision list open', quick: { label: 'Create a market', icon: 'create', tab: 'market' }, tabOf: { page: 'markets', tab: 'create' } },
  { id: 'votecoin', title: 'Votecoin', icon: 'votecoin', section: 'chain', summary: 'The reputation that votes: your holding, moving it, every holder', wallet: true, keywords: 'reputation votecoin transfer holders', tabOf: { page: 'voting', tab: 'votecoin' } },
  { id: 'payouts', title: 'Payouts', icon: 'payout', section: 'chain', summary: 'What the markets paid: settlements, fees of the voters and authors', wallet: true, keywords: 'settlement payout settled paid results' },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'hm.create-market', label: 'Create a market…', page: 'create', tab: 'market', tip: 'Open a market on a decision', keywords: 'new market' },
  { id: 'hm.list-decision', label: 'List a decision…', page: 'create', tab: 'decision', tip: 'List a question for the markets and the voters', keywords: 'new decision question' },
  { id: 'hm.periods', label: 'Periods calendar', page: 'decisions', tab: 'calendar', tip: 'The period now, and the periods ahead with their slots', keywords: 'period calendar slots' },
  { id: 'hm.transfer', label: 'Move Votecoin…', page: 'votecoin', tip: 'Move reputation to another address', keywords: 'transfer reputation' },
  { id: 'hm.paid-out', label: 'Markets that paid out', page: 'markets', tab: 'status/paid', tip: 'What the vote decided on each market, and what it paid', keywords: 'results resolved settled won lost payout' },
] as const satisfies readonly LocalCommand[];
