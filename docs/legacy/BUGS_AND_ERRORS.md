# JoJoFighter — Bugs and Errors

Bug and error tracking for the JoJoFighter project (Android build; the PSP
build shares the same gameplay source through `Android/app/jni/src/main.c`).

Related documents:

- [CHANGELOG.md](CHANGELOG.md) — milestone history and test status
- [JFA1.md](JFA1.md) — animation format specification

## How this document is maintained

### Rules

1. **Record confirmed bugs and errors.** An entry describes something that was
   actually observed to happen (on a device, in a host test, or in a build).
2. **Keep suspected problems separate from confirmed bugs.** Doubts,
   theoretical concerns and single observations belong in
   [Suspected problems](#suspected-problems-unconfirmed), never in the active
   bug list.
3. **Never invent issues or mark an issue fixed without evidence.** Every
   status change to *Fixed* must name the commit, file or test that shows the
   fix. Every change to *Verified* must name the check that passed.
4. **Mark a fix as Verified only after the relevant test passes** — a host
   test, a build, or a device test reported by the user. Never mark device
   testing as passed on the project's own assumption.
5. **Preserve resolved entries as project history.** Fixed, Verified and
   Won't Fix entries stay in the document; they are moved to
   [Resolved entries](#resolved-entries) but never deleted or renumbered.
6. **Assign new unique IDs without reusing old ones.** IDs are permanent.
   The next free ID is recorded in [ID register](#id-register).

### Fields used by every entry

| Field | Meaning |
|---|---|
| **ID** | `BUG-NNN`, assigned once, never reused |
| **Title** | One line naming the fault |
| **Description** | What is wrong, in enough detail to recognise it |
| **Category** | `gameplay`, `animation`, `rendering`, `controls`, `Android`, `build`, `performance`, `other` |
| **Severity** | `Critical` (unplayable / data loss), `High` (feature broken, no workaround), `Medium` (feature degraded, workaround exists), `Low` (cosmetic or minor) |
| **Status** | `Open`, `In Progress`, `Fixed`, `Verified`, `Won't Fix` |
| **Affected version** | Milestone and/or commit where the fault is present |
| **Steps to reproduce** | Exact, repeatable sequence |
| **Expected behavior** | What should happen |
| **Actual behavior** | What happens instead |
| **Suspected cause** | Hypothesis, explicitly marked *unconfirmed* unless proven; if the cause is known, state the evidence |
| **Resolution** | What was changed, with commit SHA / file / function |
| **Verification** | The check that passed and when; device results only when reported by the user |

Status meanings:

- **Open** — confirmed, not being worked on yet.
- **In Progress** — actively being investigated or fixed.
- **Fixed** — a change addressing it is committed, but the relevant test has
  not passed yet.
- **Verified** — the relevant test (host test, build, or user-reported device
  test) passed after the fix.
- **Won't Fix** — deliberately accepted; the reason must be recorded.

## Development workflow (required before each milestone)

Before each development milestone, Freebuff must:

1. Read both documentation files — this one and
   [CHANGELOG.md](CHANGELOG.md).
2. Review open bugs and known limitations, including the suspected-problems
   list.
3. Plan related features as a development group, so that fixes and features
   that touch the same code are designed together.
4. Run the automated tests after implementation (host tests and the CI APK
   build).
5. Record newly discovered issues here, as confirmed bugs or as suspected
   problems, whichever is accurate.
6. Update the changelog with the milestone entry, its test status and the
   commit SHA.
7. Prepare one combined APK for device testing when appropriate, rather than
   several overlapping builds.
8. Never mark device testing as passed until the user reports the result.

## Active bugs

No confirmed, unresolved bugs are currently recorded.

This list is intentionally empty: no bug has been confirmed against the
current build. Theoretical concerns and review observations are not recorded
here (see the rules above).

## Suspected problems (unconfirmed)

None recorded.

Add a suspected problem only when there is a concrete observation that has not
yet been confirmed, and state what would confirm it.

## Resolved entries

None yet. Resolved entries are kept here as project history.

## ID register

- Next free ID: **BUG-001**
- Assigned IDs: none

When the first bug is recorded, change the register to
`Next free ID: BUG-002` / `Assigned IDs: BUG-001`, and so on.

## Entry template

```markdown
### BUG-001 — <one-line title>

- **Category:** gameplay | animation | rendering | controls | Android | build | performance | other
- **Severity:** Critical | High | Medium | Low
- **Status:** Open | In Progress | Fixed | Verified | Won't Fix
- **Affected version/milestone:** <milestone, commit SHA>
- **Reported:** <date and where it was seen: device model, host test, CI log>

**Description**

<What is wrong.>

**Steps to reproduce**

1. <step>
2. <step>
3. <step>

**Expected behavior**

<What should happen.>

**Actual behavior**

<What happens instead. Include logs or measurements when available.>

**Suspected cause (unconfirmed)**

<Hypothesis only. Remove this heading and state the cause plainly once it is
proven by a test or a code reading.>

**Resolution**

<What was changed, with commit SHA, file and function. Leave empty while open.>

**Verification**

<The exact check that passed and its result. Device results only when reported
by the user. Leave empty until a check has passed.>
```
