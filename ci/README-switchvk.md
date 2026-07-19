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

## Local MSYS2 build

Open `E:\bin\msys64\msys2.exe`, enter the `GBAStation_3DS` checkout, and run
the Bash wrapper. Docker and PowerShell wrappers are not used.

If a sibling `switchVK/nvk-switch-25.3.6` or
`switch-nvk/nvk-switch-25.3.6` package exists, simply run:

```sh
bash build_local.sh
```

When the two source repositories are siblings and the SDK package does not
exist yet, this command automatically invokes the sibling
`switchVK/build_local.sh` first. The normal directory layout is:

```text
parent/
├── switchVK/
└── GBAStation_3DS/
```

To rebuild only the driver package explicitly:

```sh
cd ../switchVK
bash build_local.sh --rebuild
```

Or rebuild the sibling driver and GBAStation in one command:

```sh
cd ../GBAStation_3DS
bash build_local.sh --rebuild-switchvk
```

An explicit local SDK can be selected with:

```sh
SWITCH_NVK_ROOT=/path/to/nvk-switch-25.3.6 bash build_local.sh
```

Use `--diagnostic` for the diagnostic variant and `--incremental` to preserve
the existing `build_switch` directory.
