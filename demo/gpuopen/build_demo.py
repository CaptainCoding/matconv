#!/usr/bin/env python3
"""Roundtrip cached GPUOpen materials, optionally downloading the catalog."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape as escape_xml


API_ROOT = "https://api.matlib.gpuopen.com/api/"
ALLOWED_LICENSES = {"MIT Public Domain", "CC0 Public Domain"}
IMAGE_EXTENSIONS = {".bmp", ".exr", ".hdr", ".jpeg", ".jpg", ".png", ".tif", ".tiff"}
MATERIALX_MDL_PATH = Path(
    "build/_deps/matconv_materialx-src/source/MaterialXGenMdl/mdl"
)


def fetch_json(url: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url, headers={"Accept": "application/json", "User-Agent": "matconv-demo/1.0"}
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def fetch_pages(endpoint: str) -> list[dict[str, Any]]:
    url = API_ROOT + endpoint
    results: list[dict[str, Any]] = []
    while url:
        page = fetch_json(url)
        results.extend(page["results"])
        url = page.get("next")
    return results


def parse_size(value: str) -> int:
    match = re.fullmatch(r"\s*([\d.]+)\s*(B|KB|MB|GB)\s*", value, re.I)
    if not match:
        return sys.maxsize
    multipliers = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3}
    return int(float(match.group(1)) * multipliers[match.group(2).upper()])


def select_package(
    material: dict[str, Any], packages_by_id: dict[str, dict[str, Any]]
) -> dict[str, Any] | None:
    packages = [
        packages_by_id[package_id]
        for package_id in material.get("packages", [])
        if package_id in packages_by_id
    ]
    if not packages:
        return None
    preferred = [package for package in packages if package.get("label") == "1k 8b"]
    return min(preferred or packages, key=lambda package: parse_size(package["size"]))


def slugify(value: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    return slug or "material"


def safe_extract(archive: zipfile.ZipFile, destination: Path) -> list[Path]:
    extracted: list[Path] = []
    base = destination.resolve()
    for info in archive.infolist():
        member = PurePosixPath(info.filename)
        if member.is_absolute() or ".." in member.parts:
            raise ValueError(f"Unsafe path in package archive: {info.filename}")
        if info.is_dir():
            continue
        unix_mode = info.external_attr >> 16
        if unix_mode & 0o170000 == 0o120000:
            raise ValueError(f"Symbolic link in package archive: {info.filename}")
        target = destination.joinpath(*member.parts)
        if base not in target.resolve().parents:
            raise ValueError(f"Package member escapes its destination: {info.filename}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(info) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output)
        extracted.append(target)
    return extracted


def resolve_texture_path(source_root: Path, value: str) -> str | None:
    normalized = value.replace("\\", "/")
    path = PurePosixPath(normalized)
    if path.is_absolute() or "://" in normalized or "<" in normalized:
        return None

    current = source_root
    for part in path.parts:
        if part in ("", "."):
            continue
        if part == "..":
            current = current.parent
            if source_root.resolve() not in current.resolve().parents and current.resolve() != source_root.resolve():
                return None
            continue
        try:
            entries = {entry.name: entry.name for entry in current.iterdir()}
        except OSError:
            return None
        actual = entries.get(part)
        if actual is None:
            matches = [
                name for name in entries if name.casefold() == part.casefold()
            ]
            if len(matches) != 1:
                return None
            actual = matches[0]
        current = current / actual

    if not current.is_file():
        return None
    return current.relative_to(source_root).as_posix()


def portable_materialx_source(
    source_material: Path,
) -> tuple[Path, dict[str, Any]]:
    source_text = source_material.read_text(encoding="utf-8")
    repaired: list[dict[str, str]] = []
    missing: list[str] = []
    references = 0

    def normalize_input(match: re.Match[str]) -> str:
        nonlocal references
        tag = match.group(0)
        try:
            element = ET.fromstring(tag)
        except ET.ParseError:
            return tag
        if element.get("name") != "file":
            return tag
        original = element.get("value")
        if not original:
            return tag
        references += 1
        resolved = resolve_texture_path(source_material.parent, original)
        if resolved is None:
            missing.append(original)
            return tag
        if resolved == original:
            return tag

        attribute = re.search(
            r"(\bvalue\s*=\s*)(['\"])(.*?)(\2)", tag, re.DOTALL
        )
        if attribute is None:
            return tag
        escaped = escape_xml(resolved, {'"': "&quot;", "'": "&apos;"})
        repaired.append({"from": original, "to": resolved})
        return (
            tag[: attribute.start(3)]
            + escaped
            + tag[attribute.end(3) :]
        )

    normalized_text = re.sub(r"<input\b[^>]*>", normalize_input, source_text)
    if normalized_text != source_text:
        portable_path = source_material.with_name(
            f"{source_material.stem}_portable{source_material.suffix}"
        )
        portable_path.write_text(normalized_text, encoding="utf-8")
        source_material = portable_path

    return source_material, {
        "image_files": sum(
            1
            for path in source_material.parent.rglob("*")
            if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
        ),
        "references": references,
        "repaired": repaired,
        "missing": sorted(set(missing)),
    }


def run_conversion(
    executable: Path,
    source_material: Path,
    source_root: Path,
    material_name: str,
    roundtrip_root: Path,
    repository: Path,
) -> dict[str, Any]:
    roundtrip_root.mkdir(parents=True, exist_ok=True)
    mdl_output = roundtrip_root / f"{source_material.stem}.mdl"
    mtlx_output = roundtrip_root / f"{source_material.stem}_roundtrip.mtlx"
    to_mdl = subprocess.run(
        [
            str(executable),
            "mtlx-to-mdl",
            "--input",
            str(source_material),
            "--output",
            str(mdl_output),
            "--material",
            material_name,
        ],
        cwd=repository,
        capture_output=True,
        text=True,
        check=False,
    )
    to_mtlx: subprocess.CompletedProcess[str] | None = None
    if to_mdl.returncode == 0 and mdl_output.is_file():
        to_mtlx = subprocess.run(
            [
                str(executable),
                "mdl-to-mtlx",
                "--input",
                str(mdl_output),
                "--output",
                str(mtlx_output),
                "--material",
                material_name,
                "--mdl-path",
                str(source_root),
            ],
            cwd=repository,
            capture_output=True,
            text=True,
            check=False,
        )

    log_path = roundtrip_root / "conversion.log"
    with log_path.open("w", encoding="utf-8") as log:
        log.write("=== MaterialX to MDL ===\n")
        log.write(to_mdl.stdout)
        log.write(to_mdl.stderr)
        if to_mtlx is not None:
            log.write("\n=== MDL to MaterialX ===\n")
            log.write(to_mtlx.stdout)
            log.write(to_mtlx.stderr)

    status = "failed"
    warnings: list[str] = []
    for result in (to_mdl, to_mtlx):
        if result is not None:
            warnings.extend(
                line.strip()
                for line in (result.stdout + result.stderr).splitlines()
                if "Warning:" in line or "warn :" in line or "error:" in line
            )
    if (
        to_mdl.returncode == 0
        and to_mtlx is not None
        and to_mtlx.returncode == 0
        and mtlx_output.is_file()
    ):
        status = "converted"
        try:
            generated = ET.parse(mtlx_output).getroot()
            fallback = next(
                (
                    node
                    for node in generated.findall(".//oren_nayar_diffuse_bsdf")
                    if node.get("name") == "unsupported_surface_bsdf"
                ),
                None,
            )
            if fallback is not None:
                status = "converted_with_black_fallback"
        except ET.ParseError as error:
            status = "invalid_materialx"
            warnings.append(f"Generated MaterialX is not valid XML: {error}")

    return {
        "status": status,
        "mdl": mdl_output,
        "materialx": mtlx_output,
        "log": log_path,
        "warnings": warnings,
    }


def relative_asset(stage_path: Path, asset_path: Path) -> str:
    return Path(os.path.relpath(asset_path, stage_path.parent)).as_posix()


def write_usd_submodel(
    output: Path, record: dict[str, Any], stage_root: Path
) -> None:
    try:
        from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade
    except ImportError as error:
        raise RuntimeError(
            "The USD submodels require Pixar USD Python bindings (`pxr`)."
        ) from error

    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(output))
    if not stage:
        raise RuntimeError(f"Could not create USD stage: {output}")
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    root = UsdGeom.Xform.Define(stage, "/MaterialPair").GetPrim()
    stage.SetDefaultPrim(root)
    root.SetCustomDataByKey("title", record["title"])
    root.SetCustomDataByKey("conversionStatus", record["conversion"]["status"])

    source_asset = stage_root / record["source_material"]
    if not source_asset.is_file():
        raise FileNotFoundError(f"Original MaterialX asset does not exist: {source_asset}")
    material_name = record.get("materialx_name") or source_asset.stem

    def reference_material(name: str, asset: Path, role: str) -> Sdf.Path:
        looks = UsdGeom.Scope.Define(
            stage, f"/MaterialPair/{name}Looks"
        ).GetPrim()
        looks.GetReferences().AddReference(
            relative_asset(output, asset), Sdf.Path("/MaterialX")
        )
        looks.SetCustomDataByKey("role", role)
        looks.SetCustomDataByKey(
            "conversionStatus", record["conversion"]["status"]
        )
        return Sdf.Path(f"{looks.GetPath()}/Materials/{material_name}")

    original_material_path = reference_material(
        "Original", source_asset, "original"
    )
    converted_asset = stage_root / record["conversion"]["materialx"]
    if converted_asset.is_file():
        converted_material_path = reference_material(
            "Converted", converted_asset, "converted"
        )
    else:
        material = UsdShade.Material.Define(
            stage, "/MaterialPair/MissingConvertedMaterial"
        )
        shader = UsdShade.Shader.Define(
            stage, "/MaterialPair/MissingConvertedMaterial/Preview"
        )
        shader.CreateIdAttr("UsdPreviewSurface")
        shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
            Gf.Vec3f(1.0, 0.0, 1.0)
        )
        shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.65)
        shader.CreateOutput("surface", Sdf.ValueTypeNames.Token)
        material.CreateSurfaceOutput().ConnectToSource(
            shader.ConnectableAPI(), "surface"
        )
        material.GetPrim().SetCustomDataByKey("role", "converted")
        material.GetPrim().SetCustomDataByKey(
            "conversionStatus", record["conversion"]["status"]
        )
        material.GetPrim().SetCustomDataByKey(
            "missingConvertedMaterialX", True
        )
        converted_material_path = material.GetPath()

    def create_uv_sphere(name: str, x_position: float) -> UsdGeom.Mesh:
        sectors = 64
        stacks = 32
        vertices = [Gf.Vec3f(x_position, 2.0, 0.0)]
        texcoords = [Gf.Vec2f(0.5, 1.0)]
        normals = [Gf.Vec3f(0.0, 1.0, 0.0)]

        for stack in range(1, stacks):
            v = stack / stacks
            latitude = math.pi * v
            y = math.cos(latitude)
            radial = math.sin(latitude)
            for sector in range(sectors + 1):
                u = sector / sectors
                longitude = 2.0 * math.pi * u
                x = radial * math.sin(longitude)
                z = radial * math.cos(longitude)
                vertices.append(
                    Gf.Vec3f(x_position + x, 1.0 + y, z)
                )
                texcoords.append(Gf.Vec2f(u, 1.0 - v))
                normals.append(Gf.Vec3f(x, y, z))

        bottom_index = len(vertices)
        vertices.append(Gf.Vec3f(x_position, 0.0, 0.0))
        texcoords.append(Gf.Vec2f(0.5, 0.0))
        normals.append(Gf.Vec3f(0.0, -1.0, 0.0))

        counts: list[int] = []
        indices: list[int] = []
        first_ring = 1
        for sector in range(sectors):
            counts.append(3)
            indices.extend((0, first_ring + sector, first_ring + sector + 1))

        for stack in range(stacks - 2):
            upper = 1 + stack * (sectors + 1)
            lower = upper + sectors + 1
            for sector in range(sectors):
                a = upper + sector
                b = a + 1
                d = lower + sector
                c = d + 1
                counts.append(4)
                indices.extend((a, d, c, b))

        last_ring = 1 + (stacks - 2) * (sectors + 1)
        for sector in range(sectors):
            counts.append(3)
            indices.extend(
                (bottom_index, last_ring + sector + 1, last_ring + sector)
            )

        if sum(counts) != len(indices):
            raise RuntimeError(
                f"Generated sphere {name} has {len(indices)} indices for "
                f"{sum(counts)} face vertices."
            )
        if len(vertices) != len(texcoords) or len(vertices) != len(normals):
            raise RuntimeError(
                f"Generated sphere {name} has mismatched vertex, UV, or normal counts."
            )

        mesh = UsdGeom.Mesh.Define(stage, f"/MaterialPair/{name}")
        mesh.CreatePointsAttr(vertices)
        mesh.CreateFaceVertexCountsAttr(counts)
        mesh.CreateFaceVertexIndicesAttr(indices)
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.CreateOrientationAttr(UsdGeom.Tokens.rightHanded)
        mesh.CreateNormalsAttr(normals)
        mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
        primvar = UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(
            "st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex
        )
        primvar.Set(texcoords)
        return mesh

    for name, x_position, material in (
        ("OriginalSphere", -1.25, original_material_path),
        ("ConvertedSphere", 1.25, converted_material_path),
    ):
        sphere = create_uv_sphere(name, x_position)
        prim = sphere.GetPrim()
        UsdShade.MaterialBindingAPI.Apply(prim)
        prim.CreateRelationship("material:binding", custom=False).SetTargets(
            [material]
        )
        prim.SetCustomDataByKey(
            "role", "original" if name == "OriginalSphere" else "converted"
        )

    stage.GetRootLayer().Save()


def write_usd_index(
    output: Path, records: list[dict[str, Any]], stage_root: Path
) -> None:
    try:
        from pxr import Gf, Sdf, Usd, UsdGeom
    except ImportError as error:
        raise RuntimeError(
            "The USD index requires Pixar USD Python bindings (`pxr`)."
        ) from error

    submodels_dir = output.parent / "submodels"
    submodels_dir.mkdir(parents=True, exist_ok=True)
    for record in records:
        write_usd_submodel(
            submodels_dir / f"{record['slug']}.usda", record, stage_root
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(output))
    if not stage:
        raise RuntimeError(f"Could not create USD stage: {output}")
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    root = UsdGeom.Xform.Define(stage, "/GPUOpenMaterials").GetPrim()
    stage.SetDefaultPrim(root)
    root.SetCustomDataByKey(
        "description",
        "Grid of per-material USD submodels. Each submodel has original and converted material spheres.",
    )
    columns = max(1, math.ceil(math.sqrt(len(records))))
    for index, record in enumerate(records):
        slug = record["slug"].replace("-", "_")
        pair = UsdGeom.Xform.Define(
            stage, f"/GPUOpenMaterials/{slug}"
        )
        pair_prim = pair.GetPrim()
        pair_prim.GetReferences().AddReference(
            relative_asset(output, submodels_dir / f"{record['slug']}.usda"),
            Sdf.Path("/MaterialPair"),
        )
        pair.AddTranslateOp().Set(
            Gf.Vec3d((index % columns) * 6.0, 0.0, (index // columns) * 4.5)
        )
        pair_prim.SetCustomDataByKey("title", record["title"])
        pair_prim.SetCustomDataByKey(
            "conversionStatus", record["conversion"]["status"]
        )
    stage.GetRootLayer().Save()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="GPUOpen demo directory (default: this script's directory)",
    )
    parser.add_argument(
        "--matconv",
        type=Path,
        default=Path("build/matconv"),
        help="Path to the built matconv executable",
    )
    parser.add_argument("--jobs", type=int, default=4, help="parallel materials")
    parser.add_argument(
        "--download",
        action="store_true",
        help="download GPUOpen material packages before converting (default: use local catalog)",
    )
    parser.add_argument(
        "--limit", type=int, help="process only the first N selected materials"
    )
    args = parser.parse_args()

    repository = Path(__file__).resolve().parents[2]
    output_dir = args.output_dir.resolve()
    executable = args.matconv
    if not executable.is_absolute():
        executable = repository / executable
    executable = executable.resolve()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    if not executable.is_file():
        parser.error(f"matconv executable not found: {executable}")
    materialx_mdl_path = repository / MATERIALX_MDL_PATH
    if not materialx_mdl_path.is_dir():
        parser.error(
            "MaterialX MDL support modules not found; configure and build with "
            "MATCONV_FETCH_DEPENDENCIES=ON."
        )
    try:
        from pxr import Sdf, Usd, UsdGeom, UsdShade
    except ImportError:
        parser.error("USD Python bindings (`pxr`) are required to generate USDA.")

    jobs: list[dict[str, Any]] = []
    skipped: list[dict[str, str]] = []
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be at least 1")

    if args.download:
        materials = fetch_pages("materials/?limit=100")
        packages = fetch_pages("packages/?limit=100")
        packages_by_id = {package["id"]: package for package in packages}
        selected = [
            material
            for material in materials
            if material.get("license") in ALLOWED_LICENSES
            and material.get("status", "Published") == "Published"
        ]
        if args.limit is not None:
            selected = selected[: args.limit]

        for material in selected:
            package = select_package(material, packages_by_id)
            if package is None:
                skipped.append(
                    {"title": material["title"], "reason": "No downloadable package"}
                )
                continue
            slug = slugify(material["title"])
            jobs.append(
                {
                    "material": material,
                    "package": package,
                    "slug": f"{slug}-{material['id'][:8]}",
                }
            )
        print(
            f"Selected {len(jobs)} of {len(materials)} materials with allowed "
            f"license metadata; skipped {len(skipped)} without packages.",
            flush=True,
        )
    else:
        catalog_dir = output_dir / "catalog"
        for source_material in sorted(catalog_dir.glob("*/source/*.mtlx")):
            if source_material.stem.endswith("_portable"):
                continue
            root = source_material.parent.parent
            metadata_path = source_material.parent / "metadata.json"
            metadata = (
                json.loads(metadata_path.read_text(encoding="utf-8"))
                if metadata_path.is_file()
                else {}
            )
            jobs.append(
                {
                    "slug": root.name,
                    "source_material": source_material,
                    "material_name": metadata.get("materialx_name")
                    or source_material.stem,
                    "metadata": metadata,
                }
            )
        if args.limit is not None:
            jobs = jobs[: args.limit]
        if not jobs:
            parser.error(
                f"No local MaterialX sources found in {catalog_dir}; "
                "run again with --download to fetch them."
            )
        print(
            f"Using {len(jobs)} local MaterialX source(s); no files will be downloaded.",
            flush=True,
        )

    def process(job: dict[str, Any]) -> dict[str, Any]:
        slug = job["slug"]
        root = output_dir / "catalog" / slug
        source_root = root / "source"
        if "source_material" in job:
            source_material = job["source_material"]
            material_name = job["material_name"]
            metadata = job["metadata"]
        else:
            material = job["material"]
            package = job["package"]
            source_root.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="matconv-gpuopen-") as temp_dir:
                archive_path = Path(temp_dir) / "package.zip"
                request = urllib.request.Request(
                    package["file_url"],
                    headers={"User-Agent": "matconv-demo/1.0"},
                )
                with urllib.request.urlopen(request, timeout=180) as response:
                    with archive_path.open("wb") as archive_file:
                        shutil.copyfileobj(response, archive_file)
                with zipfile.ZipFile(archive_path) as archive:
                    extracted = safe_extract(archive, source_root)

            mtlx_files = [
                path for path in extracted if path.suffix.lower() == ".mtlx"
            ]
            preferred_name = material.get("mtlx_filename", "")
            source_material = next(
                (path for path in mtlx_files if path.name == preferred_name),
                mtlx_files[0] if mtlx_files else None,
            )
            if source_material is None:
                raise RuntimeError(f"No .mtlx file in package {package['file']}")

            source_url = (
                "https://api.matlib.gpuopen.com/api/materials/"
                f"{material['id']}/"
            )
            metadata = {
                "title": material["title"],
                "author": material.get("author"),
                "license": material.get("license"),
                "material_id": material["id"],
                "material_url": source_url,
                "package_id": package["id"],
                "package_url": package["file_url"],
                "package_name": package.get("file"),
                "package_label": package.get("label"),
                "package_size": package.get("size"),
                "materialx_name": material.get("mtlx_material_name"),
            }
            (source_root / "metadata.json").write_text(
                json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
            )
            material_name = (
                material.get("mtlx_material_name") or source_material.stem
            )

        original_source_material = source_material
        source_material, texture_audit = portable_materialx_source(
            source_material
        )
        conversion = run_conversion(
            executable,
            source_material,
            source_root,
            material_name,
            root / "roundtrip",
            repository,
        )
        print(f"{slug}: {conversion['status']}", flush=True)
        return {
            **metadata,
            "title": metadata.get("title") or source_material.stem,
            "license": metadata.get("license") or "Unknown",
            "source_material": str(source_material.relative_to(output_dir)),
            "source_material_original": str(
                original_source_material.relative_to(output_dir)
            ),
            "source_texture_audit": texture_audit,
            "source_url": metadata.get("material_url") or "",
            "slug": slug,
            "conversion": {
                "status": conversion["status"],
                "mdl": str(conversion["mdl"].relative_to(output_dir)),
                "materialx": str(conversion["materialx"].relative_to(output_dir)),
                "log": str(conversion["log"].relative_to(output_dir)),
                "warnings": conversion["warnings"],
            },
        }

    records: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_job = {executor.submit(process, job): job for job in jobs}
        for future in concurrent.futures.as_completed(future_to_job):
            job = future_to_job[future]
            try:
                records.append(future.result())
            except Exception as error:
                errors.append(
                    {
                        "title": job["material"]["title"],
                        "material_id": job["material"]["id"],
                        "error": str(error),
                    }
                )
                print(
                    f"{job['slug']}: ERROR: {error}",
                    file=sys.stderr,
                    flush=True,
                )
    records.sort(key=lambda record: record["title"].casefold())
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "api": API_ROOT,
        "allowed_licenses": sorted(ALLOWED_LICENSES),
        "selected_materials": len(jobs),
        "completed": len(records),
        "skipped": skipped,
        "errors": errors,
        "source_texture_summary": {
            "materials_with_images": sum(
                record["source_texture_audit"]["image_files"] > 0
                for record in records
            ),
            "image_files": sum(
                record["source_texture_audit"]["image_files"]
                for record in records
            ),
            "filename_references": sum(
                record["source_texture_audit"]["references"]
                for record in records
            ),
            "repaired_references": sum(
                len(record["source_texture_audit"]["repaired"])
                for record in records
            ),
            "missing_references": sum(
                len(record["source_texture_audit"]["missing"])
                for record in records
            ),
        },
        "materials": records,
    }
    (output_dir / "catalog_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    write_usd_index(output_dir / "all_materials.usda", records, output_dir)
    print(
        f"Finished: {len(records)} materials, {len(errors)} errors. "
        f"USD index: {output_dir / 'all_materials.usda'}",
        flush=True,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
