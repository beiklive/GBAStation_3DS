# switchVK CI dependency

Nintendo Switch builds consume a versioned SDK from the private
`beiklive/switchVK` repository. `ci/switchvk.lock` selects the Release tag and
asset. `ci/fetch-switchvk.sh` downloads through the authenticated GitHub API,
validates SHA-256, rejects unsafe archive paths, and exports
`SWITCH_NVK_ROOT` for CMake.

The `GBAStation_3DS` repository needs this Actions secret:

```text
SWITCHVK_READ_TOKEN
```

Use a fine-grained personal access token scoped only to
`beiklive/switchVK`, with repository permission `Contents: Read-only`.
Never expose this workflow on `pull_request_target` and never pass the token
as a command-line argument.

After the first trusted `switchvk-mesa-25.3.6-r1` release, copy the archive
SHA-256 into `ci/switchvk.lock`, or set the non-secret repository variable
`SWITCHVK_SHA256`. The repository variable takes precedence and avoids a
follow-up source commit.
