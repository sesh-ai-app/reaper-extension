# Security Policy

## Supported Versions

Sesh AI is pre-release. Security fixes land on `main` and ship in the next
tagged release. Only the most recent release is supported — if you are running
an older binary, update before reporting.

## Reporting a Vulnerability

Report privately through GitHub's private vulnerability reporting: open the
[Security tab](https://github.com/sesh-ai-app/reaper-extension/security/advisories/new)
on this repository and submit a draft advisory. That channel is visible only to
the maintainers until a fix is published.

Please do not open a public issue for a suspected vulnerability, and please do
not disclose it elsewhere before a fix is available.

Helpful things to include:

- What the issue is and what an attacker could do with it
- Steps to reproduce, or a proof of concept
- The extension version, your operating system, and your REAPER version
- Any relevant log output, with credentials removed

## What to Expect

- Acknowledgement within 5 business days
- An assessment and a rough remediation timeline within 10 business days
- Updates on the advisory thread as the fix progresses
- Credit in the advisory and release notes, unless you prefer otherwise

## Scope

This repository is the REAPER extension — the native plugin that REAPER loads,
its embedded UI, and its build configuration. In scope, for example:

- Code execution, privilege escalation, or file access beyond what the extension
  needs on the producer's machine
- Leaking Cognito tokens, stream credentials, or other credential material
  through logs, the UI, disk, or the network
- Missing or incorrect TLS verification on outbound connections
- Accepting a malformed or hostile server message in a way that damages the
  producer's project or bypasses a confirmation

Out of scope here:

- The Sesh AI server, agent, and AWS infrastructure. Those are not in this
  repository; report them through the same channel and note that they are
  server-side
- REAPER itself, CEF, and other upstream dependencies — report those upstream.
  If an upstream issue is made exploitable by how this extension uses it, that
  is in scope
- The protocol and tool schemas being readable. This repository is public by
  design and nothing in it depends on a secret staying hidden

## Security Design Notes

Context that may save you time:

- The extension holds no AWS credentials. Cognito tokens and the stream publish
  token are the only credential material it sees, and tokens are held in memory
  rather than written to disk
- The extension does not execute code it receives. Generated ReaScript is
  delivered as a file the producer reads and runs themselves
- Every outbound connection uses TLS
- Tool authorization happens on the server. The extension refuses on safety
  preconditions, which is a separate check, not a substitute for that one
