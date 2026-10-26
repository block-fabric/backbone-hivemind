//! Byte-level oracle for the Hivemind drop-in: THEIR truthcoin-dc v0.20.0 crates.
//!
//!   hm_oracle vectors <seed> <n>                 random txs/blocks: borsh, bincode, txid, serde_json
//!   hm_oracle replay <fixture.jsonl> [--testing N]  their State over a block fixture; one digest per block
//!
//! The digest is what tests/oracle/compare.py diffs against `truthcoin-replay` (ours).
#![allow(clippy::all)]

mod sim;

use std::collections::BTreeMap;
use std::io::{BufRead, Write};
use std::str::FromStr;

use bitcoin::hashes::Hash as _;
use hashlink::LinkedHashMap;
use rand::{Rng as _, RngExt as _, SeedableRng as _, rngs::ChaCha20Rng};
use truthcoin_dc::{
    archive::Archive,
    state::State,
    types::{
        proto::mainchain::{BlockEvent, BlockHeaderInfo, BlockInfo, Deposit, TwoWayPegData},
        Address, AuthorizedTransaction, BallotItem, Block, BlockHash, Body,
        ClaimDecisionPayload, Coinbase, DecisionClaimEntry, Header, OutPoint,
        Output, OutputContent, Transaction, TransactionData,
        WithdrawalBundleEvent, WithdrawalBundleEventStatus,
        authorization::{self, BatchVerificationContext, SigningKey},
    },
};
use truthcoin_dc_types::{
    decision::{DecisionId, DecisionType},
    market::{DimensionSpec, MarketId},
};

fn hex(b: &[u8]) -> String {
    const_hex::encode(b)
}

// ------------------------------------------------------------------ random values
struct Gen(ChaCha20Rng);

impl Gen {
    fn u8(&mut self) -> u8 {
        self.0.random()
    }
    fn u32(&mut self) -> u32 {
        self.0.random()
    }
    fn u64(&mut self) -> u64 {
        match self.0.random_range(0..4u8) {
            0 => self.0.random_range(0..1000u64),
            1 => u64::MAX - self.0.random_range(0..3u64),
            _ => self.0.random(),
        }
    }
    fn bool(&mut self) -> bool {
        self.0.random()
    }
    fn f64(&mut self) -> f64 {
        loop {
            let v = match self.0.random_range(0..6u8) {
                0 => 0.0,
                1 => 1.0,
                2 => self.0.random_range(0.0..1.0f64),
                3 => -0.0,
                4 => f64::from_bits(self.0.random()),
                _ => (self.0.random_range(0..1000u32) as f64) / 7.0,
            };
            if !v.is_nan() {
                return v;
            }
        }
    }
    fn string(&mut self) -> String {
        let n = self.0.random_range(0..12usize);
        let alphabet = ["a", "Z", "0", " ", "é", "€", "𝄞", "\"", "\\", "\n", "_"];
        (0..n).map(|_| alphabet[self.0.random_range(0..alphabet.len())]).collect()
    }
    fn opt<T>(&mut self, f: impl FnOnce(&mut Self) -> T) -> Option<T> {
        if self.bool() { Some(f(self)) } else { None }
    }
    fn arr<const N: usize>(&mut self) -> [u8; N] {
        let mut a = [0u8; N];
        self.0.fill_bytes(&mut a);
        a
    }
    fn address(&mut self) -> Address {
        Address(self.arr())
    }
    fn decision_id(&mut self) -> DecisionId {
        DecisionId::from_bytes(self.arr()).unwrap()
    }
    fn decision_type(&mut self) -> DecisionType {
        match self.0.random_range(0..3u8) {
            0 => DecisionType::Binary,
            1 => DecisionType::Scaled { min: self.f64(), max: self.f64(), increment: self.f64() },
            _ => DecisionType::Category { options: (0..self.0.random_range(0..4usize)).map(|_| self.string()).collect() },
        }
    }
    fn claim(&mut self) -> ClaimDecisionPayload {
        let n = self.0.random_range(0..3usize);
        ClaimDecisionPayload {
            decision_type: self.decision_type(),
            decisions: (0..n)
                .map(|_| DecisionClaimEntry {
                    decision_id_bytes: self.arr(),
                    header: self.string(),
                    description: self.string(),
                    option_0_label: self.opt(|g| g.string()),
                    option_1_label: self.opt(|g| g.string()),
                    option_labels: self.opt(|g| (0..g.0.random_range(0..3usize)).map(|_| g.string()).collect()),
                    tags: self.opt(|g| (0..g.0.random_range(0..3usize)).map(|_| g.string()).collect()),
                })
                .collect(),
        }
    }
    fn data(&mut self) -> TransactionData {
        match self.0.random_range(0..7u8) {
            0 => TransactionData::ClaimDecision(self.claim()),
            1 => TransactionData::CreateMarket {
                title: self.string(),
                description: self.string(),
                dimension_specs: (0..self.0.random_range(0..4usize))
                    .map(|_| if self.bool() { DimensionSpec::Single(self.decision_id()) } else { DimensionSpec::Categorical(self.decision_id()) })
                    .collect(),
                new_claims: (0..self.0.random_range(0..3usize)).map(|_| self.claim()).collect(),
                trading_fee: self.opt(|g| g.f64()),
                tx_pow_hash_selector: self.opt(|g| g.u8()),
                tx_pow_ordering: self.opt(|g| g.u8()),
                tx_pow_difficulty: self.opt(|g| g.u8()),
            },
            2 => TransactionData::Trade {
                market_id: MarketId(self.arr()),
                outcome_index: self.u32(),
                shares: self.u64() as i64,
                trader: self.address(),
                limit_sats: self.u64(),
                tx_pow_nonce: self.opt(|g| g.u64()),
                prev_block_hash: BlockHash(self.arr()),
            },
            3 => TransactionData::SubmitVote {
                voter: self.address(),
                decision_id_bytes: self.arr(),
                vote_value: self.f64(),
                voting_period: self.u32(),
            },
            4 => TransactionData::SubmitBallot {
                voter: self.address(),
                votes: (0..self.0.random_range(0..4usize))
                    .map(|_| BallotItem { decision_id_bytes: self.arr(), vote_value: self.f64() })
                    .collect(),
                voting_period: self.u32(),
            },
            5 => TransactionData::TransferReputation { sender: self.address(), dest: self.address(), amount: self.f64() },
            _ => TransactionData::AmplifyBeta { market_id: MarketId(self.arr()), amount: self.u64(), market_author: self.address() },
        }
    }
    fn outpoint(&mut self) -> OutPoint {
        match self.0.random_range(0..5u8) {
            0 => OutPoint::Regular { txid: self.arr::<32>().into(), vout: self.u32() },
            1 => OutPoint::Coinbase { txid: self.arr::<32>().into(), vout: self.u32() },
            2 => OutPoint::Deposit(bitcoin::OutPoint { txid: bitcoin::Txid::from_byte_array(self.arr()), vout: self.u32() }),
            3 => OutPoint::MarketFunds { market_id: self.arr(), block_height: self.u32(), is_fee: self.bool() },
            _ => OutPoint::Payout { hash: self.arr::<32>().into(), vout: self.u32() },
        }
    }
    fn output(&mut self) -> Output {
        const ADDRS: [&str; 5] = [
            "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq",
            "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2",
            "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy",
            "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx",
            "bc1p5d7rjq7g6rdk2yhzks9smlaqtedr4dekq08ge8ztwac72sfr9rusxg3297",
        ];
        let content = match self.0.random_range(0..3u8) {
            0 => OutputContent::Value(bitcoin::Amount::from_sat(self.u64())),
            1 => OutputContent::Withdrawal {
                value: bitcoin::Amount::from_sat(self.u64() / 4),
                main_fee: bitcoin::Amount::from_sat(self.u64() / 4),
                main_address: ADDRS[self.0.random_range(0..ADDRS.len())].parse().unwrap(),
            },
            _ => OutputContent::MarketFunds { market_id: self.arr(), amount: bitcoin::Amount::from_sat(self.u64()), is_fee: self.bool() },
        };
        Output { address: self.address(), content }
    }
    fn transaction(&mut self) -> Transaction {
        Transaction {
            inputs: (0..self.0.random_range(0..4usize)).map(|_| (self.outpoint(), self.arr())).collect::<Vec<_>>().into(),
            proof: Default::default(),
            outputs: (0..self.0.random_range(0..4usize)).map(|_| self.output()).collect::<Vec<_>>().into(),
            data: self.opt(|g| g.data()),
        }
    }
}

fn vectors(seed: u64, n: usize) -> anyhow::Result<()> {
    let mut g = Gen(ChaCha20Rng::seed_from_u64(seed));
    let out = std::io::stdout();
    let mut out = out.lock();
    for i in 0..n {
        let tx = g.transaction();
        // signed: one key per input address, plus an optional actor proof
        let keys: Vec<SigningKey> = (0..tx.inputs.len()).map(|_| SigningKey::new(&mut g.0)).collect();
        let addrs: Vec<(Address, &SigningKey)> =
            keys.iter().map(|k| (authorization::get_address(&k.into()), k)).collect();
        let mut atx: AuthorizedTransaction = authorization::authorize(&mut g.0, &addrs, tx.clone())?;
        if g.bool() {
            let k = SigningKey::new(&mut g.0);
            let a = authorization::get_address(&(&k).into());
            let mut p = authorization::authorize(&mut g.0, &[(a, &k)], Transaction::default())?;
            // an actor proof over THIS tx: sign it as a 1-input authorization
            let s = authorization::authorize(&mut g.0, &[(a, &k)], tx.clone())?;
            p.authorizations = s.authorizations;
            atx.actor_proof = p.authorizations.pop().map(Box::new);
        }
        let borsh = borsh::to_vec(&tx)?;
        let line = serde_json::json!({
            "i": i,
            "borsh": hex(&borsh),
            "bincode": hex(&bincode::serialize(&tx)?),
            "txid": tx.txid().to_string(),
            "json": serde_json::to_string(&tx)?,
            "atx_bincode": hex(&bincode::serialize(&atx)?),
            "atx_json": serde_json::to_string(&atx)?,
            "verify": authorization::verify_transaction(&BatchVerificationContext::new(&mut g.0), &atx).is_ok(),
        });
        writeln!(out, "{line}")?;
    }
    // bodies with several txs (CBMT roots incl. data commitments)
    for i in 0..n / 4 {
        let txs: Vec<Transaction> = (0..g.0.random_range(0..6usize)).map(|_| g.transaction()).collect();
        let coinbase = Coinbase { memo: g.arr::<3>().to_vec(), outputs: (0..g.0.random_range(0..3usize)).map(|_| g.output()).collect::<Vec<_>>().into() };
        // filled txs with random spent outputs so fees are defined
        let filled: Vec<truthcoin_dc::types::FilledTransaction> = txs
            .iter()
            .map(|t| truthcoin_dc::types::FilledTransaction {
                transaction: t.clone(),
                spent_utxos: t.inputs.iter().map(|_| Output { address: Address([0; 20]), content: OutputContent::Value(bitcoin::Amount::from_sat(g.0.random_range(0..u64::MAX / 16))) }).collect(),
                actor_address: None,
            })
            .collect();
        let root = Body::compute_merkle_root(&coinbase, &filled).ok().map(|r| r.to_string());
        let spent: Vec<Vec<String>> = filled.iter().map(|f| f.spent_utxos.iter().map(|o| hex(&bincode::serialize(o).unwrap())).collect()).collect();
        let body = Body { coinbase, transactions: txs, authorizations: Vec::new(), actor_proofs: vec![None; 0] };
        let line = serde_json::json!({
            "body": i,
            "body_bincode": hex(&bincode::serialize(&body)?),
            "spent": spent,
            "merkle_root": root,
            "body_json": serde_json::to_string(&body)?,
        });
        writeln!(out, "{line}")?;
    }
    Ok(())
}

// ------------------------------------------------------------------ replay
fn parse_btc_hash(s: &str) -> anyhow::Result<bitcoin::BlockHash> {
    Ok(bitcoin::BlockHash::from_str(s)?)
}

fn deposit_address(bytes: &[u8]) -> Address {
    // lib/types/proto.rs:465-497
    match std::str::from_utf8(bytes) {
        Ok(s) => match Address::from_deposit_address(s).or_else(|_| Address::from_str(s)) {
            Ok(a) => a,
            Err(_) => Address::ALL_ZEROS,
        },
        Err(_) => Address::ALL_ZEROS,
    }
}

fn twpd_from_json(v: &serde_json::Value) -> anyhow::Result<TwoWayPegData> {
    let mut block_info = LinkedHashMap::new();
    for b in v.as_array().unwrap() {
        let main = parse_btc_hash(b["main"].as_str().unwrap())?;
        let bmm_commitment = match b["bmm"].as_str() {
            Some(s) => Some(BlockHash(const_hex::decode_to_array(s)?)),
            None => None,
        };
        let mut events = Vec::new();
        for e in b["events"].as_array().unwrap() {
            if let Some(d) = e.get("deposit") {
                let txid = bitcoin::Txid::from_str(d["txid"].as_str().unwrap())?;
                let address = deposit_address(&const_hex::decode(d["address_hex"].as_str().unwrap())?);
                events.push(BlockEvent::Deposit(Deposit {
                    tx_index: d["tx_index"].as_u64().unwrap(),
                    outpoint: bitcoin::OutPoint { txid, vout: d["vout"].as_u64().unwrap() as u32 },
                    output: Output { address, content: OutputContent::Value(bitcoin::Amount::from_sat(d["value"].as_u64().unwrap())) },
                }));
            } else {
                let w = &e["bundle"];
                let status = match w["status"].as_str().unwrap() {
                    "submitted" => WithdrawalBundleEventStatus::Submitted,
                    "confirmed" => WithdrawalBundleEventStatus::Confirmed,
                    _ => WithdrawalBundleEventStatus::Failed,
                };
                events.push(BlockEvent::WithdrawalBundle(WithdrawalBundleEvent {
                    m6id: truthcoin_dc::types::M6id(bitcoin::Txid::from_str(w["m6id"].as_str().unwrap())?),
                    status,
                }));
            }
        }
        block_info.replace(main, BlockInfo { bmm_commitment, events });
    }
    Ok(TwoWayPegData { block_info })
}

fn table_hash<I: IntoIterator<Item = (Vec<u8>, Vec<u8>)>>(it: I) -> (usize, String) {
    let mut h = blake3::Hasher::new();
    let mut n = 0;
    for (k, v) in it {
        h.update(&(k.len() as u64).to_le_bytes());
        h.update(&k);
        h.update(&(v.len() as u64).to_le_bytes());
        h.update(&v);
        n += 1;
    }
    (n, h.finalize().to_hex().to_string())
}

fn digest(env: &sneed::Env, state: &State) -> anyhow::Result<serde_json::Value> {
    let rotxn = env.read_txn()?;
    let tip = state.try_get_tip(&rotxn)?.map(|t| t.to_string());
    let height = state.try_get_height(&rotxn)?;
    let roots: Vec<String> = state.get_accumulator(&rotxn)?.get_roots().iter().map(|r| r.to_string()).collect();
    let mut utxos: Vec<(Vec<u8>, Vec<u8>)> = state
        .get_utxos(&rotxn)?
        .into_iter()
        .map(|(op, o)| (truthcoin_dc::types::OutPointKey::from(&op).as_ref().to_vec(), bincode::serialize(&o).unwrap()))
        .collect();
    utxos.sort();
    let (n_utxos, utxo_hash) = table_hash(utxos);
    let reps = state.reputation().get_all_reputations(&rotxn)?;
    let rep: Vec<(String, String)> = reps.iter().map(|(a, r)| (a.to_string(), format!("{:016x}", r.to_bits()))).collect();
    // decisions: claimed entries per period (BTreeSet order), histories
    let decs = state.decisions();
    let mut periods = BTreeMap::new();
    for e in decs.get_all_claimed_decisions(&rotxn)? {
        periods.entry(e.decision_id.period_index()).or_insert_with(Vec::new).push(e);
    }
    let decisions = table_hash(periods.iter().map(|(p, v)| (p.to_le_bytes().to_vec(), bincode::serialize(v).unwrap())));
    let mut hist = Vec::new();
    for (_, v) in &periods {
        for e in v {
            if let Some(h) = decs.get_decision_state_history(&rotxn, e.decision_id)? {
                hist.push((e.decision_id.as_bytes().to_vec(), bincode::serialize(&h).unwrap()));
            }
        }
    }
    hist.sort();
    let hist = table_hash(hist);
    let mut pricing = Vec::new();
    for p in 0..200u32 {
        if let Some(pp) = decs.get_period_pricing(&rotxn, p)? {
            pricing.push((p.to_le_bytes().to_vec(), bincode::serialize(&pp).unwrap()));
        }
    }
    let pricing = table_hash(pricing);
    let markets_db = state.markets();
    let mut markets: Vec<(Vec<u8>, Vec<u8>)> =
        markets_db.get_all_markets(&rotxn)?.into_iter().map(|m| (m.id.0.to_vec(), bincode::serialize(&m).unwrap())).collect();
    markets.sort();
    let mut funds = Vec::new();
    for (id, _) in &markets {
        let mid = MarketId(id.as_slice().try_into().unwrap());
        for fee in [false, true] {
            if let Some(op) = markets_db.get_market_funds_utxo(&rotxn, &mid, fee)? {
                let mut k = id.clone();
                k.push(fee as u8);
                funds.push((k, bincode::serialize(&op).unwrap()));
            }
        }
    }
    let markets = table_hash(markets);
    let funds = table_hash(funds);
    let accounts = table_hash(markets_db.get_all_share_accounts(&rotxn)?.into_iter().map(|(a, pos)| (a.0.to_vec(), bincode::serialize(&pos).unwrap())));
    let voting = state.voting().databases();
    let mut votes = Vec::new();
    let mut outcomes = Vec::new();
    let mut stats = Vec::new();
    for p in 0..200u32 {
        let pid = truthcoin_dc::state::voting::types::VotingPeriodId::new(p);
        for (k, e) in voting.get_votes_for_period(&rotxn, pid)? {
            votes.push((bincode::serialize(&k).unwrap(), bincode::serialize(&e).unwrap()));
        }
        if let Some(s) = voting.get_period_stats(&rotxn, pid)? {
            stats.push((p.to_le_bytes().to_vec(), bincode::serialize(&s).unwrap()));
        }
    }
    for (_, v) in &periods {
        for e in v {
            if let Some(o) = voting.get_decision_outcome(&rotxn, e.decision_id)? {
                outcomes.push((e.decision_id.as_bytes().to_vec(), bincode::serialize(&o).unwrap()));
            }
        }
    }
    votes.sort();
    outcomes.sort();
    let votes = table_hash(votes);
    let outcomes = table_hash(outcomes);
    let stats = table_hash(stats);
    Ok(serde_json::json!({
        "height": height, "tip": tip, "roots": roots,
        "utxos": [n_utxos, utxo_hash], "rep": rep,
        "decisions": decisions, "histories": hist, "pricing": pricing,
        "markets": markets, "funds": funds, "accounts": accounts,
        "votes": votes, "outcomes": outcomes, "stats": stats,
        "main_ts": state.try_get_mainchain_timestamp(&rotxn)?, "genesis_ts": state.try_get_genesis_timestamp(&rotxn)?,
    }))
}

fn replay(path: &str, testing: Option<u32>) -> anyhow::Result<()> {
    let dir = tempfile_dir()?;
    let mut opts = heed::EnvOpenOptions::new();
    opts.map_size(16 * 1024 * 1024 * 1024).max_dbs(State::NUM_DBS + Archive::NUM_DBS);
    let env = unsafe { sneed::Env::open(&opts, &dir) }?;
    let state = State::new(&env, testing)?;
    let archive = Archive::new(&env)?;
    let ctxt = BatchVerificationContext::new(&mut rand::rng());
    let out = std::io::stdout();
    let mut out = out.lock();
    let f = std::io::BufReader::new(std::fs::File::open(path)?);
    let mut last_main: Option<bitcoin::BlockHash> = None;
    for line in f.lines() {
        let v: serde_json::Value = serde_json::from_str(&line?)?;
        let block: Block = bincode::deserialize(&const_hex::decode(v["block"].as_str().unwrap())?)?;
        let main_ts = v["main_ts"].as_u64().unwrap();
        let twpd = twpd_from_json(&v["twpd"])?;
        let mut rwtxn = env.write_txn()?;
        // The archive needs the prev_main header to record the block (put_header looks up its
        // successors for BMM). The state machine reads no mainchain data from the archive, so
        // a chain linking each block's prev_main to the previous one suffices.
        let pm = block.header.prev_main_hash;
        if archive.try_get_main_header_info(&rwtxn, &pm)?.is_none() {
            let info = BlockHeaderInfo {
                block_hash: pm,
                prev_block_hash: last_main.unwrap_or_else(bitcoin::BlockHash::all_zeros),
                height: 0,
                work: bitcoin::Target::MAX.to_work(),
                timestamp: 0,
            };
            archive.put_main_header_info(&mut rwtxn, &info)?;
        }
        last_main = Some(pm);
        archive.put_header(&mut rwtxn, &block.header)?;
        let res = (|| -> anyhow::Result<()> {
            let pre = state.prevalidate_block(&rwtxn, &ctxt, &block.header, &block.body, &archive)?;
            state.connect_prevalidated_block(&mut rwtxn, &block.header, &block.body, pre, main_ts)?;
            state.connect_two_way_peg_data(&mut rwtxn, &twpd)?;
            Ok(())
        })();
        match res {
            Ok(()) => {
                archive.put_body(&mut rwtxn, block.header.hash(), &block.body)?;
                rwtxn.commit()?;
                writeln!(out, "{}", digest(&env, &state)?)?;
            }
            Err(e) => {
                writeln!(out, "{}", serde_json::json!({"height": v["height"], "error": format!("{e:#}")}))?;
                break;
            }
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
    Ok(())
}

fn tempfile_dir() -> anyhow::Result<std::path::PathBuf> {
    let base = std::env::var("HM_ORACLE_TMP").unwrap_or_else(|_| std::env::temp_dir().to_string_lossy().to_string());
    let p = std::path::Path::new(&base).join(format!("hm-oracle-{}-{}", std::process::id(), rand::rng().random::<u64>()));
    std::fs::create_dir_all(&p)?;
    Ok(p)
}

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("vectors") => vectors(args[2].parse()?, args[3].parse()?),
        Some("simulate") => {
            let testing = args.iter().position(|a| a == "--testing").map(|i| args[i + 1].parse()).transpose()?.unwrap_or(4);
            sim::simulate(args[2].parse()?, args[3].parse()?, testing)
        }
        Some("replay") => {
            let testing = args.iter().position(|a| a == "--testing").map(|i| args[i + 1].parse()).transpose()?;
            replay(&args[2], testing)
        }
        _ => {
            eprintln!("usage: hm_oracle vectors <seed> <n> | replay <fixture.jsonl> [--testing N]");
            std::process::exit(2)
        }
    }
}
