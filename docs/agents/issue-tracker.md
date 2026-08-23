# Issue Tracker

No remote issue tracker is configured. Use repository-local Markdown until this
document explicitly adopts an external tracker.

- One effort lives under `.scratch/<effort>/`.
- Its specification is `.scratch/<effort>/spec.md` when present.
- Tickets live at `.scratch/<effort>/issues/<NN>-<slug>.md`, numbered from `01`.
- Ticket metadata uses `Status: open|claimed|resolved`, optional
  `Assignee: <owner>`, optional `Blocked by: NN, NN`, optional
  `Type: research|prototype|grilling|task`, and optional `Labels:`.
- Conversation updates append under `## Comments`; completed work is recorded
  under `## Answer` before setting `Status: resolved`.
- Concurrent claims use an atomically created
  `.scratch/<effort>/.claims/<NN>/` directory. A session proceeds only after its
  ticket metadata and claim owner agree on reread.

If the project adopts GitHub, GitLab, or another tracker, update this file first
and migrate active local tickets deliberately.
