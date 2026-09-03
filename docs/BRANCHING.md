# Branching and Release Lines

Cortrix uses a small public branch surface. Branches support review and integration; immutable annotated tags and GitHub Releases identify published versions.

## Long-lived branches

### `main`

`main` is the only integration branch for ongoing development. It moves as reviewed changes merge and must not be used as the permanent identity of a published version.

Changes reach `main` through pull requests. The branch rules require at least one approval and the configured status checks, reject force pushes, and prevent branch deletion.

### `release/1.0`

`release/1.0` is the stabilization and maintenance line for the 1.0 family. It begins when an exact release candidate is frozen from `main` and can carry release candidates, the final 1.0.0 release, and accepted 1.0.x maintenance work.

New features continue on `main`. A security fix, severe regression fix, release blocker, required test, or accuracy correction normally lands on `main` first and then reaches `release/1.0` through a separate `backport/1.0-*` pull request. If an emergency fix lands on the release line first, an equivalent forward fix must return to `main`.

`release/1.0` is protected at least as strictly as `main`. Direct and force pushes are not part of the normal workflow.

## Short-lived branches

Use one branch and one pull request for one logical change:

```text
feat/<issue-or-topic>
fix/<issue-or-topic>
docs/<issue-or-topic>
security/<issue-or-topic>
legal/<issue-or-topic>
release-prep/<version>
backport/1.0-<issue-or-topic>
```

Maintainers may create short-lived branches in the organization repository for active collaboration. External contributors should normally work from a personal fork.

Delete a topic branch after its pull request is merged or closed. A remote branch is not an issue tracker, release record, experiment archive, or substitute for a tag.

## Pull request lifecycle

1. Start from the latest intended base branch.
2. Keep the pull request focused on one logical change.
3. Run the relevant local checks and include a `Signed-off-by` trailer in every new commit.
4. Push the topic branch and open a pull request against `main` or the intended release line.
5. Resolve review conversations and obtain an approval on the current head commit. A new commit invalidates an older approval.
6. Let the pull request author merge after required checks and review gates pass.
7. Delete the remote topic branch and remove local worktrees only after confirming that no uncommitted work remains.

## Release preparation

For a 1.0 release candidate:

1. Merge accepted product, documentation, security, and license changes into `main`.
2. Freeze an exact candidate and create `release/1.0` from that commit.
3. Create `release-prep/<version>` from `release/1.0` for version synchronization, release notes, and allowed release-only finishing work.
4. Merge the reviewed release-preparation pull request into `release/1.0`.
5. Run the release gate on the exact candidate commit.
6. Create an annotated version tag and GitHub Release only after final human approval.

Do not create a permanent branch for every release candidate. `release/1.0` remains the 1.0 maintenance line; `v1.0.0-rc.1`, `v1.0.0-rc.2`, and later versions are immutable tags.

## Tags

Published version tags are annotated and immutable. Never move or recreate a published tag. Release documentation, artifacts, SBOMs, benchmark references, and support records must use the tag or its complete commit SHA rather than a moving branch name.

## Local worktrees

Git worktrees are local isolation tools for parallel development, review, and validation. A worktree is not a public project state or release identity. Work that needs collaboration, CI, review, or merge must be represented by a commit, branch, and pull request.

Do not copy directories to simulate branches, and do not use a local worktree name as evidence that a change has been reviewed or released.

## Stale branch cleanup

Maintainers may remove a stale remote branch only after verifying all of the following:

- the exact branch head has not changed since the cleanup manifest was reviewed;
- the branch has no commit ahead of its intended base, or an explicit maintainer decision has abandoned or migrated the remaining work;
- no open pull request uses the branch;
- no active ruleset or release process depends on it;
- any published version represented by the branch is already preserved by an immutable tag and GitHub Release.

Branch deletion authorization applies only to the reviewed manifest. It is not a standing permission to delete future branches.
