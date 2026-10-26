Public eCash betanet, Truthcoin (slot 13), fetched 2026-10-22:

- `blocks.bin`: blocks 0..650 from seed.beta.ecash.eu.com:4013 (`truthcoin-fetch-blocks`),
  records `u32 height, u32 len, bincode(ResponseMessage::Block)` as received.
- `twpd13.json`: `GetTwoWayPegData {sidechainId: 13, endBlockHash: 0000…5998ae (main 973,919)}`
  from https://enforcer.beta.ecash.ninja (Connect JSON).
- `fixture.jsonl`: `truthcoin-replay fixture blocks.bin twpd13.json` (block, BMM block timestamp,
  2WPD window per height).
- `oracle_digests.jsonl`: `hm_oracle replay fixture.jsonl`, i.e. THEIR State (truthcoin-dc
  v0.20.0) after each block: tip, accumulator roots and hashes of every table.
