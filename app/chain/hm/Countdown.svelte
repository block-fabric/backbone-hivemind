<script lang="ts">
  // How long until a deadline of the periods (the end of one): in blocks on a network whose periods are counted in
  // blocks of this chain (a test network), else in time. Ticks every second.
  import { t } from '../../lib/i18n.svelte';
  import { every } from '../../lib/poll';
  import { untilIn, type PeriodClock } from './model';

  let { clock, target, phrase }: { clock: PeriodClock; target: number; phrase?: 'ends' | 'starts' | 'in' } = $props();

  let now = $state(Math.floor(Date.now() / 1000));
  $effect(() => every(1000, () => (now = Math.floor(Date.now() / 1000))));
  const left = $derived(clock.unit === 'blocks' ? target - (clock.height ?? clock.now) : target - now);
  const words = $derived(untilIn(clock, target, now));
  const shown = $derived.by(() => {
    const done = left <= 0;
    switch (phrase) {
      case 'ends':
        return done ? t('ends now') : t('ends in %1', words);
      case 'starts':
        return done ? t('starts now') : t('starts in %1', words);
      case 'in':
        return done ? t('now') : t('in %1', words);
      default:
        return done ? t('now') : words;
    }
  });
</script>

<span class="countdown num" class:over={left <= 0} title={clock.unit === 'blocks' ? t('Periods are counted in blocks of this chain') : t('Counted in the time of the mainchain')}>{shown}</span>

<style>
  .countdown {
    font-weight: 600;
    white-space: nowrap;
  }
  .over {
    color: var(--text-muted);
  }
</style>
