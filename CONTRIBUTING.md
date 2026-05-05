# Contributing

Thanks for your interest in `dhc-vpn-client`. The project is small and
the patch flow is intentionally lightweight.

## Before you open a PR

1. **Build and run** the change locally — see [`README.md`](README.md)
   for the MSYS2-MINGW64 setup. Anything beyond a doc/typo fix should
   pass at least `cmake --build build` and a smoke test against a real
   IKEv2 peer.
2. **Keep the scope tight.** One concern per PR. Mixed refactors and
   feature changes are hard to review and to revert.
3. **Match the existing style.** C code follows the strongSwan house
   style (4-space indents, opening brace on its own line for functions,
   on the same line for blocks). C++/Qt code follows Qt's style (camel-
   case methods, member prefix `m_`). PowerShell scripts use the patterns
   already in `scripts/`.

## Commits

- Subject line under ~70 characters, imperative mood.
- Reference the milestone if relevant (`M9c: ...`) — see
  [`ROADMAP.md`](ROADMAP.md) for the milestone list.
- Body explains *why*, not *what*; the diff already shows the *what*.
- Sign off your commits (`git commit -s`) — we use the
  [Developer Certificate of Origin](https://developercertificate.org/)
  to keep provenance clear.

## What to expect

- Bug reports and small fixes: usually merged within a few days if they
  pass review and CI.
- Feature work: please open an issue to discuss scope first.
- Anything touching the cipher path, the WinDivert filter, or the
  kernel-iph patch: expect a careful review and possibly a request for
  a packet capture or strongSwan log alongside the patch.

## Out of scope

The following are intentionally out of scope (see [`ROADMAP.md`](ROADMAP.md)):

- IKEv1 support.
- macOS / Linux GUI builds.
- Two-factor / TOTP integration in the GUI.

If you want to push the project in a direction that is out of scope,
please open a discussion issue first rather than a code PR.

## Security issues

Do **not** open a public issue for vulnerabilities. See
[`SECURITY.md`](SECURITY.md) for the disclosure process.
