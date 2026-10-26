<script lang="ts">
  // The bar of an outcome of a market: its chance while it trades, what a share of it paid once the market paid out.
  // With `mark`, where the market's last price before it settled stood, as a thin faint line across the bar: next to
  // what the outcome paid, whether the market called it. In the colour of its outcome (.hm-cN, set by the row); `tone`
  // won puts it in the colour of a gain, lost and out mute it.
  import { t } from '../../lib/i18n.svelte';
  import { barLeft, barWidth } from './actions';
  import { formatChance } from './model';

  let { fill, mark = null, tone = '' }: { fill: number; mark?: number | null; tone?: 'won' | 'lost' | 'out' | '' } = $props();
</script>

<span class="hm-barwrap" class:hm-won={tone === 'won'} class:hm-lost={tone === 'lost'} class:hm-out={tone === 'out'}>
  <span class="hm-bar"><span use:barWidth={fill}></span></span>
  {#if mark !== null}<span class="hm-mark" use:barLeft={mark} title={t('%1 before it settled', formatChance(mark))}></span>{/if}
</span>
