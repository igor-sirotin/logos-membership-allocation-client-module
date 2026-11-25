# logos-membership-allocation-client-module

Logos module `membership_allocation_client_module`: the client of the RLN membership
allocator. It asks an allocator, over libp2p, to register an RLN membership
on the client's behalf, so the client holds no funds and its RLN identity secret never leaves its
machine. The allocator side is
[`logos-membership-allocation-module`](https://github.com/logos-co/logos-membership-allocation-module).

## Methods

- `request(args_json)`: request a membership for an identity commitment. Allocators
  advertising the (registry, auth type) pair through libp2p service discovery are asked first,
  then the configured `allocatorPeerId`/`allocatorMultiaddr` pair if any. The auth payload is given raw
  (`authPayload`) or produced by an `authProvider` module that implements the `auth_vector`
  producer contract; an auth type with neither needs no payload. This module knows no auth
  vector by name. A failure carries a `kind` to branch on.
- `discover(args_json)`: the allocators currently advertising a (registry, auth type) pair.
- `peer_id()`: the peer id allocators see for this module (`peer-any`, `peer-allowlist`).

## The node

The module asks through a libp2p node of its own, linked into the module (nim-libp2p's C
binding, the revision `libp2p_module` v1.1.1 is built from). It never calls `libp2p_module` and
does not read `LIBP2P_MODULE_CONFIG`, so a host's own libp2p node is left alone whatever the order
of start.

- `request` and `discover` take `bootstrapPeers`, multiaddrs of peers on the DHT the allocators
  advertise on, and bring the node up on them. With no bootstrap peer discovery is unavailable;
  a configured allocator is still dialled.
- It listens on `127.0.0.1` only, on a port the system picks (libp2p does not start without a
  listen address), runs service discovery as a client and publishes no record: other nodes
  neither route through it nor hand it out.
- Its key is created once and kept in `allocation_node.key` (mode 0600) in the module's
  persistence directory, so the peer id is stable. Treat the file as a secret.
- It is stopped 30 s after its last use and brought up again on the next call.

The argument and reply shapes are documented on the trait in `rust-lib/src/lib.rs`.

## Dependencies

- No other module is needed to reach an allocator.
- An auth producer module, when `authProvider` names one. It is opened on first use.
- Shared crates, pinned by commit: `membership_allocation_action`, `paymaster_client`,
  `paymaster_lp` and `paymaster_wire` from `logos-membership-allocation-crates`.

## Build and test

```sh
nix build .#lgx-portable      # the release package
nix build .#node-tests        # the node against a registrar and an advertiser, in one process
nix run .#generate            # stage rust-lib/generated/ and logos-rust-sdk-src/ for cargo
cd rust-lib && cargo test && cargo clippy --all-targets -- -D warnings
```

`node-tests` builds its two peers from `libp2p_module`'s sources, which is why that repo is a
flake input.

The tests enable the `test-fake` feature of `paymaster_lp` as a dev-dependency; the
plugin build links the real `lp_*` bindings (`cargo check --lib` compiles that path).
