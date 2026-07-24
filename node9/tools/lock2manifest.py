#!/usr/bin/env python3
"""lock2manifest.py package-lock.json > manifest

Turn an npm package-lock.json (v2/v3) into a flat "<dir> <tarball-url>" manifest
that n9inst.rc can install on 9front with hget + the native tar.

npm's own installer works on node9 but spends its time in arborist's dependency
resolution, which is what makes a 140-package tree impractical on the box. The
resolution has already happened on the host by the time a lock file exists, so
the manifest carries just the answers: where each package goes and where to get
it. Dev and optional entries are dropped (no native addons run on Plan 9).
"""
import json
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: lock2manifest.py package-lock.json > manifest")
    with open(sys.argv[1]) as f:
        lock = json.load(f)
    packages = lock.get("packages")
    if not packages:
        sys.exit("not a lockfileVersion 2/3 package-lock.json (no 'packages')")

    out = []
    for path, meta in packages.items():
        if not path:
            continue  # the root project itself
        if meta.get("dev") or meta.get("optional") or meta.get("devOptional"):
            continue
        if meta.get("link"):
            continue
        resolved = meta.get("resolved")
        if not resolved or not resolved.startswith(("http://", "https://")):
            sys.exit("no tarball url for %s" % path)
        out.append((path, resolved))

    # shallow paths first so a nested node_modules is never clobbered by its parent
    out.sort(key=lambda pair: (pair[0].count("/"), pair[0]))
    for path, resolved in out:
        print("%s %s" % (path, resolved))


if __name__ == "__main__":
    main()
