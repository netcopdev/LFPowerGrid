# Repository Agent Workflow

These instructions apply to the entire repository.

## Branch policy

`dev/integration` is the shared testing and integration branch. It must contain
the latest completed work, but implementation commits must not be created
directly on it.

For every independent change:

1. Start from the latest local `dev/integration`.
2. Create or switch to a dedicated topic branch before editing files.
3. Implement, test, and commit the complete change on that topic branch.
4. Switch back to `dev/integration` and merge the topic branch with an explicit
   merge commit (`git merge --no-ff`).
5. Keep the topic branch after merging so its history and purpose remain clear.
6. Leave `dev/integration` checked out and the worktree clean when finished.

Use descriptive branch names with an appropriate prefix:

- `fix/<short-description>` for bug fixes
- `feature/<short-description>` for new functionality
- `refactor/<short-description>` for internal restructuring
- `chore/<short-description>` for tooling, build, or maintenance work
- `docs/<short-description>` for documentation-only work

Do not combine unrelated work on one topic branch. Continue using an existing
topic branch when the request is clearly a continuation of that same change.

## Required sequence

Before making changes, inspect the active branch, worktree status, and recent
history. Preserve user-owned or unrelated modifications. If the worktree is not
clean and the existing changes overlap the task, stop and ask before switching
branches or merging.

The normal sequence is:

```text
dev/integration (latest)
    -> topic branch
    -> implementation commits and verification
    -> merge --no-ff into dev/integration
    -> test from dev/integration
```

Run checks appropriate to the change on the topic branch before merging. After
the merge, verify the branch graph, merge commit, and clean worktree. Build any
final test package from the merged `dev/integration` revision so its manifest
identifies the exact revision being tested.

If commits were accidentally made directly on `dev/integration`, preserve them
on an appropriate topic branch, restore `dev/integration` to its pre-change
commit, and then merge the topic branch back with `--no-ff`.

## Remote operations

Do not push branches, tags, or merge commits unless the user explicitly asks.
Do not delete topic branches unless the user explicitly asks.
