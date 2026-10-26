//! `hm_oracle simulate <seed> <blocks> [--testing N]`: a random chain built and connected by
//! THEIR code (truthcoin-dc v0.20.0 State, Archive and Wallet), written as operations that
//! `truthcoin-replay sim` repeats on our state machine:
//!
//!   {"op":"check","atx":hex,"ok":bool}           validate_transaction on the current tip
//!   {"op":"connect","height","block","main_ts","twpd","digest"}
//!   {"op":"badblock","block","main_ts","error"}  prevalidate of a corrupted block (error|null)
//!   {"op":"disconnect","twpd","digest"}          disconnect_two_way_peg_data + disconnect_tip
//!
//! Three wallets act: deposits, transfers, decision claims (standard and not, every type),
//! markets (binary, multi, categorical; with and without new claims, TX-PoW), buys, sells,
//! amplify beta, votes, ballots, reputation transfers and withdrawals with bundle events;
//! a third of the attempts are mutated (bad values, wrong periods, ...) before signing.
#![allow(clippy::all)]

use std::collections::{BTreeMap, HashMap};
use std::io::Write;

use bitcoin::hashes::Hash as _;
use hashlink::LinkedHashMap;
use rand::{Rng as _, RngExt as _, SeedableRng as _, rngs::ChaCha20Rng};
use truthcoin_dc::{
    archive::Archive,
    state::{
        State,
        markets::block::{TradeApplyResult, TradeSimulation},
    },
    types::{
        Address, AuthorizedTransaction, BallotItem, Block, BlockHash, Body, ClaimDecisionPayload, Coinbase,
        DecisionClaimEntry, Header, M6id, OutPoint, Output, OutputContent, Transaction, TransactionData,
        WithdrawalBundleEvent, WithdrawalBundleEventStatus,
        authorization::BatchVerificationContext,
        proto::mainchain::{BlockEvent, BlockHeaderInfo, BlockInfo, Deposit, TwoWayPegData},
    },
    wallet::{CreateMarketInput, DecisionClaimInput, Wallet},
};
use truthcoin_dc_types::{decision::DecisionId, decision::DecisionType, market::MarketId};

fn twpd_json(d: &TwoWayPegData) -> serde_json::Value {
    let mut out = Vec::new();
    for (main, info) in &d.block_info {
        let mut evs = Vec::new();
        for e in &info.events {
            match e {
                BlockEvent::Deposit(dep) => {
                    let addr = dep.output.address.to_string();
                    let value = match dep.output.content {
                        OutputContent::Value(v) => v.to_sat(),
                        _ => unreachable!(),
                    };
                    evs.push(serde_json::json!({"deposit": {
                        "tx_index": dep.tx_index, "txid": dep.outpoint.txid.to_string(), "vout": dep.outpoint.vout,
                        "address_hex": const_hex::encode(addr.as_bytes()), "value": value}}));
                }
                BlockEvent::WithdrawalBundle(w) => {
                    let status = match w.status {
                        WithdrawalBundleEventStatus::Submitted => "submitted",
                        WithdrawalBundleEventStatus::Confirmed => "confirmed",
                        WithdrawalBundleEventStatus::Failed => "failed",
                    };
                    evs.push(serde_json::json!({"bundle": {"m6id": w.m6id.0.to_string(), "status": status}}));
                }
            }
        }
        out.push(serde_json::json!({"main": main.to_string(), "bmm": null, "events": evs}));
    }
    serde_json::Value::Array(out)
}

struct Actor {
    wallet: Wallet,
}

struct Applied {
    header: Header,
    body: Body,
    twpd: TwoWayPegData,
}

struct Sim {
    env: sneed::Env,
    state: State,
    archive: Archive,
    ctxt: BatchVerificationContext,
    rng: ChaCha20Rng,
    actors: Vec<Actor>,
    chain: Vec<Applied>,
    main_counter: u32,
    last_main: Option<bitcoin::BlockHash>,
    deposit_seq: u64,
    pending_events: Vec<BlockEvent>,
    stats: BTreeMap<String, (u32, u32)>,
    out: std::io::StdoutLock<'static>,
    testing: u32,
    halted: bool,
}

fn ensure_main(archive: &Archive, last: Option<bitcoin::BlockHash>, w: &mut sneed::RwTxn, pm: bitcoin::BlockHash) -> anyhow::Result<()> {
    if archive.try_get_main_header_info(w, &pm)?.is_none() {
        let info = BlockHeaderInfo {
            block_hash: pm,
            prev_block_hash: last.unwrap_or_else(bitcoin::BlockHash::all_zeros),
            height: 0,
            work: bitcoin::Target::MAX.to_work(),
            timestamp: 0,
        };
        archive.put_main_header_info(w, &info)?;
    }
    Ok(())
}

fn fake_main(n: u32) -> bitcoin::BlockHash {
    let mut b = [0u8; 32];
    b[0] = 0xee;
    b[1..5].copy_from_slice(&n.to_le_bytes());
    bitcoin::BlockHash::from_byte_array(b)
}

impl Sim {
    fn emit(&mut self, v: serde_json::Value) -> anyhow::Result<()> {
        writeln!(self.out, "{}", v)?;
        Ok(())
    }

    fn tip(&self) -> anyhow::Result<Option<BlockHash>> {
        let rotxn = self.env.read_txn()?;
        Ok(self.state.try_get_tip(&rotxn)?)
    }

    fn sync_wallets(&self) -> anyhow::Result<()> {
        let rotxn = self.env.read_txn()?;
        let utxos = self.state.get_utxos(&rotxn)?;
        for a in &self.actors {
            let addrs = a.wallet.get_addresses()?;
            let mine: HashMap<OutPoint, Output> =
                utxos.iter().filter(|(_, o)| addrs.contains(&o.address)).map(|(k, v)| (*k, v.clone())).collect();
            a.wallet.sync_confirmed(&mine, &[])?;
        }
        Ok(())
    }

    fn current_period(&self) -> anyhow::Result<u32> {
        let rotxn = self.env.read_txn()?;
        let h = self.state.try_get_height(&rotxn)?;
        let g = self.state.try_get_genesis_timestamp(&rotxn)?.unwrap_or(0);
        let ts = self.state.try_get_mainchain_timestamp(&rotxn)?.unwrap_or(0);
        Ok(self.state.decisions().get_current_period(ts, h, g)?)
    }

    fn pick<T: Clone>(&mut self, v: &[T]) -> Option<T> {
        if v.is_empty() {
            None
        } else {
            Some(v[self.rng.random_range(0..v.len())].clone())
        }
    }

    fn label(&mut self) -> String {
        let n = self.rng.random_range(1..12);
        (0..n).map(|_| (b'a' + self.rng.random_range(0..26u8)) as char).collect()
    }

    fn decision_type(&mut self) -> DecisionType {
        match self.rng.random_range(0..5u8) {
            0 => DecisionType::Scaled { min: -10.0, max: 90.0, increment: 0.5 },
            1 => DecisionType::Category { options: vec!["a".into(), "b".into(), "c".into()] },
            _ => DecisionType::Binary,
        }
    }

    fn claim_entries(&mut self, period: u32, n: usize) -> anyhow::Result<Vec<DecisionClaimEntry>> {
        let rotxn = self.env.read_txn()?;
        let avail = self.state.get_available_decisions_in_period(&rotxn, period).unwrap_or_default();
        drop(rotxn);
        let mut v = Vec::new();
        for _ in 0..n {
            let id = if self.rng.random_bool(0.15) || avail.is_empty() {
                // non-standard, or a random id (often invalid)
                let idx = self.rng.random_range(0..600u32);
                match DecisionId::new(self.rng.random_bool(0.3), period, idx) {
                    Ok(d) => d,
                    Err(_) => continue,
                }
            } else {
                avail[self.rng.random_range(0..avail.len())]
            };
            let header = self.label();
            let description = self.label();
            v.push(DecisionClaimEntry {
                decision_id_bytes: id.as_bytes(),
                header,
                description,
                option_0_label: if self.rng.random_bool(0.3) { Some("no".into()) } else { None },
                option_1_label: if self.rng.random_bool(0.3) { Some("yes".into()) } else { None },
                option_labels: None,
                tags: if self.rng.random_bool(0.3) { Some(vec!["t".into()]) } else { None },
            });
        }
        Ok(v)
    }

    fn claimed(&self) -> anyhow::Result<Vec<(DecisionId, DecisionType, bool)>> {
        let rotxn = self.env.read_txn()?;
        let mut r = Vec::new();
        for e in self.state.decisions().get_all_claimed_decisions(&rotxn)? {
            let voting = self.state.is_decision_in_voting(&rotxn, e.decision_id)?;
            if let Some(d) = e.decision {
                r.push((e.decision_id, d.decision_type.clone(), voting));
            }
        }
        Ok(r)
    }

    fn markets(&self) -> anyhow::Result<Vec<truthcoin_dc::state::Market>> {
        let rotxn = self.env.read_txn()?;
        Ok(self.state.markets().get_all_markets(&rotxn)?)
    }

    /// One random transaction from actor `ai` (None: nothing to do / builder refused).
    fn make_tx(&mut self, ai: usize) -> anyhow::Result<Option<(String, Transaction)>> {
        let fee = bitcoin::Amount::from_sat(self.rng.random_range(0..3000u64) + 1000);
        let period = self.current_period()?;
        let kind = self.rng.random_range(0..12u8);
        let ai = if (8..=10).contains(&kind) && self.rng.random_bool(0.8) {
            let rotxn = self.env.read_txn()?;
            let mut with_rep = vec![];
            for (i, a) in self.actors.iter().enumerate() {
                if self.state.reputation().get_reputation(&rotxn, &a.wallet.voter_address()?)? > 0.0 {
                    with_rep.push(i);
                }
            }
            drop(rotxn);
            self.pick(&with_rep).unwrap_or(ai)
        } else {
            ai
        };
        let w = &self.actors[ai].wallet;
        let res: Result<(String, Transaction), truthcoin_dc::wallet::Error> = match kind {
            0 => {
                let to = self.actors[self.rng.random_range(0..self.actors.len())].wallet.get_new_address()?;
                let acc = { let rotxn = self.env.read_txn()?; self.state.get_accumulator(&rotxn)? };
                let w = &self.actors[ai].wallet;
                w.create_transaction(&acc, false, to, bitcoin::Amount::from_sat(self.rng.random_range(1000..200_000u64)), fee)
                    .map(|t| ("transfer".to_string(), t))
            }
            1 | 2 => {
                let p = period + if self.rng.random_bool(0.8) { 0 } else { self.rng.random_range(0..4u32) };
                let n = if self.rng.random_bool(0.85) { 1 } else { self.rng.random_range(2..4usize) };
                let mut decisions = self.claim_entries(p, n)?;
                let decision_type = self.decision_type();
                if let DecisionType::Category { options } = &decision_type {
                    if self.rng.random_bool(0.9) {
                        for d in decisions.iter_mut() {
                            d.option_labels = Some(options.clone());
                        }
                    }
                }
                let big = bitcoin::Amount::from_sat(self.rng.random_range(0..60_000u64) + 2000);
                self.actors[ai].wallet.claim_decision(false, DecisionClaimInput { decision_type, decisions }, big)
                    .map(|t| ("claim".to_string(), t))
            }
            3 | 4 => {
                let claimed: Vec<_> = self.claimed()?.into_iter().filter(|(_, _, v)| !v).collect();
                let new_claims = if self.rng.random_bool(0.3) || claimed.is_empty() {
                    let k = self.rng.random_range(1..3usize);
                    let decisions = self.claim_entries(period, k)?;
                    vec![ClaimDecisionPayload { decision_type: DecisionType::Binary, decisions }]
                } else {
                    vec![]
                };
                let mut ids: Vec<String> = new_claims
                    .iter()
                    .flat_map(|c| c.decisions.iter().map(|d| const_hex::encode(d.decision_id_bytes)))
                    .collect();
                for _ in 0..self.rng.random_range(0..3usize) {
                    if let Some((d, _, _)) = self.pick(&claimed) {
                        let s = const_hex::encode(d.as_bytes());
                        if !ids.contains(&s) {
                            ids.push(s);
                        }
                    }
                }
                if ids.is_empty() {
                    return Ok(None);
                }
                let (dimensions, cats) = match self.rng.random_range(0..3u8) {
                    0 if ids.len() >= 2 => (format!("[[{}]]", ids.join(",")), Some(vec![ids.len() + 1])),
                    1 => (format!("[{}]", ids.join(",")), None),
                    _ => (format!("[{}]", ids[0]), None),
                };
                let pow = self.rng.random_bool(0.2);
                let input = CreateMarketInput {
                    title: self.label(),
                    description: self.label(),
                    dimensions,
                    beta: None,
                    trading_fee: if self.rng.random_bool(0.5) { Some(self.rng.random_range(0..5u32) as f64 / 100.0) } else { None },
                    initial_liquidity: if self.rng.random_bool(0.7) { Some(self.rng.random_range(2_000..300_000u64)) } else { None },
                    category_option_counts: cats,
                    tx_pow_hash_selector: if pow { Some(self.rng.random_range(1..4u8)) } else { None },
                    tx_pow_ordering: if pow { Some(self.rng.random_range(0..3u8)) } else { None },
                    tx_pow_difficulty: if pow { Some(self.rng.random_range(1..6u8)) } else { None },
                    new_claims,
                };
                let big = bitcoin::Amount::from_sat(self.rng.random_range(0..40_000u64) + 1000);
                self.actors[ai].wallet.create_market(false, input, big).map(|(t, _)| ("market".to_string(), t))
            }
            5 | 6 => {
                let ms: Vec<_> = self.markets()?.into_iter().filter(|m| m.market_state == truthcoin_dc::state::MarketState::Trading).collect();
                let Some(m) = self.pick(&ms) else { return Ok(None) };
                let n = m.shares.len();
                let outcome = self.rng.random_range(0..n.max(1));
                let sell = self.rng.random_bool(0.3);
                let shares: i64 = if sell { -(self.rng.random_range(1..30i64)) } else { self.rng.random_range(1..60i64) };
                let mut trader = self.actors[ai].wallet.get_or_generate_last_address()?;
                let mut outcome = outcome;
                if sell {
                    // a position this wallet holds in this market, when there is one
                    let addrs = self.actors[ai].wallet.get_addresses()?;
                    let rotxn = self.env.read_txn()?;
                    let held: Vec<(Address, u32)> = self
                        .state
                        .markets()
                        .get_all_share_accounts(&rotxn)?
                        .into_iter()
                        .filter(|(a, _)| addrs.contains(a))
                        .flat_map(|(a, pos)| pos.into_iter().filter(|(mid, _, sh)| *mid == m.id && *sh > 0).map(move |(_, o, _)| (a, o)))
                        .collect();
                    drop(rotxn);
                    if let Some((a, o)) = self.pick(&held) {
                        trader = a;
                        outcome = o as usize;
                    }
                }
                let limit = if sell { self.rng.random_range(0..2000u64) } else { self.rng.random_range(2000..400_000u64) };
                let pow = if m.tx_pow_difficulty > 0 {
                    Some(truthcoin_dc::types::tx_pow::TxPowConfig {
                        hash_selector: m.tx_pow_hash_selector,
                        ordering: m.tx_pow_ordering,
                        difficulty: m.tx_pow_difficulty,
                    })
                } else {
                    None
                };
                let tip = self.tip()?.unwrap();
                let prev = if self.rng.random_bool(0.1) && self.chain.len() > 3 {
                    self.chain[self.chain.len() - 1 - self.rng.random_range(0..self.chain.len().min(14))].header.hash()
                } else {
                    tip
                };
                self.actors[ai].wallet.trade(false, m.id, outcome, shares, trader, limit, pow, prev).map(|t| ("trade".to_string(), t))
            }
            7 => {
                let ms: Vec<_> = self.markets()?.into_iter().filter(|m| m.market_state == truthcoin_dc::state::MarketState::Trading || self.rng.random_bool(0.1)).collect();
                let Some(m) = self.pick(&ms) else { return Ok(None) };
                let amt = self.rng.random_range(1..50_000u64);
                let mut owner = ai;
                for (i, a) in self.actors.iter().enumerate() {
                    if a.wallet.get_addresses()?.contains(&m.creator_address) {
                        owner = i;
                    }
                }
                // a coin at the author's address: pay one there first when there is none
                let has = self.actors[owner].wallet.get_utxos()?.values().any(|o| o.address == m.creator_address);
                if !has {
                    let acc = { let rotxn = self.env.read_txn()?; self.state.get_accumulator(&rotxn)? };
                    return Ok(self.actors[owner].wallet
                        .create_transaction(&acc, false, m.creator_address, bitcoin::Amount::from_sat(80_000), fee)
                        .ok()
                        .map(|t| ("fund-author".to_string(), t)));
                }
                self.actors[owner].wallet.amplify_beta(false, m.id, amt, m.creator_address).map(|t| ("amplify".to_string(), t))
            }
            8 | 9 => {
                let voting: Vec<_> = self.claimed()?.into_iter().filter(|(_, _, v)| *v).collect();
                if voting.is_empty() {
                    return Ok(None);
                }
                let mut votes = Vec::new();
                for _ in 0..self.rng.random_range(1..4usize) {
                    let (d, t, _) = self.pick(&voting).unwrap();
                    if votes.iter().any(|v: &BallotItem| v.decision_id_bytes == d.as_bytes()) && self.rng.random_bool(0.9) {
                        continue;
                    }
                    let vote_value = match t {
                        DecisionType::Binary => if self.rng.random_bool(0.8) { self.rng.random_range(0..2u8) as f64 } else { 0.5 },
                        DecisionType::Scaled { .. } => self.rng.random_range(0..21u32) as f64 / 20.0,
                        DecisionType::Category { .. } => self.rng.random_range(0..4u8) as f64,
                    };
                    votes.push(BallotItem { decision_id_bytes: d.as_bytes(), vote_value });
                }
                let vp = voting[0].0.voting_period();
                if kind == 8 {
                    let w = &self.actors[ai].wallet;
                    let voter = w.voter_address()?;
                    let rep = { let rotxn = self.env.read_txn()?; self.state.reputation().get_reputation(&rotxn, &voter)? };
                    let _ = rep;
                    w.transfer_reputation(false, voter, 0.0, fee).map(|mut t| {
                        t.data = Some(TransactionData::SubmitVote {
                            voter,
                            decision_id_bytes: votes[0].decision_id_bytes,
                            vote_value: votes[0].vote_value,
                            voting_period: vp,
                        });
                        ("vote".to_string(), t)
                    })
                } else {
                    self.actors[ai].wallet.submit_ballot(false, votes, vp, fee).map(|t| ("ballot".to_string(), t))
                }
            }
            10 => {
                let others: Vec<usize> = (0..self.actors.len()).filter(|i| *i != ai || self.rng.random_bool(0.1)).collect();
                let di = self.pick(&others).unwrap();
                let dest = self.actors[di].wallet.voter_address()?;
                let amount = [0.1, 0.25, 0.3, 0.05, 0.0001][self.rng.random_range(0..5usize)];
                self.actors[ai].wallet.transfer_reputation(false, dest, amount, fee).map(|t| ("reputation".to_string(), t))
            }
            _ => {
                let acc = { let rotxn = self.env.read_txn()?; self.state.get_accumulator(&rotxn)? };
                let addr: bitcoin::Address<bitcoin::address::NetworkUnchecked> =
                    "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq".parse().unwrap();
                let w = &self.actors[ai].wallet;
                w.create_withdrawal(&acc, false, addr, bitcoin::Amount::from_sat(self.rng.random_range(5_000..60_000u64)),
                    bitcoin::Amount::from_sat(self.rng.random_range(500..3000u64)), fee)
                    .map(|t| ("withdrawal".to_string(), t))
            }
        };
        match res {
            Ok(x) => Ok(Some(x)),
            Err(_) => Ok(None),
        }
    }

    fn mutate(&mut self, t: &mut Transaction) {
        let r = self.rng.random_range(0..10u8);
        match &mut t.data {
            Some(TransactionData::Trade { shares, outcome_index, limit_sats, prev_block_hash, .. }) => match r {
                0 => *shares = 0,
                1 => *outcome_index += 7,
                2 => *limit_sats = 0,
                3 => *prev_block_hash = BlockHash([7u8; 32]),
                _ => *shares = -*shares,
            },
            Some(TransactionData::SubmitVote { vote_value, voting_period, .. }) => match r {
                0 => *vote_value = 2.0,
                1 => *voting_period += 1,
                2 => *vote_value = -1e300,
                _ => *vote_value = 0.25,
            },
            Some(TransactionData::SubmitBallot { votes, voting_period, .. }) => match r {
                0 => votes.clear(),
                1 => *voting_period = voting_period.wrapping_sub(1),
                2 => votes.push(votes.first().cloned().unwrap_or(BallotItem { decision_id_bytes: [0; 3], vote_value: 1.0 })),
                _ => {
                    if let Some(v) = votes.first_mut() {
                        v.vote_value = 7.5
                    }
                }
            },
            Some(TransactionData::TransferReputation { amount, .. }) => *amount = [2.0, -0.5, 0.0, f64::INFINITY, 1e-12][(r % 5) as usize],
            Some(TransactionData::AmplifyBeta { amount, .. }) => *amount = if r < 5 { 0 } else { u64::MAX },
            Some(TransactionData::CreateMarket { trading_fee, tx_pow_difficulty, title, .. }) => match r {
                0 => *trading_fee = Some(0.9),
                1 => *tx_pow_difficulty = Some(99),
                2 => *title = String::new(),
                _ => *trading_fee = Some(-0.1),
            },
            Some(TransactionData::ClaimDecision(p)) => match r {
                0 => p.decisions.clear(),
                1 => {
                    if let Some(d) = p.decisions.first().cloned() {
                        p.decisions.push(d)
                    }
                }
                2 => p.decision_type = DecisionType::Scaled { min: 5.0, max: 1.0, increment: 1.0 },
                _ => {
                    if let Some(d) = p.decisions.first_mut() {
                        d.header = String::new()
                    }
                }
            },
            _ => {
                if let Some(o) = t.outputs.0.first_mut() {
                    if let OutputContent::Value(v) = &mut o.content {
                        *v = v.checked_add(bitcoin::Amount::from_sat(1)).unwrap_or(*v);
                    }
                }
            }
        }
    }

    fn step(&mut self, n_txs: usize) -> anyhow::Result<()> {
        let tip = self.tip()?;
        // candidate transactions
        let mut cands: Vec<(String, AuthorizedTransaction)> = Vec::new();
        if tip.is_some() {
            for _ in 0..n_txs {
                let ai = self.rng.random_range(0..self.actors.len());
                let Some((kind, mut tx)) = self.make_tx(ai)? else { continue };
                let mutated = self.rng.random_bool(0.3);
                if mutated {
                    self.mutate(&mut tx);
                }
                // the builder may have used another actor (voter, market author)
                let mut found = None;
                for k in 0..self.actors.len() {
                    let i = (ai + k) % self.actors.len();
                    if let Ok(a) = self.actors[i].wallet.authorize(rand::rng(), tx.clone()) {
                        found = Some((i, a));
                        break;
                    }
                }
                let Some((ai, atx)) = found else { continue };
                let (ok, err) = {
                    let rotxn = self.env.read_txn()?;
                    let mut atx2 = atx.clone();
                    // proofs are not committed; their submit regenerates them
                    let _ = self.state.regenerate_proof(&rotxn, &HashMap::new(), &mut atx2.transaction);
                    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        self.state.validate_transaction(&rotxn, &self.ctxt, &HashMap::new(), &atx2, &self.archive)
                    }));
                    match r {
                        Ok(Ok(_)) => (true, String::new()),
                        Ok(Err(e)) => (false, format!("{e:#}")),
                        Err(_) => (false, "panic".to_string()),
                    }
                };
                let label = format!("{kind}{}", if mutated { "*" } else { "" });
                if std::env::var("HM_SIM_DEBUG").is_ok() && !ok {
                    eprintln!("{label}: {err}");
                }
                let e = self.stats.entry(label).or_default();
                e.0 += 1;
                if ok {
                    e.1 += 1;
                }
                let line = serde_json::json!({"op": "check", "atx": const_hex::encode(bincode::serialize(&atx)?), "ok": ok, "error": err});
                self.emit(line)?;
                // inputs are taken by this candidate
                let ins: Vec<OutPoint> = atx.transaction.inputs.iter().map(|(o, _)| *o).collect();
                self.actors[ai].wallet.delete_utxos(&ins)?;
                if ok {
                    cands.push((kind, atx));
                }
            }
        }
        self.build_connect(cands, tip, 0)
    }

    fn build_connect(&mut self, cands: Vec<(String, AuthorizedTransaction)>, tip: Option<BlockHash>, attempt: u32) -> anyhow::Result<()> {
        let saved_pending = self.pending_events.clone();
        let all_cands = cands.clone();
        // block assembly: node/mod.rs get_transactions
        let rotxn = self.env.read_txn()?;
        let mut fee = bitcoin::Amount::ZERO;
        let mut txs = vec![];
        let mut spent = std::collections::HashSet::new();
        let mut trades = TradeSimulation::default();
        let mut block_outputs = HashMap::<OutPoint, Output>::new();
        // non-trades first, then trades
        let (tr, nontr): (Vec<_>, Vec<_>) = cands.into_iter().partition(|(_, a)| matches!(a.transaction.data, Some(TransactionData::Trade { .. })));
        for (_, mut atx) in nontr.into_iter().chain(tr) {
            if atx.transaction.inputs.iter().any(|(o, _)| spent.contains(o)) {
                continue;
            }
            let _ = self.state.regenerate_proof(&rotxn, &block_outputs, &mut atx.transaction);
            if self.state.validate_transaction(&rotxn, &self.ctxt, &block_outputs, &atx, &self.archive).is_err() {
                continue;
            }
            let filled = self.state.fill_authorized_transaction(&rotxn, &block_outputs, atx)?;
            // Their get_transactions propagates an apply error and fails the whole template;
            // here the transaction is dropped (and counted) so the run goes on.
            match trades.apply(&self.state, &self.archive, &rotxn, &filled.transaction) {
                Ok(TradeApplyResult::Applied) => (),
                Ok(TradeApplyResult::Skipped { .. }) => continue,
                Err(_) => {
                    self.stats.entry("template-apply-error".into()).or_default().0 += 1;
                    continue;
                }
            }
            block_outputs.extend(filled.transaction.transaction.outputs_by_outpoint());
            fee = fee + truthcoin_dc::validation::miner_fee(&filled.transaction)?;
            spent.extend(filled.transaction.transaction.inputs.iter().map(|(o, _)| *o));
            txs.push(filled);
        }
        let height = self.state.try_get_height(&rotxn)?;
        let acc = self.state.get_accumulator(&rotxn)?;
        drop(rotxn);
        let is_genesis = height.is_none();
        let a0 = &self.actors[0].wallet;
        let outputs = if is_genesis {
            vec![Output { address: a0.voter_address()?, content: OutputContent::Value(fee) }]
        } else if fee > bitcoin::Amount::ZERO {
            let ai = self.rng.random_range(0..self.actors.len());
            vec![Output { address: self.actors[ai].wallet.get_new_address()?, content: OutputContent::Value(fee) }]
        } else {
            vec![]
        };
        let coinbase = Coinbase { memo: vec![], outputs: outputs.into() };
        self.main_counter += 1;
        let prev_main = fake_main(self.main_counter);
        let main_ts = 1_800_000_000u64 + 600 * self.main_counter as u64;
        let merkle_root = Body::compute_merkle_root(&coinbase, &txs)?;
        let mut acc2 = acc;
        let cb_txid = Coinbase::compute_txid(&merkle_root, &prev_main, tip.as_ref());
        Body::modify_memforest(cb_txid, coinbase.outputs.as_slice(), &txs, &mut acc2.0)?;
        let roots = acc2.0.get_roots().iter().map(|r| r.get_data()).collect();
        let body = Body::new(txs.into_iter().map(|t| t.into()).collect(), coinbase);
        let header = Header { merkle_root, roots, prev_side_hash: tip, prev_main_hash: prev_main };
        // 2WPD window: deposits for the actors, bundle events for a pending bundle
        let mut events = std::mem::take(&mut self.pending_events);
        if is_genesis || self.rng.random_bool(0.25) {
            for ai in 0..self.actors.len() {
                if !is_genesis && self.rng.random_bool(0.5) {
                    continue;
                }
                let address = if self.rng.random_bool(0.05) {
                    Address::ALL_ZEROS
                } else if is_genesis || self.rng.random_bool(0.35) {
                    self.actors[ai].wallet.voter_address()?
                } else {
                    self.actors[ai].wallet.get_new_address()?
                };
                let mut txid = [0u8; 32];
                self.rng.fill_bytes(&mut txid);
                events.push(BlockEvent::Deposit(Deposit {
                    tx_index: self.deposit_seq,
                    outpoint: bitcoin::OutPoint { txid: bitcoin::Txid::from_byte_array(txid), vout: self.rng.random_range(0..3) },
                    output: Output { address, content: OutputContent::Value(bitcoin::Amount::from_sat(self.rng.random_range(100_000..3_000_000u64))) },
                }));
                self.deposit_seq += 1;
            }
        }
        {
            let rotxn = self.env.read_txn()?;
            if let Some((bundle, _)) = self.state.try_get_pending_withdrawal_bundle(&rotxn)? {
                let m6id = bundle.compute_m6id();
                if self.rng.random_bool(0.5) {
                    events.push(BlockEvent::WithdrawalBundle(WithdrawalBundleEvent { m6id, status: WithdrawalBundleEventStatus::Submitted }));
                    let fin = if self.rng.random_bool(0.7) { WithdrawalBundleEventStatus::Confirmed } else { WithdrawalBundleEventStatus::Failed };
                    self.pending_events.push(BlockEvent::WithdrawalBundle(WithdrawalBundleEvent { m6id, status: fin }));
                }
            }
        }
        let mut twpd = TwoWayPegData { block_info: LinkedHashMap::new() };
        if !events.is_empty() {
            self.main_counter += 1;
            let ev_main = fake_main(self.main_counter);
            twpd.block_info.insert(ev_main, BlockInfo { bmm_commitment: None, events });
        }
        let block = Block { header: header.clone(), body: body.clone() };
        // sometimes: a corrupted copy first (prevalidate on the current tip)
        if self.rng.random_bool(0.15) {
            let mut bad = block.clone();
            match self.rng.random_range(0..4u8) {
                0 => bad.header.merkle_root = truthcoin_dc::types::MerkleRoot::from([1u8; 32]),
                1 => {
                    if let Some(o) = bad.body.coinbase.outputs.0.first_mut() {
                        o.content = OutputContent::Value(bitcoin::Amount::from_sat(1) + fee);
                    } else {
                        bad.body.coinbase.outputs.push(Output { address: Address::ALL_ZEROS, content: OutputContent::Value(bitcoin::Amount::from_sat(1)) });
                    }
                }
                2 => {
                    if let Some(t) = bad.body.transactions.first().cloned() {
                        bad.body.transactions.push(t);
                    }
                }
                _ => bad.body.authorizations.reverse(),
            }
            let rotxn = self.env.read_txn()?;
            let mut w = self.env.write_txn()?;
            let _ = ensure_main(&self.archive, self.last_main, &mut w, prev_main);
            let _ = self.archive.put_header(&mut w, &bad.header);
            let err = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                self.state.prevalidate_block(&w, &self.ctxt, &bad.header, &bad.body, &self.archive)
            })) {
                Ok(Ok(_)) => serde_json::Value::Null,
                Ok(Err(e)) => serde_json::Value::String(format!("{e:#}")),
                Err(_) => serde_json::Value::String("panic".into()),
            };
            w.abort();
            drop(rotxn);
            let line = serde_json::json!({"op": "badblock", "block": const_hex::encode(bincode::serialize(&bad)?), "main_ts": main_ts, "error": err});
            self.emit(line)?;
        }
        // connect
        let mut rwtxn = self.env.write_txn()?;
        ensure_main(&self.archive, self.last_main, &mut rwtxn, prev_main)?;
        self.archive.put_header(&mut rwtxn, &header)?;
        let pre = match self.state.prevalidate_block(&rwtxn, &self.ctxt, &header, &body, &self.archive) {
            Ok(p) => p,
            Err(e) => {
                // Their template validates against the tip, prevalidate at the next height:
                // a claim or market whose period window moved makes the block invalid.
                rwtxn.abort();
                let line = serde_json::json!({"op": "badblock", "block": const_hex::encode(bincode::serialize(&block)?), "main_ts": main_ts, "error": format!("{e:#}")});
                self.emit(line)?;
                self.stats.entry("template-invalid".into()).or_default().0 += 1;
                self.pending_events = saved_pending;
                if attempt >= 6 {
                    self.emit(serde_json::json!({"op": "halt", "error": format!("{e:#}")}))?;
                    self.halted = true;
                    return Ok(());
                }
                let keep: Vec<_> = if attempt == 0 {
                    all_cands.into_iter().filter(|(_, a)| !matches!(a.transaction.data, Some(TransactionData::ClaimDecision(_)) | Some(TransactionData::CreateMarket { .. }))).collect()
                } else {
                    vec![]
                };
                return self.build_connect(keep, tip, attempt + 1);
            }
        };
        let connected = self
            .state
            .connect_prevalidated_block(&mut rwtxn, &header, &body, pre, main_ts)
            .map(|_| ())
            .and_then(|()| self.state.connect_two_way_peg_data(&mut rwtxn, &twpd));
        if let Err(e) = connected {
            // prevalidated, then refused while connecting (e.g. two reputation transfers of
            // one sender that each validated alone): their node rejects the block
            rwtxn.abort();
            let line = serde_json::json!({"op": "badconnect", "block": const_hex::encode(bincode::serialize(&block)?), "main_ts": main_ts, "twpd": twpd_json(&twpd), "error": format!("{e:#}")});
            self.emit(line)?;
            self.stats.entry("template-connect-fails".into()).or_default().0 += 1;
            self.pending_events = saved_pending;
            if all_cands.is_empty() || attempt >= 6 {
                // even an empty block cannot connect at this height: their chain stops here
                self.emit(serde_json::json!({"op": "halt", "error": format!("{e:#}")}))?;
                self.halted = true;
                return Ok(());
            }
            let mut keep = all_cands;
            if attempt < 3 && !keep.is_empty() {
                let k = self.rng.random_range(0..keep.len());
                keep.remove(k);
            } else {
                keep.clear();
            }
            return self.build_connect(keep, tip, attempt + 1);
        }
        self.archive.put_body(&mut rwtxn, header.hash(), &body)?;
        let acc = self.state.get_accumulator(&rwtxn)?;
        self.archive.put_accumulator(&mut rwtxn, header.hash(), &acc)?;
        rwtxn.commit()?;
        self.last_main = Some(prev_main);
        let d = crate::digest(&self.env, &self.state)?;
        let line = serde_json::json!({"op": "connect", "height": height.map_or(0, |h| h + 1), "block": const_hex::encode(bincode::serialize(&block)?), "main_ts": main_ts, "twpd": twpd_json(&twpd), "digest": d});
        self.emit(line)?;
        self.chain.push(Applied { header, body, twpd });
        self.sync_wallets()?;
        Ok(())
    }

    fn disconnect(&mut self) -> anyhow::Result<()> {
        let Some(a) = self.chain.pop() else { return Ok(()) };
        let mut rwtxn = self.env.write_txn()?;
        self.state.disconnect_two_way_peg_data(&mut rwtxn, &a.twpd)?;
        self.state.disconnect_tip(&mut rwtxn, &a.header, &a.body)?;
        match self.state.try_get_tip(&rwtxn)? {
            Some(t) => {
                let acc = self.archive.get_accumulator(&rwtxn, t)?;
                self.state.utreexo_accumulator.put(&mut rwtxn, &(), &acc)?;
            }
            None => {
                self.state.utreexo_accumulator.delete(&mut rwtxn, &())?;
            }
        }
        rwtxn.commit()?;
        // bundle events of the disconnected window happen again later
        self.pending_events.clear();
        let d = crate::digest(&self.env, &self.state)?;
        let line = serde_json::json!({"op": "disconnect", "twpd": twpd_json(&a.twpd), "digest": d});
        self.emit(line)?;
        self.stats.entry("disconnect".into()).or_default().0 += 1;
        self.sync_wallets()?;
        Ok(())
    }
}

pub fn simulate(seed: u64, blocks: u32, testing: u32) -> anyhow::Result<()> {
    let dir = crate::tempfile_dir()?;
    let mut opts = heed::EnvOpenOptions::new();
    opts.map_size(4 * 1024 * 1024 * 1024).max_dbs(State::NUM_DBS + Archive::NUM_DBS);
    let env = unsafe { sneed::Env::open(&opts, &dir) }?;
    let state = State::new(&env, Some(testing))?;
    let archive = Archive::new(&env)?;
    let mut rng = ChaCha20Rng::seed_from_u64(seed);
    let mut actors = Vec::new();
    for i in 0..3 {
        let wallet = Wallet::new(&dir.join(format!("wallet{i}")))?;
        let mut s = [0u8; 64];
        rng.fill_bytes(&mut s);
        wallet.set_seed(&s)?;
        actors.push(Actor { wallet });
    }
    let out: &'static std::io::Stdout = Box::leak(Box::new(std::io::stdout()));
    let mut sim = Sim {
        env,
        state,
        archive,
        ctxt: BatchVerificationContext::new(&mut rand::rng()),
        rng,
        actors,
        chain: vec![],
        main_counter: 0,
        last_main: None,
        deposit_seq: 0,
        pending_events: vec![],
        stats: BTreeMap::new(),
        out: out.lock(),
        testing,
        halted: false,
    };
    let _ = sim.testing;
    while (sim.chain.len() as u32) < blocks && !sim.halted {
        if sim.chain.len() > 3 && sim.rng.random_bool(0.06) {
            let k = sim.rng.random_range(1..4usize);
            for _ in 0..k {
                sim.disconnect()?;
            }
        }
        let n = sim.rng.random_range(0..9usize);
        sim.step(n)?;
    }
    for (k, (n, ok)) in &sim.stats {
        eprintln!("{k}: {n} attempted, {ok} valid");
    }
    let rotxn = sim.env.read_txn()?;
    eprintln!("markets {}, claimed {}", sim.state.markets().get_all_markets(&rotxn)?.len(), sim.state.decisions().get_all_claimed_decisions(&rotxn)?.len());
    drop(rotxn);
    let _ = std::fs::remove_dir_all(&dir);
    Ok(())
}
