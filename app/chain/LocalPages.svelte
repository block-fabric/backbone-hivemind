<script lang="ts">
  // The pages Hivemind draws: the drop-in pages in the kit's places (chain/overrides.ts), and its own (chain/local.ts).
  // Create is a tab of Markets and Votecoin one of the voting center (their ids lead there: tabOf).
  import Explorer from './dropin/Explorer.svelte';
  import History from './dropin/History.svelte';
  import Home from './dropin/Home.svelte';
  import NodePage from './dropin/NodePage.svelte';
  import Overview from './dropin/Overview.svelte';
  import Peg from './dropin/Peg.svelte';
  import Receive from './dropin/Receive.svelte';
  import Send from './dropin/Send.svelte';
  import Decisions from './pages/Decisions.svelte';
  import Markets from './pages/Markets.svelte';
  import Payouts from './pages/Payouts.svelte';
  import Portfolio from './pages/Portfolio.svelte';
  import Voting from './pages/Voting.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'home'}<Home />
{:else if page === 'overview'}<Overview />
{:else if page === 'send'}<Send />
{:else if page === 'receive'}<Receive />
{:else if page === 'transactions'}<History />
{:else if page === 'peg'}<Peg />
{:else if page === 'explorer'}<Explorer />
{:else if page === 'node'}<NodePage />
{:else if page === 'markets'}<Markets />
{:else if page === 'portfolio'}<Portfolio />
{:else if page === 'voting'}<Voting />
{:else if page === 'decisions'}<Decisions />
{:else if page === 'payouts'}<Payouts />
{/if}
