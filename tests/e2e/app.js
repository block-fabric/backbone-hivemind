// The end-to-end run of hivemind-app on a local eCash regtest (run_app.py): the app starts and runs its node (truthcoin);
// the wallet's backup phrase is made (its words only in the trusted dialog); ECX is deposited from eCash through the
// mainchain interface's wallet; a payment is sent to another Hivemind node and one received from it; a withdrawal to
// eCash is asked; a market is opened on a new yes-or-no decision, shares of Yes bought and some sold; Votecoin moved to
// node B's voter address; once the decision's voting period comes, it is voted Yes; the market pays out (its result
// bars on Markets, the payout on Payouts); the withdrawal paid; the pages pictured, dark and light.
// window.__E2E: { peerAddress, peerVoter, mainAddress, saveFile } from the runner.
(async () => {
  const { host, sleep, log, T, waitFor, check, snapshot, click, type, go, theme, finish, ready } = E2E;
  const overview = () => host('dropin.overview');
  const history = () => host('dropin.history', { limit: 100 });
  const status = () => host('hivemind.status');
  const ecx = 100_000_000;
  const QUESTION = 'Will the e2e run pass?';
  /** Pick an option of a select as the user does (the page's binding hears a change event). */
  function choose(selector, value) {
    const el = document.querySelector(selector);
    if (!el) throw new Error(`no ${selector}`);
    el.value = String(value);
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }
  /** A radio or chip by its words, inside `root`. */
  function pick(selector, words, root = document) {
    const b = [...root.querySelectorAll(selector)].find((x) => x.textContent.trim().includes(words));
    if (!b) throw new Error(`no ${selector} with ${words}`);
    b.click();
  }
  try {
    await theme('midnight');
    await ready();
    await check('the app runs its node (phase ready, a tip)', T().app.phase === 'ready');
    await check('app.info is the node’s and the app’s', T().app.info.name === 'Hivemind' && T().app.info.hostApi === 1 && T().app.info.embedded === false);
    await go('home');
    await waitFor('the setup card', () => document.querySelector('.setup'));

    // ---- the backup phrase: made by the node, shown in the trusted dialog (answered yes by the test), the wallet set
    await host('test.confirmAnswer', { yes: true });
    await click('Make a new backup phrase');
    await waitFor('the wallet set up', async () => (await host('dropin.seed.status')).hasSeed);
    const phraseQ = (await host('test.confirmed')).find((q) => q.kind === 'node:phrase.create');
    await check('the phrase was asked in the trusted dialog, its words never recorded', !!phraseQ && phraseQ.details === '(the words, not recorded)');
    await check('the page got no words', !JSON.stringify(T().app).match(/\b(abandon|zoo)\b/));

    // ---- a deposit from eCash, paid by Backbone's wallet (-grpcwallet), to the bare address
    await go('peg', 'deposit');
    await waitFor('the deposit form', () => document.querySelector('#dep-amount'));
    type('#dep-amount', '10');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and deposit');
    await waitFor('the deposit credited', async () => (await overview()).wallet.available >= 10 * ecx, 300000);
    const q1 = (await host('test.confirmed')).find((q) => q.kind === 'node:deposit');
    await check('the deposit was asked with its amount', !!q1 && q1.rows.some((r) => r.label === 'Amount' && r.value === '10.00000000 ECX'));
    const genesis = await host('hivemind.votecoin');
    await check('the wallet mined block 0: its voter address holds the initial Votecoin', genesis.votecoin > 0, JSON.stringify(genesis));

    // ---- send to the other Hivemind node (truthcoin's create_transfer)
    await go('send');
    type('#send-to', window.__E2E.peerAddress);
    await waitFor('the address checked', () => document.querySelector('.state.good'));
    type('#send-amount', '1.5');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and send');
    await waitFor('the payment in a block', async () => (await history()).some((r) => r.kind === 'send' && !r.pending), 300000);

    // ---- a little ECX to the voter address: what pays the fee of moving Votecoin
    if ((await host('hivemind.votecoin')).voterCoins < 100_000) {
      await go('send');
      type('#send-to', genesis.voter);
      await waitFor('the voter address checked', () => document.querySelector('.state.good'));
      type('#send-amount', '0.01');
      await host('test.confirmAnswer', { yes: true });
      await click('Review and send');
      await waitFor('coins at the voter address', async () => (await host('hivemind.votecoin')).voterCoins >= 100_000, 300000);
    }

    // ---- receive from it
    await go('receive');
    const shown = await waitFor('an address shown', () => {
      const el = document.querySelector('.addr .mono.big');
      return el ? el.textContent.trim() : null;
    });
    await log(`SIGNAL receive-address ${shown}`);
    await waitFor('the payment received', async () => (await history()).some((r) => r.kind === 'receive' && !r.pending && r.amount === 0.25 * ecx), 300000);
    await check('a payment received at the address shown', true);

    // ---- withdraw to eCash: asked now, paid at the end (a bundle, voted)
    await go('peg', 'withdraw');
    type('#w-to', window.__E2E.mainAddress);
    await waitFor('the eCash address checked', () => document.querySelector('.state.good'));
    type('#w-amount', '2');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and withdraw');
    await waitFor('the withdrawal asked', async () => (await host('dropin.peg')).withdrawals.length > 0, 60000);

    // ---- a market on a new decision, its answer known in the next period (voted on in the one after)
    const st0 = await status();
    const period = st0.period.current + 1;
    await go('create', 'market');
    await waitFor('the create form', () => document.querySelector('#create-question'));
    type('#create-question', QUESTION);
    await waitFor(`period ${period} offered`, () => [...document.querySelectorAll('#create-period option')].some((o) => o.value === String(period)));
    choose('#create-period', period);
    type('#create-liquidity', '0.001');
    await sleep(800);
    await snapshot('hivemind-01-create');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and open the market');
    const created = await waitFor('the market in a block', async () => (await host('hivemind.markets')).find((m) => m.title.includes(QUESTION)), 300000);
    const qm = (await host('test.confirmed')).find((q) => q.kind === 'node:market');
    await check('the market was asked with its question and its liquidity', !!qm && JSON.stringify(qm.rows).includes(QUESTION), JSON.stringify(qm && qm.rows));
    await check('the market opened on one new decision, two outcomes', created.decisions.length === 1 && created.outcomes.length === 2, JSON.stringify(created.outcomes));

    // ---- buy 20,000 shares of Yes, then sell 5,000 of them back
    await go('markets', `m-${created.id}`);
    await waitFor('the trade panel', () => document.querySelector('#trade-amount'));
    pick('[role="radio"].opt', 'Yes');
    pick('.hm-chip', 'Shares to buy');
    type('#trade-amount', '20000');
    await waitFor('the node’s quote', () => document.querySelector('.facts.preview'), 30000);
    await snapshot('hivemind-02-buy');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and buy');
    const yes = created.outcomes.find((o) => /yes/i.test(o.label)) ?? created.outcomes[1];
    const held = async () => (await host('hivemind.positions')).filter((p) => p.market === created.id && p.outcome === yes.index).reduce((s, p) => s + p.shares, 0);
    await waitFor('20,000 shares held', async () => (await held()) >= 20000, 300000);
    const qb = (await host('test.confirmed')).find((q) => q.kind === 'node:trade' && /buy/i.test(q.title));
    await check('the buy was asked with its shares and its most cost', !!qb && JSON.stringify(qb.rows).includes('20'), JSON.stringify(qb && qb.rows));
    await go('markets', `m-${created.id}`);
    await waitFor('the trade panel', () => document.querySelector('#trade-amount'));
    [...document.querySelectorAll('[role="tab"]')].find((b) => b.textContent.trim() === 'Sell').click();
    await sleep(400);
    pick('[role="radio"].opt', 'Yes');
    type('#trade-amount', '5000');
    await waitFor('the node’s quote', () => document.querySelector('.facts.preview'), 30000);
    await host('test.confirmAnswer', { yes: true });
    await click('Review and sell');
    await waitFor('15,000 shares held', async () => (await held()) === 15000, 300000);
    await check('the sell went through: 15,000 shares of Yes held', (await held()) === 15000);
    await go('markets', `m-${created.id}`);
    await sleep(1500);
    await snapshot('hivemind-03-market');

    // ---- Votecoin: a tenth moved to node B's voter address
    await go('votecoin');
    await waitFor('the move form', () => document.querySelector('#vc-to'));
    type('#vc-to', window.__E2E.peerVoter);
    type('#vc-amount', '0.1');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and move');
    await waitFor('the Votecoin moved', async () => {
      const v = await host('hivemind.votecoin');
      return v.votecoin < genesis.votecoin - 0.09;
    }, 300000);
    await check('Votecoin moved to node B', true);

    // ---- the vote, in the decision's voting period
    const decision = created.decisions[0].id;
    await waitFor(`voting period ${period + 1}`, async () => (await status()).period.current >= period + 1, 600000);
    await go('voting');
    const ballot = await waitFor('the decision to vote on', () => document.querySelector(`[data-decision="${decision}"]`), 60000);
    pick('[role="radio"]', 'Yes', ballot);
    await sleep(400);
    await snapshot('hivemind-04-voting');
    await host('test.confirmAnswer', { yes: true });
    await click('Cast vote');
    await waitFor('the ballot in a block', async () => (await host('hivemind.ballots')).some((b) => b.decision === decision), 300000);
    await check('the ballot is on the chain', true);

    // ---- the market pays out: the vote resolves the decision, the market settles
    const settled = await waitFor('the market paid out', async () => {
      const m = await host('hivemind.market', { id: created.id });
      return m.state === 'settled' ? m : null;
    }, 900000);
    const won = settled.outcomes.find((o) => o.index === yes.index);
    await check('Yes won (its share paid 1)', !!won && (won.finalPrice ?? 0) > 0.99, JSON.stringify(settled.outcomes));
    const payouts = await waitFor('the payout listed', async () => (await host('hivemind.payouts')).find((p) => p.market === created.id), 120000);
    await check('the wallet was paid for its 15,000 shares', payouts.total >= 15000, JSON.stringify(payouts));
    await go('markets', 'status/paid');
    await sleep(1500);
    await snapshot('hivemind-05-paid-out');
    await go('markets', `m-${created.id}`);
    await sleep(1500);
    await snapshot('hivemind-06-result');
    await go('payouts');
    await sleep(1500);
    await snapshot('hivemind-07-payouts');

    // ---- the withdrawal paid
    const paid = await waitFor('the withdrawal paid on eCash', async () => (await host('dropin.peg')).withdrawals.find((w) => w.state === 'paid'), 900000);
    await check('the withdrawal is paid on eCash', !!paid, JSON.stringify(paid));
    await log(`SIGNAL withdrawal-paid ${paid.m6id}`);

    // ---- the other pages, both themes
    for (const [mode, id] of [['dark', 'midnight'], ['light', 'light']]) {
      await theme(id);
      for (const [page, tab, name] of [['home', '', 'home'], ['markets', '', 'markets'], ['portfolio', '', 'portfolio'], ['voting', '', 'voting'], ['votecoin', '', 'votecoin'], ['decisions', '', 'decisions'], ['create', 'decision', 'list-decision'], ['payouts', '', 'payouts'], ['transactions', '', 'activity'], ['peg', 'deposit', 'drivechains'], ['explorer', '', 'explorer'], ['settings', 'dropwallet', 'settings-wallet']]) {
        await go(page, tab);
        await sleep(1200);
        await snapshot(`hivemind-${mode}-${name}`);
      }
    }
    await theme('midnight');

    // ---- the backup phrase shown again: a question of the trusted dialog, never words in the page
    await go('settings', 'dropwallet');
    await host('test.confirmAnswer', { yes: true });
    await click('Show the phrase');
    await sleep(1000);
    const shownQ = (await host('test.confirmed')).filter((q) => q.kind === 'node:phrase.show');
    await check('showing the phrase is a question of the trusted dialog', shownQ.length === 1);

    // ---- saved to a file the user chose: asked, then written (the runner checks the file: owner-only, the words)
    await host('test.fileAnswer', { path: window.__E2E.saveFile });
    await host('test.confirmAnswer', { yes: true });
    await click('Save it to a file');
    await waitFor('the phrase saved', async () => (await host('test.confirmed')).some((q) => q.kind === 'node:phrase.save'), 30000);
    await sleep(1500);
  } catch (e) {
    await check('the run', false, e && e.stack ? e.stack : String(e));
  }
  await finish();
})();
