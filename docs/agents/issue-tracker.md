# Issue Tracker

GitHub is the canonical source for the roadmap, active issues, dependencies,
ownership, and completion state.

- Repository: <https://github.com/Arenoros/ulog>
- Issues: <https://github.com/Arenoros/ulog/issues>
- GitHub Project: <https://github.com/users/Arenoros/projects/5>

## Issue Bodies

Every implementation issue belongs to a parent roadmap issue. Keep these fields
at the top of the body:

```text
Parent roadmap: #<issue>
Type: research|prototype|grilling|task
Blocked by: #<issue>, #<issue> | none
```

Follow them with the context and scope needed to execute the issue, then use
these sections:

```markdown
## Acceptance criteria

- [ ] Observable completion criterion

## Answer

<!-- Fill in the outcome, evidence, and deliberate deviations before closing. -->
```

Use GitHub issue references for every parent and blocker. Split work whose
acceptance criteria cannot be completed and verified by one agent without a
handoff. Before closing an issue, complete `## Answer`, check every satisfied
criterion, and link the relevant commits, pull requests, tests, benchmarks, or
ADRs.

## Claiming and Coordination

Agents share one GitHub identity, so assignees and comment authors do not
identify the active agent. Coordinate with a unique token such as a Codex task
path or session identifier:

```text
Claim: <token> | scope: <issue or explicit sub-scope> | started: <UTC timestamp>
Release: <token> | result: completed|blocked|handoff | next: <summary>
```

Post a Claim comment, then reread all issue comments before starting. The
earliest active Claim for overlapping scope wins; later claimants coordinate in
comments or select another issue. A Claim remains active until its matching
Release comment. Parallel claims on one issue require explicit, non-overlapping
sub-scopes. If an apparent stale Claim must be taken over, obtain an explicit
handoff or maintainer decision in the issue first.

Record blockers and material discoveries in the issue as they arise. When work
stops, post a Release comment with the verified state and next action so another
agent can resume without reconstructing the session.

## Local Archive

`.scratch/` is non-canonical migration and archive context. Migrate every active
local roadmap item and ticket to a GitHub issue, preserving decisions and links.
After migration, GitHub determines status, blockers, claims, and completion;
local files may retain historical context or point to their GitHub issue, but
must not be used to coordinate active work.
