# sw/webui/

S6 deliverable: 12 thin-client CGI binaries that marshal JSON between
BusyBox httpd and the daemon Unix socket `/run/tetra_d.sock`.

- `cgi_common.{c,h}` — shared library: env-parse, length-prefixed JSON
  envelope, allow-list gate, error-mapping. See header for the locked
  `IF_WEBUI_CGI_v1` contract.
- `src/<name>_main.c` — per-binary entry point, < 30 LOC, sets the
  compile-time op allow-list and calls `cgi_run()`.
- `index.html` — operator dashboard (Live + Profiles + Entities tabs).

Build (host x86, sw-test): `make`
Build (ARM hard-float, deploy): `make ARM=1`
Tests: `make -C ../../tb/sw/webui all`

Wire format: `docs/OPERATIONS.md §6`. Endpoint catalog: §1..§5.
