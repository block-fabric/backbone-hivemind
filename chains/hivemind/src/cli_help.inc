// Generated from truthcoin-cli 0.20.0 (official release binary) --help / -h output. Do not edit.
static const char* HELP_MAIN = R"HELP(Usage: truthcoin-cli [OPTIONS] <COMMAND>

Commands:
  balance
          Get balance in sats
  connect-block
          Connect a block for which a BMM request was included in the specified mainchain block. The block is the JSON returned by `get-block-template`
  connect-peer
          Connect to a peer
  create-deposit
          Deposit to address
  create-transfer
          Create a tx that transfers funds to the specified address
  create-transfer-many
          Create a tx that transfers funds to each address in a JSON map of address to value in sats, such as `{"<address>": 1000}`
  create-withdrawal
          Creates a tx that initiates a withdrawal to the specified mainchain address
  format-deposit-address
          Format a deposit address
  forget-peer
          Delete peer from known_peers DB. Connections to the peer are not terminated
  generate-mnemonic
          Generate a mnemonic seed phrase
  get-best-mainchain-block-hash
          Get the best mainchain block hash
  get-best-sidechain-block-hash
          Get the best sidechain block hash
  get-block
          Get the block with specified block hash, if it exists
  get-blockcount
          Get the current block count
  get-block-hash
          Get the block hash at the specified height in the active chain, if it exists
  get-block-index
          Get everything about a block that its body does not carry
  get-block-template
          Assemble a block to blind merge mine, without requesting BMM for it
  get-bmm-inclusions
          Get mainchain blocks that commit to a specified block hash
  get-new-address
          Get a new address
  get-stxos
          Get stxos for addresses
  get-transaction
          Get transaction by txid
  get-two-way-peg-events
          Get the coin movements that a block applied outside its body
  get-utxos
          Get utxos for addresses
  get-wallet-addresses
          Get wallet addresses, sorted by base58 encoding
  get-wallet-utxos
          Get wallet UTXOs
  get-unconfirmed-wallet-utxos
          Get the unconfirmed wallet UTXOs that the wallet may spend
  get-withdrawal-bundle
          Get withdrawal bundle by M6id
  invalidate-block
          Invalidate a block, potentially re-orging to a valid ancestor of the current tip
  latest-failed-withdrawal-bundle-height
          Get the height of the latest failed withdrawal bundle
  list-mempool
          List the transactions the mempool holds
  list-peers
          List peers
  list-utxos
          List all UTXOs
  mainchain-sync-progress
          Get the progress of the startup sync with the mainchain
  mine
          Attempt to mine a sidechain block
  pending-withdrawal-bundle
          Get pending withdrawal bundle
  openapi-schema
          Show OpenAPI schema
  remove-from-mempool
          Remove a tx from the mempool
  set-seed-from-mnemonic
          Set the wallet seed from a mnemonic seed phrase
  sidechain-wealth
          Get total sidechain wealth
  sign-transaction
          Sign a transaction, and optionally broadcast it
  submit-transaction
          Verify and broadcast a transaction
  stop
          Stop the node
  get-voter-address
          Get the voter address (used for reputation and voting)
  my-unconfirmed-utxos
          List unconfirmed owned UTXOs
  get-transaction-info
          Get transaction info
  get-new-encryption-key
          Get new encryption key
  get-new-verifying-key
          Get new verifying key
  encrypt-msg
          Encrypt message
  decrypt-msg
          Decrypt message
  sign-arbitrary-msg
          Sign arbitrary message with verifying key
  sign-arbitrary-msg-as-addr
          Sign arbitrary message as address
  verify-signature
          Verify signature
  decision-status
          Get decision system status
  decision-list
          List decisions with optional filtering
  decision-get
          Get decision by ID
  decision-fee
          Get listing fee info for a period
  decision-fee-for-id
          Get the listing fee for a specific decision_id
  decision-claim
          Claim a decision. The application picks the cheapest available unlocked standard slot in `period_index` automatically
  market-create
          Create prediction market with optional new-decision claims. Pass `--dimensions-json` containing a JSON array of DimensionInput values (`{"type":"existing","id":"..."}` or `{"type":"new","period_index":N,"decision_type":"binary","header":"..."}`). Decisions tagged `type:new` are claimed in the same block
  list-open-periods
          List open periods and the cheapest available slot price per period
  market-list
          List all markets
  market-get
          Get market details
  market-price-history
          Get the price history of a market
  market-buy
          Buy shares in a market
  market-sell
          Sell shares in a market
  market-amplify-beta
          Amplify a market's LMSR beta by funding its treasury (market author only)
  market-positions
          Get share positions for an address
  calculate-share-cost
          Calculate share cost (dry run)
  calculate-initial-liquidity
          Calculate initial liquidity required
  vote-voter
          Get voter information
  vote-voters
          List all registered voters
  vote-submit
          Submit vote(s) - use comma-separated "id:value" for batch
  vote-list
          Query votes with filters
  vote-period
          Get voting period info (omit period_id for current)
  votecoin-transfer
          Transfer votecoin
  votecoin-balance
          Get votecoin balance
  help
          Print this message or the help of the given subcommand(s)

Options:
      --rpc-url <RPC_URL>  Base URL used for requests to the RPC server [default: http://localhost:6013]
      --timeout <TIMEOUT>  Timeout for RPC requests in seconds (default: 60)
  -v, --verbose            Enable verbose HTTP output
  -h, --help               Print help
  -V, --version            Print version
)HELP";
struct CmdHelp { const char* name; const char* long_help; const char* short_help; };
static const CmdHelp CMD_HELP[] = {
    {"balance", R"HELP(Get balance in sats

Usage: truthcoin-cli balance

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"connect-block", R"HELP(Connect a block for which a BMM request was included in the specified mainchain block. The block is the JSON returned by `get-block-template`

Usage: truthcoin-cli connect-block <BLOCK> <MAIN_BLOCK_HASH>

Arguments:
  <BLOCK>            
  <MAIN_BLOCK_HASH>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"connect-peer", R"HELP(Connect to a peer

Usage: truthcoin-cli connect-peer <ADDR>

Arguments:
  <ADDR>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"create-deposit", R"HELP(Deposit to address

Usage: truthcoin-cli create-deposit --value-sats <VALUE_SATS> --fee-sats <FEE_SATS> <ADDRESS>

Arguments:
  <ADDRESS>  

Options:
      --value-sats <VALUE_SATS>  
      --fee-sats <FEE_SATS>      
  -h, --help                     Print help
)HELP", nullptr},
    {"create-transfer", R"HELP(Create a tx that transfers funds to the specified address

Usage: truthcoin-cli create-transfer --value-sats <VALUE_SATS> --fee-sats <FEE_SATS> <DEST>

Arguments:
  <DEST>  

Options:
      --value-sats <VALUE_SATS>  
      --fee-sats <FEE_SATS>      
  -h, --help                     Print help
)HELP", nullptr},
    {"create-transfer-many", R"HELP(Create a tx that transfers funds to each address in a JSON map of address to value in sats, such as `{"<address>": 1000}`

Usage: truthcoin-cli create-transfer-many --fee-sats <FEE_SATS> <DESTS>

Arguments:
  <DESTS>  

Options:
      --fee-sats <FEE_SATS>  
  -h, --help                 Print help
)HELP", nullptr},
    {"create-withdrawal", R"HELP(Creates a tx that initiates a withdrawal to the specified mainchain address

Usage: truthcoin-cli create-withdrawal --amount-sats <AMOUNT_SATS> --fee-sats <FEE_SATS> --mainchain-fee-sats <MAINCHAIN_FEE_SATS> <MAINCHAIN_ADDRESS>

Arguments:
  <MAINCHAIN_ADDRESS>  

Options:
      --amount-sats <AMOUNT_SATS>                
      --fee-sats <FEE_SATS>                      
      --mainchain-fee-sats <MAINCHAIN_FEE_SATS>  
  -h, --help                                     Print help
)HELP", nullptr},
    {"format-deposit-address", R"HELP(Format a deposit address

Usage: truthcoin-cli format-deposit-address <ADDRESS>

Arguments:
  <ADDRESS>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"forget-peer", R"HELP(Delete peer from known_peers DB. Connections to the peer are not terminated

Usage: truthcoin-cli forget-peer <ADDR>

Arguments:
  <ADDR>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"generate-mnemonic", R"HELP(Generate a mnemonic seed phrase

Usage: truthcoin-cli generate-mnemonic

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-best-mainchain-block-hash", R"HELP(Get the best mainchain block hash

Usage: truthcoin-cli get-best-mainchain-block-hash

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-best-sidechain-block-hash", R"HELP(Get the best sidechain block hash

Usage: truthcoin-cli get-best-sidechain-block-hash

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-block", R"HELP(Get the block with specified block hash, if it exists

Usage: truthcoin-cli get-block <BLOCK_HASH> [VERBOSE]

Arguments:
  <BLOCK_HASH>  
  [VERBOSE]     [possible values: true, false]

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-blockcount", R"HELP(Get the current block count

Usage: truthcoin-cli get-blockcount

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-block-hash", R"HELP(Get the block hash at the specified height in the active chain, if it exists

Usage: truthcoin-cli get-block-hash <HEIGHT>

Arguments:
  <HEIGHT>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-block-index", R"HELP(Get everything about a block that its body does not carry

Usage: truthcoin-cli get-block-index <BLOCK_HASH>

Arguments:
  <BLOCK_HASH>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-block-template", R"HELP(Assemble a block to blind merge mine, without requesting BMM for it

Usage: truthcoin-cli get-block-template

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-bmm-inclusions", R"HELP(Get mainchain blocks that commit to a specified block hash

Usage: truthcoin-cli get-bmm-inclusions <BLOCK_HASH>

Arguments:
  <BLOCK_HASH>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-new-address", R"HELP(Get a new address

Usage: truthcoin-cli get-new-address

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-stxos", R"HELP(Get stxos for addresses

Usage: truthcoin-cli get-stxos <ADDRESSES>...

Arguments:
  <ADDRESSES>...  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-transaction", R"HELP(Get transaction by txid

Usage: truthcoin-cli get-transaction <TXID>

Arguments:
  <TXID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-two-way-peg-events", R"HELP(Get the coin movements that a block applied outside its body

Usage: truthcoin-cli get-two-way-peg-events <BLOCK_HASH>

Arguments:
  <BLOCK_HASH>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-utxos", R"HELP(Get utxos for addresses

Usage: truthcoin-cli get-utxos <ADDRESSES>...

Arguments:
  <ADDRESSES>...  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-wallet-addresses", R"HELP(Get wallet addresses, sorted by base58 encoding

Usage: truthcoin-cli get-wallet-addresses

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-wallet-utxos", R"HELP(Get wallet UTXOs

Usage: truthcoin-cli get-wallet-utxos

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-unconfirmed-wallet-utxos", R"HELP(Get the unconfirmed wallet UTXOs that the wallet may spend

Usage: truthcoin-cli get-unconfirmed-wallet-utxos

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-withdrawal-bundle", R"HELP(Get withdrawal bundle by M6id

Usage: truthcoin-cli get-withdrawal-bundle <M6ID>

Arguments:
  <M6ID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"invalidate-block", R"HELP(Invalidate a block, potentially re-orging to a valid ancestor of the current tip

Usage: truthcoin-cli invalidate-block <BLOCK_HASH>

Arguments:
  <BLOCK_HASH>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"latest-failed-withdrawal-bundle-height", R"HELP(Get the height of the latest failed withdrawal bundle

Usage: truthcoin-cli latest-failed-withdrawal-bundle-height

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"list-mempool", R"HELP(List the transactions the mempool holds

Usage: truthcoin-cli list-mempool

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"list-peers", R"HELP(List peers

Usage: truthcoin-cli list-peers

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"list-utxos", R"HELP(List all UTXOs

Usage: truthcoin-cli list-utxos

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"mainchain-sync-progress", R"HELP(Get the progress of the startup sync with the mainchain

Usage: truthcoin-cli mainchain-sync-progress

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"mine", R"HELP(Attempt to mine a sidechain block

Usage: truthcoin-cli mine [OPTIONS]

Options:
      --fee-sats <FEE_SATS>  
  -h, --help                 Print help
)HELP", nullptr},
    {"pending-withdrawal-bundle", R"HELP(Get pending withdrawal bundle

Usage: truthcoin-cli pending-withdrawal-bundle

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"openapi-schema", R"HELP(Show OpenAPI schema

Usage: truthcoin-cli openapi-schema

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"remove-from-mempool", R"HELP(Remove a tx from the mempool

Usage: truthcoin-cli remove-from-mempool <TXID>

Arguments:
  <TXID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"set-seed-from-mnemonic", R"HELP(Set the wallet seed from a mnemonic seed phrase

Usage: truthcoin-cli set-seed-from-mnemonic <MNEMONIC>

Arguments:
  <MNEMONIC>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"sidechain-wealth", R"HELP(Get total sidechain wealth

Usage: truthcoin-cli sidechain-wealth

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"sign-transaction", R"HELP(Sign a transaction, and optionally broadcast it

Usage: truthcoin-cli sign-transaction <TRANSACTION> [BROADCAST]

Arguments:
  <TRANSACTION>  
  [BROADCAST]    

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"submit-transaction", R"HELP(Verify and broadcast a transaction

Usage: truthcoin-cli submit-transaction <TRANSACTION>

Arguments:
  <TRANSACTION>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"stop", R"HELP(Stop the node

Usage: truthcoin-cli stop

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-voter-address", R"HELP(Get the voter address (used for reputation and voting)

Usage: truthcoin-cli get-voter-address

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"my-unconfirmed-utxos", R"HELP(List unconfirmed owned UTXOs

Usage: truthcoin-cli my-unconfirmed-utxos

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-transaction-info", R"HELP(Get transaction info

Usage: truthcoin-cli get-transaction-info <TXID>

Arguments:
  <TXID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-new-encryption-key", R"HELP(Get new encryption key

Usage: truthcoin-cli get-new-encryption-key

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"get-new-verifying-key", R"HELP(Get new verifying key

Usage: truthcoin-cli get-new-verifying-key

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"encrypt-msg", R"HELP(Encrypt message

Usage: truthcoin-cli encrypt-msg --encryption-pubkey <ENCRYPTION_PUBKEY> --msg <MSG>

Options:
      --encryption-pubkey <ENCRYPTION_PUBKEY>  
      --msg <MSG>                              
  -h, --help                                   Print help
)HELP", nullptr},
    {"decrypt-msg", R"HELP(Decrypt message

Usage: truthcoin-cli decrypt-msg [OPTIONS] --encryption-pubkey <ENCRYPTION_PUBKEY> --msg <MSG>

Options:
      --encryption-pubkey <ENCRYPTION_PUBKEY>  
      --msg <MSG>                              
      --utf8                                   
  -h, --help                                   Print help
)HELP", nullptr},
    {"sign-arbitrary-msg", R"HELP(Sign arbitrary message with verifying key

Usage: truthcoin-cli sign-arbitrary-msg --verifying-key <VERIFYING_KEY> --msg <MSG>

Options:
      --verifying-key <VERIFYING_KEY>  
      --msg <MSG>                      
  -h, --help                           Print help
)HELP", nullptr},
    {"sign-arbitrary-msg-as-addr", R"HELP(Sign arbitrary message as address

Usage: truthcoin-cli sign-arbitrary-msg-as-addr --address <ADDRESS> --msg <MSG>

Options:
      --address <ADDRESS>  
      --msg <MSG>          
  -h, --help               Print help
)HELP", nullptr},
    {"verify-signature", R"HELP(Verify signature

Usage: truthcoin-cli verify-signature --signature <SIGNATURE> --verifying-key <VERIFYING_KEY> --dst <DST> --msg <MSG>

Options:
      --signature <SIGNATURE>
          

      --verifying-key <VERIFYING_KEY>
          

      --dst <DST>
          Possible values:
          - transaction
          - arbitrary:   Arbitrary, non-protocol messages

      --msg <MSG>
          

  -h, --help
          Print help (see a summary with '-h')
)HELP", R"HELP(Verify signature

Usage: truthcoin-cli verify-signature --signature <SIGNATURE> --verifying-key <VERIFYING_KEY> --dst <DST> --msg <MSG>

Options:
      --signature <SIGNATURE>          
      --verifying-key <VERIFYING_KEY>  
      --dst <DST>                      [possible values: transaction, arbitrary]
      --msg <MSG>                      
  -h, --help                           Print help (see more with '--help')
)HELP"},
    {"decision-status", R"HELP(Get decision system status

Usage: truthcoin-cli decision-status

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"decision-list", R"HELP(List decisions with optional filtering

Usage: truthcoin-cli decision-list [OPTIONS]

Options:
      --period <PERIOD>  Filter by period
      --status <STATUS>  Filter by status: available, claimed, voting, settled
  -h, --help             Print help
)HELP", nullptr},
    {"decision-get", R"HELP(Get decision by ID

Usage: truthcoin-cli decision-get <DECISION_ID>

Arguments:
  <DECISION_ID>  Decision ID (hex)

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"decision-fee", R"HELP(Get listing fee info for a period

Usage: truthcoin-cli decision-fee <PERIOD>

Arguments:
  <PERIOD>  Period index

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"decision-fee-for-id", R"HELP(Get the listing fee for a specific decision_id

Usage: truthcoin-cli decision-fee-for-id <DECISION_ID>

Arguments:
  <DECISION_ID>  Decision ID (hex)

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"decision-claim", R"HELP(Claim a decision. The application picks the cheapest available unlocked standard slot in `period_index` automatically

Usage: truthcoin-cli decision-claim [OPTIONS] --period-index <PERIOD_INDEX> --decision-type <DECISION_TYPE> --header <HEADER>

Options:
      --period-index <PERIOD_INDEX>
          
      --decision-type <DECISION_TYPE>
          Decision type: "binary", "scaled", or "category"
      --header <HEADER>
          
      --description <DESCRIPTION>
          
      --min <MIN>
          
      --max <MAX>
          
      --increment <INCREMENT>
          Step size for valid scaled-vote values (default 1.0). Honored only with --decision-type scaled
      --option-0-label <OPTION_0_LABEL>
          
      --option-1-label <OPTION_1_LABEL>
          
      --option-labels <OPTION_LABELS>
          
      --tx-fee-sats <TX_FEE_SATS>
          Miner fee (sats) [default: 1000]
      --max-listing-fee-sats <MAX_LISTING_FEE_SATS>
          Optional cap on the protocol-set listing fee
  -h, --help
          Print help
)HELP", nullptr},
    {"market-create", R"HELP(Create prediction market with optional new-decision claims. Pass `--dimensions-json` containing a JSON array of DimensionInput values (`{"type":"existing","id":"..."}` or `{"type":"new","period_index":N,"decision_type":"binary","header":"..."}`). Decisions tagged `type:new` are claimed in the same block

Usage: truthcoin-cli market-create [OPTIONS] --title <TITLE> --description <DESCRIPTION> --dimensions-json <DIMENSIONS_JSON>

Options:
      --title <TITLE>
          
      --description <DESCRIPTION>
          
      --dimensions-json <DIMENSIONS_JSON>
          JSON array of dimension inputs (see command help)
      --beta <BETA>
          
      --trading-fee <TRADING_FEE>
          [default: 0.005]
      --initial-liquidity <INITIAL_LIQUIDITY>
          
      --tx-pow-hash-selector <TX_POW_HASH_SELECTOR>
          
      --tx-pow-ordering <TX_POW_ORDERING>
          
      --tx-pow-difficulty <TX_POW_DIFFICULTY>
          
      --tx-fee-sats <TX_FEE_SATS>
          [default: 1000]
      --max-listing-fee-sats <MAX_LISTING_FEE_SATS>
          
  -h, --help
          Print help
)HELP", nullptr},
    {"list-open-periods", R"HELP(List open periods and the cheapest available slot price per period

Usage: truthcoin-cli list-open-periods

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"market-list", R"HELP(List all markets

Usage: truthcoin-cli market-list

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"market-get", R"HELP(Get market details

Usage: truthcoin-cli market-get <MARKET_ID>

Arguments:
  <MARKET_ID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"market-price-history", R"HELP(Get the price history of a market

Usage: truthcoin-cli market-price-history <MARKET_ID>

Arguments:
  <MARKET_ID>  

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"market-buy", R"HELP(Buy shares in a market

Usage: truthcoin-cli market-buy --market-id <MARKET_ID> --outcome-index <OUTCOME_INDEX> --shares-amount <SHARES_AMOUNT> --max-cost <MAX_COST>

Options:
      --market-id <MARKET_ID>          
      --outcome-index <OUTCOME_INDEX>  
      --shares-amount <SHARES_AMOUNT>  
      --max-cost <MAX_COST>            
  -h, --help                           Print help
)HELP", nullptr},
    {"market-sell", R"HELP(Sell shares in a market

Usage: truthcoin-cli market-sell [OPTIONS] --market-id <MARKET_ID> --outcome-index <OUTCOME_INDEX> --shares-amount <SHARES_AMOUNT> --seller-address <SELLER_ADDRESS>

Options:
      --market-id <MARKET_ID>
          
      --outcome-index <OUTCOME_INDEX>
          
      --shares-amount <SHARES_AMOUNT>
          
      --seller-address <SELLER_ADDRESS>
          Address holding the shares to sell
      --min-proceeds <MIN_PROCEEDS>
          Minimum proceeds to accept (slippage protection) [default: 0]
  -h, --help
          Print help
)HELP", nullptr},
    {"market-amplify-beta", R"HELP(Amplify a market's LMSR beta by funding its treasury (market author only)

Usage: truthcoin-cli market-amplify-beta --market-id <MARKET_ID> --amount-sats <AMOUNT_SATS>

Options:
      --market-id <MARKET_ID>      
      --amount-sats <AMOUNT_SATS>  
  -h, --help                       Print help
)HELP", nullptr},
    {"market-positions", R"HELP(Get share positions for an address

Usage: truthcoin-cli market-positions [OPTIONS]

Options:
      --address <ADDRESS>      
      --market-id <MARKET_ID>  
  -h, --help                   Print help
)HELP", nullptr},
    {"calculate-share-cost", R"HELP(Calculate share cost (dry run)

Usage: truthcoin-cli calculate-share-cost --market-id <MARKET_ID> --outcome-index <OUTCOME_INDEX> --shares-amount <SHARES_AMOUNT>

Options:
      --market-id <MARKET_ID>          
      --outcome-index <OUTCOME_INDEX>  
      --shares-amount <SHARES_AMOUNT>  
  -h, --help                           Print help
)HELP", nullptr},
    {"calculate-initial-liquidity", R"HELP(Calculate initial liquidity required

Usage: truthcoin-cli calculate-initial-liquidity [OPTIONS] --beta <BETA>

Options:
      --beta <BETA>                  
      --dimensions <DIMENSIONS>      Dimension specification in bracket notation
      --num-outcomes <NUM_OUTCOMES>  Number of outcomes (alternative to dimensions)
  -h, --help                         Print help
)HELP", nullptr},
    {"vote-voter", R"HELP(Get voter information

Usage: truthcoin-cli vote-voter <ADDRESS>

Arguments:
  <ADDRESS>  Voter address

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"vote-voters", R"HELP(List all registered voters

Usage: truthcoin-cli vote-voters

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"vote-submit", R"HELP(Submit vote(s) - use comma-separated "id:value" for batch

Usage: truthcoin-cli vote-submit [OPTIONS]

Options:
      --decision-id <DECISION_ID>  Single vote: --decision-id X --vote-value Y, or batch: --votes "id1:val1,id2:val2"
      --vote-value <VOTE_VALUE>    
      --votes <VOTES>              
      --fee-sats <FEE_SATS>        [default: 1000]
  -h, --help                       Print help
)HELP", nullptr},
    {"vote-list", R"HELP(Query votes with filters

Usage: truthcoin-cli vote-list [OPTIONS]

Options:
      --voter <VOTER>              
      --decision-id <DECISION_ID>  
      --period-id <PERIOD_ID>      
  -h, --help                       Print help
)HELP", nullptr},
    {"vote-period", R"HELP(Get voting period info (omit period_id for current)

Usage: truthcoin-cli vote-period [OPTIONS]

Options:
      --period-id <PERIOD_ID>  
  -h, --help                   Print help
)HELP", nullptr},
    {"votecoin-transfer", R"HELP(Transfer votecoin

Usage: truthcoin-cli votecoin-transfer [OPTIONS] --amount <AMOUNT> <DEST>

Arguments:
  <DEST>  

Options:
      --amount <AMOUNT>      
      --fee-sats <FEE_SATS>  [default: 1000]
  -h, --help                 Print help
)HELP", nullptr},
    {"votecoin-balance", R"HELP(Get votecoin balance

Usage: truthcoin-cli votecoin-balance <ADDRESS>

Arguments:
  <ADDRESS>  Address to check

Options:
  -h, --help  Print help
)HELP", nullptr},
    {"help", R"HELP(error: unrecognized subcommand '--help'

Usage: truthcoin-cli [OPTIONS] <COMMAND>

For more information, try '--help'.
)HELP", R"HELP(error: unrecognized subcommand '-h'

Usage: truthcoin-cli [OPTIONS] <COMMAND>

For more information, try '--help'.
)HELP"},
};
