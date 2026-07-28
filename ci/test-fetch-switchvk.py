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

            if self.path.endswith("/releases/latest"):
                payload = json.dumps(
                    {
                        "tag_name": "switchvk-mesa-26.1.4-test",
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
        with tempfile.TemporaryDirectory(
            prefix=".switchvk-fetch-test-", dir=repository
        ) as temp:
            temp_path = pathlib.Path(temp)
            lock = temp_path / "switchvk.lock"
            destination = temp_path / "sdk"
            lock.write_text(
                "\n".join(
                    [
                        'SWITCHVK_REPOSITORY="beiklive/switchVK"',
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
            bash = env.get("BASH", "bash")
            script = repository / "ci/fetch-switchvk.sh"
            destination_arg = str(destination)
            if pathlib.Path(bash).is_absolute():
                bash_directory = pathlib.Path(bash).parent
                env["PATH"] = f"{bash_directory}{os.pathsep}{env.get('PATH', '')}"
                cygpath = bash_directory / "cygpath.exe"
                if cygpath.is_file():
                    def msys_path(path: pathlib.Path) -> str:
                        return subprocess.check_output(
                            [str(cygpath), "-u", str(path)], text=True
                        ).strip()

                    env["SWITCHVK_LOCK_FILE"] = msys_path(lock)
                    script = msys_path(script)
                    destination_arg = msys_path(destination)
            subprocess.run(
                [
                    bash,
                    str(script),
                    destination_arg,
                ],
                cwd=repository,
                env=env,
                check=True,
            )
            library = destination / "nvk-switch-26.1.4/lib/libvulkan.a"
            if not library.is_file():
                raise RuntimeError("fetch test did not install libvulkan.a")
    finally:
        server.shutdown()
        server.server_close()

    print("fetch-switchvk integration test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
