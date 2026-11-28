#!/usr/bin/env python3
"""Finds the shaders where the light is lost, without running the game.

Why this exists
---------------
On a card without native ray tracing the proxy rewrites every ray query into
"nothing was hit" (see StripRayQuery in dllmain.cpp). A shader that computed
indirect light, reflections or shadowing from those hits then multiplies the
lighting by zero - a black zone. The only real cure is to put a hand-made
replacement into spv_replace, because a replacement file is used verbatim
instead of being rewritten.

Picking WHICH module deserves a replacement was guesswork. This tool reads the
modules the proxy dumped (DumpLookupInput=N in VulkanProxy.ini, files named
spv_input_<stage>_<number>_<hash>.spv) and ranks them by evidence inside the
SPIR-V itself:

    ray_query      uses OpRayQuery* instructions -> the rewrite definitely
                   removed its ray evaluation (strongest signal)
    ray_stage      ray generation / miss / closest hit stage -> cannot be
                   rewritten at all, it is withheld entirely
    light_names    function/variable names mention light, shadow, gi, probe,
                   reflection, irradiance ... (a hint, not a proof)
    image_samples  how many OpImageSample* the module performs (a lighting
                   shader that reads shadow/light maps)

Nothing here modifies anything: it only prints a table and, for the top
candidates, the exact file name the replacement file must have.

Usage
-----
    python tools/spv_light_report.py "D:\\game\\The Great Circle"
    python tools/spv_light_report.py path\\to\\dir --top 20 --json report.json

Exit code 1 if no spv_input_*.spv files were found (i.e. the dump was not
enabled for that run), 0 otherwise.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- SPIR-V constants used here (no external tools needed) -------------------

OP_STRING = 7            # OpString (debug names)
OP_LINE = 8              # OpLine
OP_EXT_INST_IMPORT = 11
OP_ENTRY_POINT = 15
OP_NAME = 5
OP_MEMBER_NAME = 6
OP_CAPABILITY = 17
OP_DECORATE = 71

CAP_RAY_QUERY = 4472
CAP_RAY_TRACING = 4479

EXEC_MODELS = {
    0: "vertex",
    4: "fragment",
    5: "compute",
    1: "tess-control",
    2: "tess-eval",
    3: "geometry",
    5267: "task-NV", 5268: "mesh-NV",
    5364: "task-EXT", 5365: "mesh-EXT",
    5313: "raygen",
    5314: "intersection",
    5315: "anyhit",
    5316: "closesthit",
    5317: "miss",
    5318: "callable",
}

# OpRayQuery* family: 4473..4479 are the initialize/terminate/proceed family,
# 6016..6032 are the getters (origin, direction, candidate, ...). Both mean the
# module actually evaluates rays.
def is_ray_query_op(op: int) -> bool:
    return 4473 <= op <= 4479 or 6016 <= op <= 6032


# Words that show up in lighting-related debug names. Lowercase, matched as
# substrings, so "Lightmap", "DirectLighting", "ShadowCascade" all hit.
LIGHT_WORDS = (
    "light", "shadow", "irradiance", "radiance", "reflection", "reflect",
    "gi", "globalgi", "probe", "ambient", "occlusion", "ao", "sky",
    "indirect", "diffuse", "specular", "raytrace", "rayquery", "lumen",
    "ssr", "ssao", "ibl",
)


@dataclass
class Module:
    path: Path
    stage: str = "?"
    model: int = -1
    hash: str = ""
    caps: list[str] = field(default_factory=list)
    ray_query_ops: int = 0
    image_samples: int = 0
    names: list[str] = field(default_factory=list)
    light_hits: list[str] = field(default_factory=list)
    words: int = 0
    error: str = ""

    # A single ordering number, so the table can be sorted without arguing about
    # what "more important" means: evidence in the module beats a guess from names.
    @property
    def score(self) -> tuple[int, int, int, int]:
        return (
            1 if self.ray_query_ops else 0,
            1 if self.stage in ("raygen", "miss", "closesthit", "anyhit", "intersection", "callable") else 0,
            len(self.light_hits),
            self.image_samples,
        )

    @property
    def verdict(self) -> str:
        if self.error:
            return f"unreadable: {self.error}"
        if self.ray_query_ops:
            return "ROBUST AIM: evaluates rays, its lighting was rewritten to 'no hit'"
        if self.stage in ("raygen", "miss", "closesthit", "anyhit", "intersection", "callable"):
            return "ROBUST AIM: ray-tracing stage, currently withheld completely"
        if self.light_hits:
            return "worth a look: lighting names, but no ray instructions (may be lit through other paths)"
        return "unlikely to be a lighting shader"

    @property
    def replacement_name(self) -> str:
        return f"spv_{self.hash}.spv" if self.hash else "(hash unknown)"


def read_words(path: Path) -> list[int]:
    raw = path.read_bytes()
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError(f"not a SPIR-V file ({len(raw)} bytes)")
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    if words[0] != 0x07230203:
        raise ValueError("bad SPIR-V magic")
    return words


def parse(path: Path) -> Module:
    m = Module(path=path)
    # The proxy names the dump spv_input_<model>_<module#>_<hash>.spv; fall back to
    # the older spv_input_<model>_<module#>.spv so old dumps still work.
    parts = path.stem.split("_")
    if len(parts) >= 3 and parts[0] == "spv" and parts[1] == "input":
        try:
            m.model = int(parts[2])
        except ValueError:
            pass
    # Hash is the last field before the trailing module number, whichever naming
    # scheme produced the file:
    #   older:  spv_input_<stage>_<module>_<hash>_<module>.spv   (module number twice)
    #   v22:    spv_input_<stage>_<hash>_<module>.spv
    # parts: ['spv','input',stage, ...rest]; the 16-hex field is the hash.
    for part in reversed(parts[3:]):
        if len(part) == 16 and all(c in "0123456789ABCDEFabcdef" for c in part):
            m.hash = part.upper()
            break
    try:
        words = read_words(path)
    except ValueError as e:
        m.error = str(e)
        return m

    m.words = len(words)
    i = 5
    while i < 5 + 5 or i < len(words):
        if i >= len(words):
            break
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            m.error = "truncated instruction stream"
            break
        body = words[i + 1:i + wc]
        if op == OP_ENTRY_POINT and len(body) >= 2 and m.model < 0:
            m.model = body[0]
        elif op == OP_CAPABILITY and len(body) >= 1:
            c = body[0]
            name = f"cap{c}"
            if c == CAP_RAY_QUERY:
                name = "RayQueryKHR"
            elif c == CAP_RAY_TRACING:
                name = "RayTracingKHR"
            m.caps.append(name)
        elif op in (OP_NAME, OP_MEMBER_NAME, OP_STRING):
            chars: list[str] = []
            for w in body[1:] if op != OP_STRING else body:
                for shift in (0, 8, 16, 24):
                    ch = (w >> shift) & 0xFF
                    if ch == 0:
                        break
                    chars.append(chr(ch) if 32 <= ch < 127 else " ")
            s = "".join(chars).strip()
            if s:
                m.names.append(s)
        elif is_ray_query_op(op):
            m.ray_query_ops += 1
        elif op == 87:      # OpImageSampleImplicitLod
            m.image_samples += 1
        elif op == 88:      # OpImageSampleExplicitLod
            m.image_samples += 1
        elif op == 95:      # OpImageSampleProjImplicitLod
            m.image_samples += 1
        elif op == 96:      # OpImageSampleProjExplicitLod
            m.image_samples += 1
        i += wc

    m.stage = EXEC_MODELS.get(m.model, f"model{m.model}" if m.model >= 0 else "?")
    seen: set[str] = set()
    for s in m.names:
        low = s.lower()
        for w in LIGHT_WORDS:
            if w in low and s not in seen:
                seen.add(s)
                m.light_hits.append(s)
                break
    return m


def human(n: int) -> str:
    return f"{n:,}".replace(",", " ")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Rank dumped shaders by how likely they are to be where the light is lost.")
    ap.add_argument("paths", nargs="+", type=Path, help="game folder (contains spv_input_*.spv) or the files themselves")
    ap.add_argument("--top", type=int, default=15, help="how many candidates to print in detail (default 15)")
    ap.add_argument("--all", action="store_true",
                    help="also list modules with no evidence (useful after DumpLookupInput=-1, "
                         "which dumps every module: this is the full inventory of the shader set)")
    ap.add_argument("--json", type=Path, help="write the full table as JSON to this file")
    args = ap.parse_args(argv)

    files: list[Path] = []
    for p in args.paths:
        if p.is_dir():
            files.extend(sorted(p.glob("spv_input_*.spv")))
        elif p.is_file():
            files.append(p)
        else:
            print(f"skip: not found: {p}")
    if not files:
        print("No spv_input_*.spv found.")
        print("Enable the dump for one run: in VulkanProxy.ini set DumpLookupInput=32,")
        print("play for two minutes where it is dark, then run this again.")
        print("Afterwards put DumpLookupInput=0 back.")
        return 1

    mods = [parse(f) for f in files]
    good = [m for m in mods if not m.error]
    bad = [m for m in mods if m.error]

    print(f"Read {len(mods)} dumped module(s): {len(good)} parsed, {len(bad)} unreadable.")
    by_stage: dict[str, int] = {}
    for m in good:
        by_stage[m.stage] = by_stage.get(m.stage, 0) + 1
    if by_stage:
        print("By stage: " + ", ".join(f"{k} {v}" for k, v in sorted(by_stage.items())))

    rq = [m for m in good if m.ray_query_ops]
    print()
    print(f"Modules that actually evaluate rays (OpRayQuery*): {len(rq)}")
    if rq:
        print("  These are rewritten to 'no hit' today, so their lighting is what turns black.")
    stages = [m for m in good if m.stage in ("raygen", "miss", "closesthit", "anyhit", "intersection", "callable")]
    if stages:
        print(f"Modules that are ray-tracing stages (withheld completely): {len(stages)}")

    ranked = sorted(good, key=lambda m: m.score, reverse=True)
    if not args.all:
        # After DumpLookupInput=-1 every module is dumped, so the list has to be readable:
        # keep the ones with any evidence and say how many were hidden.
        with_evidence = [m for m in ranked if m.score[0] or m.score[1]]
        hidden = len(ranked) - len(with_evidence)
        if with_evidence:
            ranked = with_evidence
        else:
            hidden = 0
        if hidden:
            print(f"({hidden} module(s) with no ray/light evidence are hidden - add --all to list them)")
    print()
    print("Candidates, best evidence first")
    print(f"{'#':>3}  {'stage':<12} {'rays':>5} {'smp':>4} {'light-names':>11}  {'hash':<16} verdict")
    limit = max(args.top, 1)
    for n, m in enumerate(ranked[:limit], 1):
        print(f"{n:>3}  {m.stage:<12} {m.ray_query_ops:>5} {m.image_samples:>4} {len(m.light_hits):>11}  {m.hash or '-':<16} {m.verdict}")

    actionable = [m for m in ranked if m.score[0] or m.score[1]]
    print()
    if actionable:
        print("To act on this: a replacement file must be named after the hash of the module")
        print("the game compiles on THIS machine, so the name is read from the dump:")
        for m in actionable[:limit]:
            print(f"  {m.path.name}")
            print(f"      -> save the hand-made replacement as: {m.replacement_name}")
            print(f"      -> check it can serve the folder: spv_check.exe \"<spv_replace>\" \"{m.path}\"")
    else:
        print("No module in this dump evaluates rays or is a ray-tracing stage.")
        print("If the game is still dark there, the light is lost somewhere else: try")
        print("DumpLookupInput with a bigger number, or check whether the dark zone looks")
        print("the same with the proxy removed (rename vulkan-1.dll) - that separates")
        print("'the proxy weakened the lighting' from 'the GPU cannot do it at all'.")

    if bad:
        print()
        print("Unreadable dumps (truncated file or the game was closed while writing):")
        for m in bad[:5]:
            print(f"  {m.path.name}: {m.error}")

    if args.json:
        payload = [
            {
                "file": m.path.name,
                "stage": m.stage,
                "hash": m.hash,
                "ray_query_ops": m.ray_query_ops,
                "image_samples": m.image_samples,
                "light_names": m.light_hits[:20],
                "words": m.words,
                "replacement_name": m.replacement_name,
                "verdict": m.verdict,
            }
            for m in ranked
        ]
        args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"\nJSON written to {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
