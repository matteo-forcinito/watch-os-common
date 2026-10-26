# Development activity reporting

The user uses Agent Deck to follow Codex / Claude Code work, builds, tests and releases.

- Read `tools/agent_deck/README.md` when using the monitor. Its Python CLI has no third-party dependencies.
- Run substantial builds, tests and release commands through `tools/agent_deck/monitor.py run` when practical. Use an accurate title, agent, kind and repository working directory. The wrapper propagates the actual exit code and writes a private local log; do not claim completion before it exits successfully.
- Record decisions requiring user input and handover information with `monitor.py event`, using a stable activity ID. Update that ID after the question is resolved. Include the relevant branch, next step and a resume command where available. An event is a declared status, distinct from a verified process exit.
- Do not publish credentials, raw transcripts or local activity databases. Keep device pairing files outside the repository. Never infer successful builds from process disappearance, or session allowances from token totals.
- The dashboard cannot approve agent permissions or run arbitrary shell commands. Required input remains in the original agent session. Tracking does not authorize deployment, publication, interruption or any other action beyond the user's task.
- Agent Deck's device snapshot and networking are on demand. Closing the native app must free the snapshot and stop its owned web service; do not add boot-time workers for it.
