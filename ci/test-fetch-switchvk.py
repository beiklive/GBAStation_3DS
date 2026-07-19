#!/usr/bin/env python3
"""Local integration test for fetch-switchvk.sh using a mock GitHub API."""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test-fetch-switchvk.py SDK_ARCHIVE REPOSITORY_ROOT", file=sys.stderr)
        return 2

    archive = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(sys.argv[2]).resolve()
    archive_bytes = archive.read_bytes()
    archive_sha256 = hashlib.sha256(archive_bytes).hexdigest()
    archive_name = archive.name
    checksum_name = f"{archive_name}.sha256"
    checksum_bytes = f"{archive_sha256}  {archive_name}\n".encode()
    expected_token = "github_pat_switchvk_fetch_validation"

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            if self.headers.get("Authorization") != f"Bearer {expected_token}":
                self.send_error(401)
                return

            if self.path.endswith("/releases/tags/container-validation"):
                payload = json.dumps(
                    {
                        "assets": [
                            {"name": archive_name, "url": f"{api_root}/assets/1"},
                            {"name": checksum_name, "url": f"{api_root}/assets/2"},
                        ]
                    }
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
            elif self.path == "/assets/1":
                payload = archive_bytes
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
            elif self.path == "/assets/2":
                payload = checksum_bytes
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
            else:
                self.send_error(404)
                return

            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, _format: str, *_args: object) -> None:
            return

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    api_root = f"http://127.0.0.1:{server.server_port}"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        with tempfile.TemporaryDirectory(prefix="switchvk-fetch-test-") as temp:
            temp_path = pathlib.Path(temp)
            lock = temp_path / "switchvk.lock"
            destination = temp_path / "sdk"
            lock.write_text(
                "\n".join(
                    [
                        'SWITCHVK_REPOSITORY="beiklive/switchVK"',
                        'SWITCHVK_TAG="container-validation"',
                        f'SWITCHVK_ASSET="{archive_name}"',
                        f'SWITCHVK_CHECKSUM_ASSET="{checksum_name}"',
                        'SWITCHVK_ROOT_DIRECTORY="nvk-switch-25.3.6"',
                        'SWITCHVK_SHA256=""',
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            env = os.environ.copy()
            env.update(
                {
                    "GITHUB_API_URL": api_root,
                    "SWITCHVK_LOCK_FILE": str(lock),
                    "SWITCHVK_TOKEN": expected_token,
                }
            )
            subprocess.run(
                ["bash", str(repository / "ci/fetch-switchvk.sh"), str(destination)],
                cwd=repository,
                env=env,
                check=True,
            )
            library = destination / "nvk-switch-25.3.6/lib/libvulkan.a"
            if not library.is_file():
                raise RuntimeError("fetch test did not install libvulkan.a")
    finally:
        server.shutdown()
        server.server_close()

    print("fetch-switchvk integration test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
