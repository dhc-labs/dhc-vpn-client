# Security Policy

## Reporting a Vulnerability

If you believe you have found a security issue in `dhc-vpn-client`,
**please do not open a public GitHub issue**. Instead, email
**security@drhc.de** with:

- A description of the issue and its impact.
- Steps to reproduce, ideally with a minimal config or capture.
- The affected commit / release tag.
- Whether you would like to be credited in the fix announcement.

We aim to acknowledge new reports within five working days and to
publish a fix or coordinated disclosure timeline within thirty days
where feasible. Critical issues affecting the IKE/ESP path or the
WinDivert capture filter take priority.

## Scope

In scope:

- Code in `src/` (plugin, GUI, spike tools, helpers).
- The build, installer, and signing scripts under `scripts/` and
  `.github/workflows/`.
- Configuration defaults shipped via `install.ps1`.

Out of scope (please report to the upstream project instead):

- Vulnerabilities in [strongSwan](https://www.strongswan.org/) — reach
  out via their [security policy](https://docs.strongswan.org/docs/latest/security/security.html).
- Vulnerabilities in [Wintun](https://www.wintun.net/) — report to the
  WireGuard project.
- Vulnerabilities in [WinDivert](https://reqrypt.org/windivert.html) —
  report to the WinDivert maintainers.

If unsure whether something is in or out of scope, send the report
anyway and we will route it.

## Supported Versions

This project is pre-1.0. Only the current `main` branch is supported;
fixes are not back-ported to historical tags. Once stable releases
exist this section will be updated with a support window.

## Cryptographic Notes

`dhc-vpn-client` performs IKE/ESP via strongSwan's `libipsec` running
in user space. Cipher and integrity-algorithm choices are configured
in `swanctl.conf`. When reporting a crypto-relevant issue, please
include the negotiated proposals from `swanctl --list-sas` so we can
distinguish between policy and implementation defects.
