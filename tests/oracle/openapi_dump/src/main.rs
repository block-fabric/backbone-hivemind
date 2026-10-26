fn main() {
    let which = std::env::args().nth(1).unwrap_or_default();
    let doc = match which.as_str() {
        "public" => truthcoin_dc_app_rpc_api::public_openapi(),
        "private" => truthcoin_dc_app_rpc_api::private_openapi(),
        _ => truthcoin_dc_app_rpc_api::openapi(),
    }
    .unwrap();
    print!("{}", serde_json::to_string(&doc).unwrap());
}
