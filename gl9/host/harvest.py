#!/usr/bin/env python3
# harvest.py — turn Mesa's meson-emitted compile_commands.json into harvest.json:
# a per-translation-unit spec (source, language, scrubbed defines, remapped include
# dirs, std, arch flags) that build-gl9.py compiles with cc9's clang for 9front.
#
# WHY: meson knows the exact -D/-I/-std for every file (incl. generated sources
# under build-gen). We reuse that verbatim EXCEPT for the Linux/glibc/asm feature
# macros that don't exist on 9front+cc9 — dropping those makes Mesa's own portable
# fallback branches compile. See the SCRUB set below; each entry is a define whose
# guarded code path can't build or link under cc9 (the plan's "scrub Linuxisms").
#
# The build-gen tree is produced by host/linux-configure.sh inside a linux/amd64
# container that mounts the repo at /work, so paths in compile_commands.json are
# container-absolute (/work/gl9/...) or relative to the build dir. We remap the
# container repo root to the host repo root and resolve everything to host-absolute.
#
#   python3 harvest.py [--build-gen DIR] [--repo ROOT] [--container-root /work]
#                      [-o harvest.json]
import argparse, json, os, shlex, sys

# Defines to DROP (match by macro name, before any '='). Everything else meson
# passes is kept — including all clang builtins and function attributes, which are
# genuinely available. These are the ones whose code path can't compile/link on
# 9front+cc9, or that we deliberately route around.
SCRUB = {
    # --- headers 9front/cc9 doesn't have -> include would fail ---
    "HAVE_ENDIAN_H", "HAVE_DLFCN_H", "HAVE_SYS_SHM_H", "HAVE_SYS_INOTIFY_H",
    "HAVE_LINUX_FUTEX_H", "HAVE_CET_H", "MAJOR_IN_SYSMACROS",
    # --- Linux/glibc syscalls & libc extensions with no cc9 analogue ---
    "HAVE_MEMFD_CREATE", "HAS_SCHED_H", "HAS_SCHED_GETAFFINITY",
    "HAVE_PTHREAD_SETAFFINITY", "ALLOW_KCMP", "HAVE_FLOCK",
    "HAVE_POSIX_FALLOCATE", "HAVE_MKOSTEMP", "HAVE_GETRANDOM", "HAVE_RANDOM_R",
    "HAVE_SECURE_GETENV", "HAVE_GNU_QSORT_R", "HAVE_STRTOD_L",
    "HAVE_PROGRAM_INVOCATION_NAME", "HAVE_DLADDR", "HAVE_DL_ITERATE_PHDR",
    "HAVE_REALLOCARRAY", "HAVE_POSIX_MEMALIGN", "HAVE_DIRENT_D_TYPE",
    "HAVE_ISSIGNALING",
    # --- optional libs we don't link on the target ---
    "HAVE_ZLIB", "HAVE_COMPRESSION", "USE_DRICONF", "HAVE_OPENMP",
    # --- force the portable path, not Linux futex or hand-written asm dispatch ---
    # HAVE_LINUX_FUTEX_H already dropped; drop the x86-64 asm glapi stubs so the
    # C dispatch (entry.c / glapi_gentable) is used — the .S TLS relocations don't
    # survive elf2aout.
    "USE_X86_64_ASM",
}

# Compiler-flag prefixes we carry through to cc9 clang. Everything not matching is
# dropped (we supply our own -c/-o/-O/target/nostdlib in build-gl9.py). -D is
# handled separately (with scrubbing). Forced includes (-include) and -isystem are
# kept and remapped.
KEEP_PREFIX = ("-I", "-isystem", "-include", "-std=", "-m", "-f")
# ...but a few -f/-m flags are host-toolchain noise or conflict with the cc9 target.
DROP_EXACT = {"-fdiagnostics-color=always", "-pipe", "-fno-common"}


def macro_name(tok):
    return tok[2:].split("=", 1)[0]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_default = os.path.dirname(os.path.dirname(here))  # .../agent9
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-gen", default=os.path.join(here, "..", "build-gen"))
    ap.add_argument("--repo", default=repo_default)
    ap.add_argument("--container-root", default="/work")
    ap.add_argument("-o", "--out", default=os.path.join(here, "harvest.json"))
    a = ap.parse_args()

    build_gen = os.path.abspath(a.build_gen)
    repo = os.path.abspath(a.repo)
    cc_json = os.path.join(build_gen, "compile_commands.json")
    entries = json.load(open(cc_json))

    # Container build dir (what `directory` says) -> host build dir. meson records
    # directory as <container-root>/gl9/build-gen; map its prefix to the host repo.
    def remap(p):
        # container-absolute -> host-absolute
        if p.startswith(a.container_root + "/"):
            return os.path.normpath(repo + "/" + p[len(a.container_root) + 1:])
        return p

    def resolve(p, base):
        # base is the (host) build dir; meson paths are relative to it
        if os.path.isabs(p):
            return remap(p)
        return os.path.normpath(os.path.join(base, p))

    out = []
    dropped_defs, dropped_flags = set(), set()
    for e in entries:
        toks = shlex.split(e.get("command") or " ".join(e.get("arguments", [])))
        comp = toks[0]
        lang = "c++" if comp in ("c++", "g++", "clang++") else "c"
        src = resolve(e["file"], build_gen)
        defines, includes, std, arch, forced = [], [], None, [], []
        i = 1
        while i < len(toks):
            t = toks[i]
            if t.startswith("-D"):
                if macro_name(t) in SCRUB:
                    dropped_defs.add(t)
                else:
                    # Some -D values embed a build-dir path (e.g. mapi's
                    # -DMAPI_ABI_HEADER="/work/gl9/build-gen/.../glapi_mapi_tmp.h").
                    # Remap the container root inside the value too.
                    defines.append(t.replace(a.container_root + "/gl9",
                                              repo + "/gl9"))
            elif t.startswith("-I"):
                includes.append("-I" + resolve(t[2:], build_gen))
            elif t == "-isystem":
                includes.append("-isystem"); includes.append(resolve(toks[i + 1], build_gen)); i += 1
            elif t == "-include":
                forced.append("-include"); forced.append(resolve(toks[i + 1], build_gen)); i += 1
            elif t.startswith("-std="):
                std = t
            elif t in DROP_EXACT:
                dropped_flags.add(t)
            elif t.startswith("-m"):
                arch.append(t)
            i += 1
        out.append({
            "src": src, "lang": lang, "std": std,
            "defines": defines, "includes": includes, "arch": arch, "forced": forced,
            # meson's object name, used to derive a unique .o path
            "obj": e.get("output") or os.path.basename(src) + ".o",
        })

    json.dump({"repo": repo, "build_gen": build_gen, "entries": out},
              open(a.out, "w"), indent=1)
    nc = sum(1 for x in out if x["lang"] == "c")
    print(f"harvested {len(out)} TUs ({nc} C, {len(out)-nc} C++) -> {a.out}")
    print(f"scrubbed {len(dropped_defs)} distinct defines, {len(dropped_flags)} flags")
    for d in sorted(dropped_defs):
        print("  drop", d)


if __name__ == "__main__":
    main()
