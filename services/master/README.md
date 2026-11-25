# bwmaster

A master server for Bulwark games: servers heartbeat it, the server
browser asks it for the list. It speaks the DarkPlaces master protocol
(what Bulwark, FTEQW and DarkPlaces use), so it is one Python file with no
dependencies. The protocol and the browser are in
[docs/spec/serverbrowser.md](../../docs/spec/serverbrowser.md).

    python3 bwmaster.py --port 27950 [--bind6 ::] [--http 127.0.0.1:27951] [--verbose]

- Servers: `sv_public 1` and `sv_master1 <host>:27950`.
- Players: `net_master1 <host>:27950` (Stiletto lists `com_protocolname`
  `StilettoSwift`, set by its `base.fmf`).
- `--http` serves the list as JSON; keep it on localhost.
- A server is listed only after it answers `getinfo` with the master's
  challenge, and is dropped after `--timeout` seconds (900) without a
  heartbeat.

`bwlive-server` also defaults to UDP 27950: on one machine give one of them
another port.

Test: `python3 tests/serverbrowser/master_test.py`.
