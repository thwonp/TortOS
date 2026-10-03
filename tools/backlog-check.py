#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural checks on BACKLOG.md.

The backlog is the other half of an open-item store split across two repos:
Diatom's docs/scoping-register.md owns Diatom's design scope, this file owns
TortOS work and anything crossing the socket. The register has had
check-register.py since 2026-08-25. This file had nothing, and drifted exactly
as far as attention allowed.

Two failures, both from 2026-09-05, both invisible until someone looked:

  - `DECIDED, NOT BUILT: how game audio reaches a Bluetooth sink` was still
    the heading while that work was being finished. Additions get written down
    on discovery; completions have no moment that demands it.
  - Four Diatom-owned sections were sitting here, where check-register cannot
    see them. One of them says in its own text "Not a TortOS bug".

So this exists for the reason check-seam and check-register do. Those rules
hold because something fails when they are broken, not because anyone
remembered. Nothing ever failed when this file drifted, so it drifted.

Same two severities as check-register.py, and the same reasoning:

  FAIL   structural facts a machine can be certain about - a settled heading
         still listed as work to pick up, a missing sweep date, the file's own
         permissions.
  WARN   suspected omissions. Whether a section belongs in the table is a
         judgment, and a check that guesses wrong loudly is one people stop
         reading.

The file is gitignored, so a worktree or a fresh clone will not have it. That
is not a failure here: the check reports it and exits 0, because failing a
build over an absent untracked file would just teach people to skip the suite.
"""
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKLOG = os.path.join(ROOT, "BACKLOG.md")

SWEPT = re.compile(r"\*\*Last swept:\s*(\d{4})-(\d{2})-(\d{2})", re.I)
HEADING = re.compile(r"^## (.+?)\s*$", re.M)
ROW = re.compile(r"^\|\s*\d+\s*\|\s*(.+?)\s*\|[^|]*\|[^|]*\|\s*$", re.M)
PICKUP = "## What to pick up next"
# A decision is a completion, even when nothing was built. Without DECIDED
# here, a decided item counts as live and gets nagged for a pick-up row it
# should not have, while the table still listing it goes unnoticed.
SETTLED = re.compile(r"\b(DONE|CLOSED|DECIDED|FIXED|RESOLVED|SUPERSEDED)\b", re.I)

# Headings that describe the file rather than an item of work.
META = ("what to pick up next", "where diatom items live")

# A sweep older than this is not wrong, only unverified. Long enough that a
# quiet fortnight does not nag, short enough to catch a month of drift.
STALE_DAYS = 14

STOP = {
    "about", "after", "against", "already", "another", "anything", "because",
    "before", "being", "between", "cannot", "could", "every", "first", "from",
    "have", "here", "into", "itself", "just", "like", "might", "much", "must",
    "never", "only", "other", "same", "should", "since", "some", "still",
    "than", "that", "them", "then", "there", "these", "they", "thing", "this",
    "those", "through", "under", "until", "what", "when", "where", "which",
    "while", "will", "with", "would", "things", "their", "left", "over",
}


def subject(text):
    """Distinctive tokens: acronyms, and words long enough to mean something."""
    toks = set()
    for w in re.findall(r"[A-Za-z_][A-Za-z0-9_.-]+", text):
        w = w.strip(".-_")
        if w.isupper() and len(w) >= 2:
            toks.add(w.lower())
        elif len(w) >= 4 and w.lower() not in STOP:
            toks.add(w.lower())
    return toks


def same_subject(a, b, floor):
    """Shared distinctive vocabulary, with the threshold passed in rather than
    fixed, because the two directions want different certainty.

    Saying a settled section is still listed as work is a FAIL, so it takes two
    shared words: `AUDIO: resolved - an 18 dB typo` and `An audio player` share
    only `audio` and are not the same subject, and the first version of this
    check said they were. Saying an open section is missing from the table is a
    WARN, so one is enough - `Over The Hare` has exactly one word in it."""
    shared = a & b
    if not a or not b or len(shared) < floor:
        return None
    return shared


def gitignored(path):
    try:
        r = subprocess.run(["git", "check-ignore", "-q", path], cwd=ROOT)
        return r.returncode == 0
    except FileNotFoundError:
        return None


def main():
    if not os.path.exists(BACKLOG):
        print("backlog: BACKLOG.md is not here, which is expected in a worktree")
        print("         or a fresh clone - it is gitignored. Nothing to check.")
        return 0

    with open(BACKLOG, encoding="utf-8") as f:
        text = f.read()

    fails, warns = [], []

    # 1. The two properties the file is required to have. Both are one syscall
    #    and both have been silently lost before by an editor writing a new
    #    file rather than the same inode.
    mode = os.stat(BACKLOG).st_mode & 0o777
    if mode != 0o600:
        fails.append("BACKLOG.md is mode %o, not 600" % mode)
    ign = gitignored(BACKLOG)
    if ign is False:
        fails.append("BACKLOG.md is not gitignored and would be committed")
    elif ign is None:
        warns.append("no git here, so the gitignore rule could not be checked")

    # 2. A sweep date, so staleness is visible instead of inferred. This is the
    #    whole complaint that produced this file: nobody could tell by looking.
    m = SWEPT.search(text)
    if not m:
        fails.append("no `**Last swept: YYYY-MM-DD**` line - staleness has to be "
                     "visible in the file, not remembered")
        swept = None
    else:
        try:
            swept = datetime.date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
        except ValueError:
            fails.append("the Last swept date is not a real date: %s" % m.group(0))
            swept = None
    if swept:
        age = (datetime.date.today() - swept).days
        if age < 0:
            fails.append("Last swept is %d days in the future (%s)" % (-age, swept))
        elif age > STALE_DAYS:
            warns.append("last swept %s, %d days ago - the table is describing a "
                         "state nobody has confirmed" % (swept, age))

    headings = HEADING.findall(text)

    # Only the pick-up table. The file has two other tables - a core/turbo
    # matrix and the mixer controls - and counting their rows as work made this
    # report claim 18 items where the table has 13.
    i = text.find(PICKUP)
    if i < 0:
        rows = []
        fails.append("no `%s` heading, which is where the table lives" % PICKUP)
    else:
        j = text.find("\n## ", i + 1)
        rows = ROW.findall(text[i:j if j > 0 else len(text)])
    if not rows:
        fails.append("the pick-up table has no rows, so nothing says what is next")

    work = [h for h in headings if h.lower() not in META]
    settled = [h for h in work if SETTLED.search(h)]
    live = [h for h in work if not SETTLED.search(h)]

    # 3. The exact failure of 2026-09-05: a heading says the work is finished
    #    and the table still lists it as the thing to pick up.
    for h in settled:
        hs = subject(SETTLED.sub("", h))
        for r in rows:
            shared = same_subject(hs, subject(r), floor=2)
            if shared:
                fails.append("`%s`\n        is marked settled, but the pick-up table "
                             "still lists:\n        `%s`\n        (shared: %s)"
                             % (h[:88], r[:88], ", ".join(sorted(shared)[:4])))
                break

    # 4. The other direction: open work that the table does not mention, so
    #    reading the table does not tell you what is left. A judgment, hence
    #    a warning - some sections are reference rather than work.
    for h in live:
        hs = subject(h)
        if not any(same_subject(hs, subject(r), floor=1) for r in rows):
            warns.append("`%s`\n        is open but has no row in the pick-up table"
                         % h[:88])

    print("backlog: %d sections (%d live, %d settled), %d rows in the table"
          % (len(work), len(live), len(settled), len(rows)))
    if swept:
        print("         last swept %s" % swept)

    for w in warns:
        print("\nWARN  " + w)
    for f in fails:
        print("\nFAIL  " + f)

    if fails:
        print("\n%d structural problem(s). The backlog cannot be trusted to say "
              "what is left." % len(fails))
        return 1
    if warns:
        print("\nok, with %d item(s) worth a look. Warnings do not fail." % len(warns))
    else:
        print("\nok: the backlog says what is left")
    return 0


if __name__ == "__main__":
    sys.exit(main())
