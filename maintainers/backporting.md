# Backporting

> **Note:** This document describes upstream NixOS/nix release process. The straylight fork does not
> currently use GitHub Actions workflows for backporting.

To
[automatically backport a pull request](https://github.com/NixOS/nix/blob/master/.github/workflows/backport.yml)
to a release branch once it's merged (in upstream NixOS/nix), assign it a label of the form
[`backport <branch>`](https://github.com/NixOS/nix/labels?q=backport).

Since
[GitHub Actions workflows will not trigger other workflows](https://docs.github.com/en/actions/using-workflows/triggering-a-workflow#triggering-a-workflow-from-a-workflow),
checks on the automatic backport need to be triggered by another actor. This is achieved by closing
and reopening the backport pull request.
