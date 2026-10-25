#!/usr/bin/env python3
"""Offline NetSurf 3.11 build. No host package discovery for target objects."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zlib as host_zlib
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SDK = ROOT / "build/tools/ciuki-sdk"
EXTERNAL = ROOT / "build/external"
LIBRARIES = ("libwapcaplet", "libparserutils", "libcss", "libhubbub", "libdom",
             "libnsutils", "libutf8proc", "libnslog", "libnspsl", "libnsgif", "libnsbmp", "libsvgtiny", "libnsfb")
# fx-24: the content handlers of upstream NetSurf's framebuffer front end
# (frontends/framebuffer/Makefile.defaults: JPEG YES, PNG/GIF/BMP/WEBP/NSSVG
# AUTO, i.e. enabled when the library is present). JPEG XL (libjxl, C++),
# RISC OS sprites and video stay off; see docs/design/netsurf-port-plan.md.
FEATURES = {key: "NO" for key in ("CURL", "OPENSSL", "JPEGXL",
                                  "ROSPRITE", "HARU_PDF", "VIDEO", "FS_BACKING_STORE")}
FEATURES.update({key: "YES" for key in ("DUKTAPE", "GIF", "BMP", "PNG", "JPEG", "WEBP", "NSSVG",
                                        "UTF8PROC", "NSLOG", "NSPSL", "LIBICONV_PLUG")})
# Pinned archives (config/netsurf.json), verified on every run.
PINNED = ("archive", "zlib", "libpng", "libjpeg", "expat", "libwebp")
EPOCH = "1703721600"  # release date, independent of the checkout and build clock


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def hashes(folder):
    return {str(p.relative_to(folder)): digest(p) for p in sorted(folder.rglob("*")) if p.is_file() and "__pycache__" not in p.parts}


def run(args, cwd=None, env=None):
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def sources():
    """Verify even cache hits; extract regular files only, never archive links."""
    pins = json.loads((ROOT / "config/netsurf.json").read_text())
    result = {}
    for key in PINNED:
        pin = pins[key]
        archive = ROOT / pin["file"]
        if archive.stat().st_size != pin["size"] or digest(archive) != pin["sha256"]:
            raise ValueError(f"changed pinned archive: {archive}; no download or fallback")
        with tarfile.open(archive) as stream:
            members = stream.getmembers()
            top = PurePosixPath(members[0].name).parts[0]
            for member in members:
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts or path.parts[0] != top:
                    raise ValueError("unsafe archive path")
                if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
                    raise ValueError("special archive member")
            folder = EXTERNAL / top
            expected = {m.name[len(top) + 1:]: hashlib.sha256(stream.extractfile(m).read()).hexdigest()
                        for m in members if m.isfile()}
            if not folder.exists() or hashes(folder) != expected:
                if folder.exists():
                    shutil.rmtree(folder)
                EXTERNAL.mkdir(parents=True, exist_ok=True)
                stream.extractall(EXTERNAL, members=[m for m in members if m.isfile() or m.isdir()], filter="data")
            if hashes(folder) != expected:
                raise ValueError("extracted source hash mismatch")
            result[key] = folder
    return result


def environment():
    env = dict(os.environ)
    for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH",
                "CFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS", "MFLAGS", "CC", "AR", "RANLIB"):
        env.pop(key, None)
    env.update(SOURCE_DATE_EPOCH=EPOCH, LC_ALL="C", TZ="UTC", ZERO_AR_DATE="1",
               PERL_HASH_SEED="0",PERL_PERTURB_KEYS="0",PYTHONHASHSEED="0")
    return env


def tool(name):
    path = Path(shutil.which(name)).resolve()
    return {"path": str(path), "sha256": digest(path),
            "version": subprocess.check_output([str(path), "--version"], text=True).splitlines()[0]}


def inputs(host=False):
    return {"pins": json.loads((ROOT / "config/netsurf.json").read_text()), "features": FEATURES, "ciuki_image_handlers": ["PNG", "JPEG"],
            "internet_options": {name:digest(ROOT / "apps/control" / name) for name in ("internet_options.h","internet_storage.h","profile_lock.h")},
            "stb_image": digest(ROOT / "third_party/stb/stb_image.h"),
            "port": hashes(HERE), "sdk": digest(SDK / "manifest.json"), "epoch": EPOCH, "host": host,
            "tools": {name: tool(name) for name in ("clang", "llvm-ar", "llvm-ranlib", "make", "perl", "pkg-config", "patch", "gawk", "flex", "bison")},
            "desktop": {str(p.relative_to(ROOT)): digest(p) for folder in ("apps/libdeskui", "apps/libprint", "apps/desktop")
                        for p in sorted((ROOT / folder).glob("*.[ch]"))},
            "fonts": hashes(ROOT / "assets/fonts/liberation"),
            "sdk_libraries": {p.name: digest(p) for p in sorted((SDK / "sysroot/lib").glob("*.a"))},
            "icons": digest(ROOT / "assets/icons/upstream/tango-icon-theme-0.8.90.tar.gz"),
            "portrait": digest(ROOT / "assets/brand/ciuki-logo.png"),
            "converters": {name:digest(ROOT / "apps/desktop" / name) for name in ("convert_icons.py","convert_portrait.py")},
            "python":{"version":list(sys.version_info[:3]),"sha256":digest(Path(sys.executable).resolve()),
                      "zlib":host_zlib.ZLIB_RUNTIME_VERSION},
            "bearssl": hashes(ROOT / "third_party/bearssl"),
            "pixman": hashes(ROOT / "third_party/pixman"),
            "cacert": hashes(ROOT / "third_party/cacert"),
            "test_ca": digest(ROOT / "tests/suites/fixtures/f6-browser/ca.pem"),  # f6-18 test-profile anchor
            "host_header":digest(ROOT / "tests/host/desktop/replay_base.h") if host else None}


def flags(folder):
    return ["-O2", "-ffunction-sections", "-fdata-sections", "-Wno-error", "-Wno-unknown-warning-option",
            f"-ffile-prefix-map={ROOT}=.", f"-fdebug-prefix-map={ROOT}=.",
            f"-ffile-prefix-map={folder}=./netsurf-build", f"-fdebug-prefix-map={folder}=./netsurf-build",
            "-fdebug-compilation-dir=."]


def zlib(src, folder, prefix, cc, env, cflags):
    shutil.copytree(src, folder, dirs_exist_ok=True)
    local = dict(env, CC=str(cc), AR="llvm-ar", ARFLAGS="rcsD", RANLIB="llvm-ranlib", CFLAGS=" ".join(cflags))
    run(["sh", "configure", "--static", "--prefix=" + str(prefix)], folder, local)
    run(["make", "-j1", "install"], folder, local)


def bearssl(folder, prefix, cc, env, cflags):
    """Pinned portable C only; no implicit RNG/time providers or SIMD paths."""
    source=ROOT / "third_party/bearssl"
    folder.mkdir(parents=True,exist_ok=True)
    (prefix / "lib").mkdir(parents=True,exist_ok=True)
    shutil.copytree(source / "inc",prefix / "include",dirs_exist_ok=True)
    options=["-DBR_USE_URANDOM=0","-DBR_USE_WIN32_RAND=0","-DBR_RDRAND=0",
             "-DBR_USE_UNIX_TIME=0","-DBR_USE_WIN32_TIME=0","-DBR_AES_X86NI=0",
             "-DBR_SSE2=0","-DBR_POWER8=0","-DBR_INT128=0","-DBR_UMUL128=0"]
    objects=[]
    for path in sorted((source / "src").rglob("*.c")):
        obj=folder / ("_".join(path.relative_to(source / "src").parts)+".o")
        run([cc,*cflags,*options,"-I"+str(source / "inc"),"-I"+str(source / "src"),"-c",path,"-o",obj],env=env)
        objects.append(obj)
    archive=prefix / "lib/libbearssl.a"
    run(["llvm-ar","rcsD",archive,*objects],env=env)
    return archive


def static_archive(name, files, folder, prefix, cc, env, cflags, options=(), includes=()):
    """Compile a fixed upstream source list into prefix/lib/lib<name>.a."""
    folder.mkdir(parents=True, exist_ok=True)
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    objects = []
    for path in files:
        obj = folder / (path.parent.name + "_" + path.stem + ".o")
        if obj in objects:
            raise ValueError("duplicate object name " + str(obj))
        run([cc, *cflags, *options, *["-I" + str(p) for p in includes], "-c", path, "-o", obj], env=env)
        objects.append(obj)
    archive = prefix / "lib" / ("lib" + name + ".a")
    archive.unlink(missing_ok=True)
    run(["llvm-ar", "rcsD", archive, *objects], env=env)
    return archive


def pkgconfig(prefix, name, version, libs, description, requires=None):
    folder = prefix / "lib/pkgconfig"
    folder.mkdir(parents=True, exist_ok=True)
    text = (f"prefix={prefix}\nexec_prefix=${{prefix}}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
            f"Name: {name}\nDescription: {description}\nVersion: {version}\n"
            + (f"Requires.private: {requires}\n" if requires else "")
            + f"Libs: -L${{libdir}} {libs}\nCflags: -I${{includedir}}\n")
    (folder / (name + ".pc")).write_text(text)


def version_of(src):
    return src.name.rsplit("-", 1)[1]


def configure_template(text, values, defines):
    """CMake configure_file subset: @VAR@ and #cmakedefine, nothing left over."""
    for key, value in values.items():
        text = text.replace("@" + key + "@", value)
    lines = []
    for line in text.splitlines():
        if line.startswith("#cmakedefine "):
            name = line.split()[1]
            line = ("#define " + line[len("#cmakedefine "):]) if name in defines else "/* #undef " + name + " */"
        lines.append(line)
    text = "\n".join(lines) + "\n"
    if re.search(r"@[A-Z_]+@", text) or "#cmakedefine" in text:
        raise ValueError("unresolved configure template value")
    return text


def libpng(src, folder, prefix, cc, env, cflags):
    """libpng's own library list (Makefile.am), its prebuilt default
    configuration, no architecture-specific filter code."""
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "scripts/pnglibconf.h.prebuilt", folder / "pnglibconf.h")
    names = ("png", "pngerror", "pngget", "pngmem", "pngpread", "pngread", "pngrio", "pngrtran",
             "pngrutil", "pngset", "pngtrans", "pngwio", "pngwrite", "pngwtran", "pngwutil")
    options = ["-DPNG_ARM_NEON_OPT=0", "-DPNG_MIPS_MSA_OPT=0", "-DPNG_MIPS_MMI_OPT=0", "-DPNG_POWERPC_VSX_OPT=0",
               "-DPNG_INTEL_SSE_OPT=0", "-DPNG_LOONGARCH_LSX_OPT=0", "-DPNG_RISCV_RVV_OPT=0"]
    static_archive("png", [src / (n + ".c") for n in names], folder, prefix, cc, env, cflags, options,
                   [folder, src, prefix / "include"])
    for header in (src / "png.h", src / "pngconf.h", folder / "pnglibconf.h"):
        shutil.copy2(header, prefix / "include" / header.name)
    pkgconfig(prefix, "libpng", version_of(src), "-lpng", "Loads and saves PNG files", "zlib")


def libjpeg(src, folder, prefix, cc, env, cflags, host):
    """libjpeg-turbo's JPEG_SOURCES (CMakeLists.txt, with the release's
    pre-generated 8/12/16-bit wrappers) plus arithmetic coding, as its
    default static build; C only (WITH_SIMD off: no NASM in the recipe)."""
    folder.mkdir(parents=True, exist_ok=True)
    cmake = (src / "CMakeLists.txt").read_text()
    block = cmake.split("set(JPEG_SOURCES ", 1)[1].split(")", 1)[0]
    files = [src / name for name in block.split()] + [src / "src" / n for n in ("jaricom.c", "jcarith.c", "jdarith.c")]
    version = version_of(src)
    major, minor, revision = version.split(".")[:3]
    values = {"JPEG_LIB_VERSION": "62", "VERSION": version,
              "LIBJPEG_TURBO_VERSION_NUMBER": major + minor.zfill(3) + revision.zfill(3),
              "BUILD": time.strftime("%Y%m%d", time.gmtime(int(EPOCH))),
              "HIDDEN": '__attribute__((visibility("hidden")))', "INLINE": "__inline__ __attribute__((always_inline))",
              "THREAD_LOCAL": "", "CMAKE_PROJECT_NAME": "libjpeg-turbo", "SIZE_T": "8" if host else "4",
              "COPYRIGHT_YEAR": cmake.split('set(COPYRIGHT_YEAR "', 1)[1].split('"', 1)[0]}
    defines = {"C_ARITH_CODING_SUPPORTED", "D_ARITH_CODING_SUPPORTED", "HAVE_BUILTIN_CTZL"}
    for name in ("jconfig.h", "jconfigint.h", "jversion.h"):
        (folder / name).write_text(configure_template((src / "src" / (name + ".in")).read_text(), values, defines))
    static_archive("jpeg", files, folder, prefix, cc, env, cflags, [], [folder, src / "src"])
    for header in (src / "src/jpeglib.h", src / "src/jmorecfg.h", src / "src/jerror.h", folder / "jconfig.h"):
        shutil.copy2(header, prefix / "include" / header.name)
    pkgconfig(prefix, "libjpeg", version, "-ljpeg", "A SIMD-accelerated JPEG codec that provides the libjpeg API")


def expat(src, folder, prefix, cc, env, cflags):
    """Expat's library sources (lib/Makefile.am) with the configure defaults
    (XML_DTD, XML_GE, XML_NS, 1024 context bytes); hash salt from
    getentropy (CiukiOS call, f6-16), expat's own fallback if unavailable."""
    folder.mkdir(parents=True, exist_ok=True)
    version = version_of(src)
    (folder / "expat_config.h").write_text("\n".join([
        "#ifndef EXPAT_CONFIG_H", "#define EXPAT_CONFIG_H 1", "#define BYTEORDER 1234", "#define HAVE_GETENTROPY 1",
        "#define HAVE_FCNTL_H 1", "#define HAVE_INTTYPES_H 1", "#define HAVE_STDINT_H 1", "#define HAVE_STDIO_H 1",
        "#define HAVE_STDLIB_H 1", "#define HAVE_STRINGS_H 1", "#define HAVE_STRING_H 1", "#define HAVE_SYS_STAT_H 1",
        "#define HAVE_SYS_TYPES_H 1", "#define HAVE_UNISTD_H 1", '#define PACKAGE "expat"',
        '#define PACKAGE_BUGREPORT "https://github.com/libexpat/libexpat/issues"', '#define PACKAGE_NAME "expat"',
        f'#define PACKAGE_STRING "expat {version}"', '#define PACKAGE_TARNAME "expat"', '#define PACKAGE_URL ""',
        f'#define PACKAGE_VERSION "{version}"', "#define STDC_HEADERS 1", f'#define VERSION "{version}"',
        "#define XML_CONTEXT_BYTES 1024", "#define XML_DTD 1", "#define XML_GE 1", "#define XML_NS 1", "#endif", ""]))
    files = [src / "lib" / (n + ".c") for n in ("xcs", "xmlparse", "xmltok", "xmlrole", "random_getentropy")]
    static_archive("expat", files, folder, prefix, cc, env, cflags, ["-DXML_STATIC"], [folder, src / "lib"])
    for name in ("expat.h", "expat_external.h"):
        shutil.copy2(src / "lib" / name, prefix / "include" / name)
    pkgconfig(prefix, "expat", version, "-lexpat", "expat XML parser")


def libwebp(src, folder, prefix, cc, env, cflags):
    """The decoder library (makefile.unix LIBWEBPDECODER_OBJS); single
    threaded (no WEBP_USE_THREAD), SIMD only where the compiler targets it."""
    folder.mkdir(parents=True, exist_ok=True)
    make = (src / "makefile.unix").read_text()
    files = []
    for group in ("DEC_OBJS", "DSP_DEC_OBJS", "UTILS_DEC_OBJS"):
        block = make.split("\n" + group + " = \\\n", 1)[1].split("\n\n", 1)[0]
        files += [src / name.rstrip("\\").strip().replace(".o", ".c") for name in block.split("\n") if name.strip(" \\")]
    static_archive("webp", files, folder, prefix, cc, env, cflags,
                   ["-include", HERE / "port/webp_memory.h"], [ROOT, src, src / "src"])
    (prefix / "include/webp").mkdir(parents=True, exist_ok=True)
    for name in ("decode.h", "types.h"):
        shutil.copy2(src / "src/webp" / name, prefix / "include/webp" / name)
    pkgconfig(prefix, "libwebp", version_of(src), "-lwebp", "Library for the WebP graphics format")


def codecs(src, folder, prefix, cc, env, cflags, host):
    """fx-24: image libraries for NetSurf's PNG, JPEG, WebP and SVG handlers."""
    libpng(src["libpng"], folder / "png", prefix, cc, env, cflags)
    libjpeg(src["libjpeg"], folder / "jpeg", prefix, cc, env, cflags, host)
    expat(src["expat"], folder / "expat", prefix, cc, env, cflags)
    libwebp(src["libwebp"], folder / "webp", prefix, cc, env, cflags)


TEST_CA=ROOT / "tests/suites/fixtures/f6-browser/ca.pem"   # f6-18 test-profile anchor, gated by the f6:browser selector
def anchors(folder, host_prefix, destination, env, bundle=None, prefix=None):
    if bundle is None:
        bundle=ROOT / "third_party/cacert/CACERT.PEM"
        if digest(bundle)!="a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505":
            raise ValueError("changed pinned CA bundle")
    generator=folder / "ca-generator"
    if not generator.exists():
        run(["clang",*flags(folder),"-I"+str(host_prefix / "include"),HERE / "anchors.c",
             host_prefix / "lib/libbearssl.a","-o",generator],env=env)
    with destination.open("w") as stream:
        subprocess.run([str(generator),str(bundle),*([prefix] if prefix else [])],stdout=stream,check=True,env=env)
    return {"bundle_sha256":digest(bundle),"generator_sha256":digest(generator),
            "anchors_sha256":digest(destination)}


def prepare(folder, src):
    tree = folder / "source"
    if not tree.exists():
        shutil.copytree(src, tree)
        for patch in sorted((HERE / "patches").glob("*.patch")):
            run(["patch", "--batch", "--fuzz=0", "-p1", "-i", patch], tree)
    # Port files are owned here, not edits to retained upstream files.
    shutil.copytree(HERE / "port", tree / "netsurf/frontends/framebuffer/ciuki", dirs_exist_ok=True)
    shutil.copy2(HERE / "port/fetch_ciuki.c",tree / "netsurf/content/fetchers/ciuki.c")
    shutil.copy2(HERE / "port/nsfb_surface.c", tree / "libnsfb/src/surface/ciuki.c")
    shutil.copytree(HERE / "port", tree / "libnsfb/src/surface/ciuki_port", dirs_exist_ok=True)
    # The release omits gperf's output. Generate an equivalent ASCII folded
    # binary lookup from the pinned table; no host gperf/version dependency.
    lookup=tree / "libhubbub/src/treebuilder"
    entries=sorted(line.strip().split(",") for line in (lookup / "element-type.gperf").read_text().split("%%")[1].splitlines() if "," in line)
    text="#include <string.h>\nstatic const struct element_type_map wordlist[] = {\n"
    text+="".join('{"'+name.strip()+'",'+kind.strip()+'},\n' for name,kind in entries)+"};\n"
    text+='''static const struct element_type_map *hubbub_element_type_generated_lookup(const char *s, size_t n)
{
    size_t lo=0, hi=sizeof(wordlist)/sizeof(wordlist[0]);
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2, i=0; int cmp=0;
        const char *key=wordlist[mid].name;
        for (; i<n && key[i]; i++) {
            unsigned c=(unsigned char)s[i]; if (c>='A' && c<='Z') c+='a'-'A';
            cmp=(int)c-(unsigned char)key[i]; if (cmp) break;
        }
        if (!cmp) cmp=i<n ? 1 : key[i] ? -1 : 0;
        if (!cmp) return &wordlist[mid];
        if (cmp<0) hi=mid; else lo=mid+1;
    }
    return NULL;
}
'''
    (lookup / "autogenerated-element-type.c").write_text(text)
    # fx-24: libsvgtiny's named colours, the same lookup without host gperf
    # (src/Makefile: gperf output made static; exact, case-sensitive keys).
    table=(tree / "libsvgtiny/src/colors.gperf").read_text().split("%%")[1]
    colours=sorted(tuple(part.strip() for part in line.split(",",1)) for line in table.splitlines() if "svgtiny_RGB" in line)
    text='#include <string.h>\n#include "svgtiny.h"\n#include "svgtiny_internal.h"\n'
    text+="static const struct svgtiny_named_color svgtiny_color_words[] = {\n"
    text+="".join('{"'+name+'",'+value+'},\n' for name,value in colours)+"};\n"
    text+='''static const struct svgtiny_named_color *svgtiny_color_lookup(const char *s, size_t n)
{
    size_t lo=0, hi=sizeof(svgtiny_color_words)/sizeof(svgtiny_color_words[0]);
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        const char *key=svgtiny_color_words[mid].name;
        int cmp=strncmp(s,key,n);
        if (!cmp) cmp=key[n] ? -1 : 0;
        if (!cmp) return &svgtiny_color_words[mid];
        if (cmp<0) hi=mid; else lo=mid+1;
    }
    return NULL;
}
'''
    (tree / "libsvgtiny/src/autogenerated_colors.c").write_text(text)
    generate_images(tree)
    return tree


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result


def generate_images(tree):
    """Replace release chrome art with deterministic approved project assets."""
    icons=module("netsurf_icons",ROOT / "apps/desktop/convert_icons.py")
    portrait=module("netsurf_portrait",ROOT / "apps/desktop/convert_portrait.py")
    icon_sources=icons.store_sources()
    mapping={"left_arrow":"go-previous","right_arrow":"go-next","reload":"view-refresh",
             "stop_image":"process-stop","history_image":"document-open",
             "scrolll":"go-previous","scrollr":"go-next","scrollu":"go-up","scrolld":"go-down",
             "osk_image":"input-keyboard","pointer_image":"go-next","hand_image":"go-next",
             "caret_image":"format-text-italic","menu_image":"document-properties","progress_image":"view-refresh","move_image":"view-fullscreen"}
    names=list(mapping)
    mapping.update({name+"_g":mapping[name] for name in ("left_arrow","right_arrow","reload","stop_image","history_image")})
    text='#include <stdint.h>\n#include <stdbool.h>\n#include <libnsfb.h>\n#include "framebuffer/gui.h"\n#include "framebuffer/fbtk.h"\n'
    for name,source in mapping.items():
        choices=icon_sources[source]["png"]
        size=22 if 22 in choices else min(choices)
        rgba=choices[size][2]
        if size!=22:rgba=icons.area_scale(rgba,size,22)
        text+='static uint8_t '+name+'_data[]={'+','.join(str(b) for b in rgba)+'};\n'
        text+='struct fbtk_bitmap '+name+'={.width=22,.height=22,.pixdata='+name+'_data,.opaque=false};\n'
    # Use the exact selected portrait; nearest-neighbour conversion, no redraw.
    w,h,pixels=portrait.decode((ROOT / "assets/brand/ciuki-logo.png").read_bytes())
    if digest(ROOT / "assets/brand/ciuki-logo.png")!=portrait.SOURCE_SHA256:raise ValueError("Ciuki portrait changed")
    small=bytes(value for y in range(22) for x in range(22) for value in pixels[((y*h//22)*w+x*w//22)*4:((y*h//22)*w+x*w//22)*4+4])
    text+='static uint8_t throbber_data[]={'+','.join(str(b) for b in small)+'};\n'
    for i in range(9):text+='struct fbtk_bitmap throbber'+str(i)+'={.width=22,.height=22,.pixdata=throbber_data,.opaque=false};\n'
    (tree / "netsurf/frontends/framebuffer/ciuki/images.c").write_text(text)


def build(host=False, clean=False, directory=None):
    src = sources()
    folder = Path(directory or EXTERNAL / ("netsurf-host" if host else "netsurf-target")).resolve()
    if folder.parent!=EXTERNAL.resolve() or not folder.name.startswith("netsurf-") or folder in src.values():
        raise ValueError("NetSurf build/source work must remain under build/external")
    expected = inputs(host)
    stamp = folder / "manifest.json"
    stage = folder / "stage"
    if not clean and stamp.exists():
        cached = json.loads(stamp.read_text())
        if (stage / "Programs/netsurf").is_file() and cached.get("inputs") == expected and cached.get("files") == hashes(stage):
            print("[netsurf] verified cached inputs and installed output hashes")
            return cached
    # A cache miss invalidates the working tree too: it is disposable, and
    # generated/edited source must never be stamped as the pinned inputs.
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True, exist_ok=True)
    tree = prepare(folder, src["archive"])
    prefix = folder / "prefix"
    env = environment()
    env["TMPDIR"] = str(folder / "temp")
    Path(env["TMPDIR"]).mkdir(exist_ok=True)
    env["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    cc = shutil.which("clang") if host else SDK / "bin/ciuki-cc"
    native_headers=folder / "native-headers"
    shutil.copytree(SDK / "sysroot/include/ciuki",native_headers / "ciuki",dirs_exist_ok=True)
    adapter_headers=native_headers if host else SDK / "sysroot/include"
    cflags = flags(folder)
    host_header=["-include",str(ROOT / "tests/host/desktop/replay_base.h")] if host else []
    zlib(src["zlib"], folder / "zlib", prefix, cc, env, cflags)
    codecs(src, folder, prefix, cc, env, cflags, host)
    # Pinned host libraries for the release's image/message generators.
    hp = folder / "host-prefix"
    zlib(src["zlib"], folder / "host-zlib", hp, shutil.which("clang"), env, flags(folder))
    bearssl(folder / "host-bearssl",hp,shutil.which("clang"),env,flags(folder))
    if host:
        shutil.copy2(hp / "lib/libbearssl.a",prefix / "lib/libbearssl.a")
        shutil.copytree(hp / "include",prefix / "include",dirs_exist_ok=True)
    else:
        bearssl(folder / "bearssl",prefix,cc,env,cflags)
    (folder / "ca-generator").unlink(missing_ok=True)
    trust=anchors(folder,hp,tree / "netsurf/frontends/framebuffer/ciuki/anchors.h",env)
    trust["test_profile"]=anchors(folder,hp,tree / "netsurf/frontends/framebuffer/ciuki/test_anchors.h",env,TEST_CA,"ciuki_test")
    png = folder / "host-png"
    shutil.copytree(src["libpng"], png, dirs_exist_ok=True)
    run(["sh", "configure", "--disable-shared", "--disable-dependency-tracking", "--prefix=" + str(hp),
         "CC=clang", "CFLAGS=" + " ".join(flags(folder)), "CPPFLAGS=-I" + str(hp / "include"),
         "LDFLAGS=-L" + str(hp / "lib")], png, env)
    run(["make", "-j1", "install"], png, env)
    if host:
        shutil.copy2(hp / "lib/libpng.a",prefix / "lib/libpng.a")
        for header in ("png.h","pngconf.h","pnglibconf.h"):
            shutil.copy2(hp / "include" / header,prefix / "include" / header)
    # The target libpng is libpng() above (fx-24, the library list only); G26's
    # autotools target build also compiled contrib/libtests (feenableexcept).
    common = ["make", "-j1", "CC=" + str(cc), "AR=llvm-ar", "RANLIB=llvm-ranlib", "ARFLAGS=rcsD", "CCACHE=",
              "BUILD=" + subprocess.check_output(["clang", "-dumpmachine"], text=True).strip(),
              "HOST=" + ("x86_64-unknown-linux-gnu" if host else "i686-unknown-elf"),
              "PREFIX=" + str(prefix), "NSSHARED=" + str(tree / "buildsystem"), "PKGCONFIG=pkg-config",
              "PKG_CONFIG=pkg-config", "BUILD_CC=clang", "COMPONENT_TYPE=lib-static", "toolchain=clang",
              "WITH_LIBXML_BINDING=no", "WITH_EXPAT_BINDING=yes", "WITH_HUBBUB_BINDING=yes",
              "NSFB_SDL_AVAILABLE=no", "NSFB_XCB_AVAILABLE=no", "NSFB_VNC_AVAILABLE=no", "NSFB_WLD_AVAILABLE=no"]
    env["CFLAGS"] = " ".join([*cflags, "-DWITHOUT_ICONV_FILTER", "-I" + str(prefix / "include"),
                                "-DCIUKI_NETSURF", "-D__ciuki__", "-I" + str(ROOT), "-I" + str(adapter_headers)])
    if host: common += ["CIUKI_HOST=yes"]
    for name in LIBRARIES:
        library_env=dict(env)
        if host and name=="libnsfb":library_env["CFLAGS"]+=" "+" ".join(host_header)
        run([*common, "install"], tree / name, library_env)
    # nsgenbind 0.9 is included in the same pinned NetSurf release archive.
    # It is a host build tool even for the i686 browser.
    bind_env=dict(env);bind_env.pop("CFLAGS",None)
    run(["make","-j1","install","VARIANT=release","CC=clang","BUILD_CC=clang",
         "PREFIX="+str(hp),"NSSHARED="+str(tree / "buildsystem")],tree / "nsgenbind",bind_env)
    env["PATH"]=str(hp / "bin")+os.pathsep+env.get("PATH",os.defpath)
    browser = tree / "netsurf"
    with (browser / "frontends/framebuffer/Makefile").open("a") as stream:
        stream.write("\nS_FRONTEND += ciuki/js_memory.c ciuki/printing.c\n")
    # Build only the desktop toolkit, without its v1/v2 client transport.
    desk_objects=[]
    for source in [p for p in sorted((ROOT / "apps/libdeskui").glob("*.c")) if p.name!="client.c"] + sorted((ROOT / "apps/libprint").glob("*.c")) + [ROOT / "apps/desktop" / n for n in ("protocol.c", "compositor.c", "theme.c")]:
        # The folder is part of the name: libdeskui/dialog.c and libprint/dialog.c (G31)
        # must not replace each other in the archive.
        obj=folder / ("desk-" + source.parent.name + "-" + source.stem + ".o")
        run([cc,*cflags,*host_header,"-I" + str(adapter_headers),"-I" + str(ROOT),"-c",source,"-o",obj],env=env)
        desk_objects.append(obj)
    run(["llvm-ar","rcsD",prefix / "lib/libdeskui.a",*desk_objects],env=env)
    font_flags="-lfont -lfreetype"
    if host:
        spec=importlib.util.spec_from_file_location("netsurf_freetype",ROOT / "sdk/freetype/build.py")
        ft=importlib.util.module_from_spec(spec);spec.loader.exec_module(ft)
        library=ft.host()
        font_obj=folder / "font.o"
        run([cc,*cflags,"-I" + str(adapter_headers),*ft.includes(),"-c",ROOT / "sdk/libfont/font.c","-o",font_obj],env=env)
        run(["llvm-ar","rcsD",prefix / "lib/libfont.a",font_obj],env=env)
        shutil.copy2(library,prefix / "lib/libfreetype.a")
        font_flags+=" -lpthread"
    config = [f"override NETSURF_USE_{key} := {value}" for key, value in FEATURES.items()]
    # G5: libdeskui widgets reference the system clipboard weakly; -u pulls
    # clipboard.o out of the archive, so the static ELF has no weak undefined
    # symbol (sdk/tests/check_sdk.py).
    config += ["override NETSURF_FB_FRONTEND := " + ("ram" if host else "ciuki"),
               "override NETSURF_FB_FONTLIB := ciuki", "override NETSURF_FB_RESPATH := /system/netsurf",
               "override NETSURF_FB_FONTPATH := /system/fonts", "override MESSAGES_LANGUAGES := en",
               "CFLAGS += " + " ".join([*cflags,*(["-include",str(HERE / "port/host_network.h")] if host else [])]) + " -DCIUKI_NETSURF -D__ciuki__ -I" + str(ROOT) + " -I" + str(adapter_headers) + (" -DCIUKI_HOST" if host else "") + " -DCIUKI_ROOT=\\\"" + str(ROOT) + "\\\"",
               "LDFLAGS += -L" + str(prefix / "lib") + " -Wl,-u,du_system_clipboard_text_set -Wl,-u,du_system_clipboard_text_get -ldeskui -lbearssl -lpng -lz " + font_flags]
    (browser / "Makefile.config").write_text("\n".join(config) + "\n")
    run(["make", "-j1", "TARGET=framebuffer", "HOST=" + ("Linux" if host else "CiukiOS"), "CC=" + str(cc),
         "CCACHE=", "CC_VERSION=17.0.0", "PKG_CONFIG=pkg-config", "BUILD_CC=clang",
         "BUILD_CFLAGS=-O2 -I" + str(hp / "include"), "BUILD_LDFLAGS=-L" + str(hp / "lib"),
         "BUILD_LIBPNG_CFLAGS=-I" + str(hp / "include"), "BUILD_LIBPNG_LDFLAGS=" + str(hp / "lib/libpng.a") + " " + str(hp / "lib/libz.a") + " -lm"], browser, env)
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "Programs").mkdir(parents=True)
    shutil.copy2(browser / "nsfb", stage / "Programs/netsurf")
    resource = stage / "system/netsurf"
    resource.mkdir(parents=True)
    for name in ("default.css", "quirks.css", "internal.css", "adblock.css"):
        shutil.copy2(browser / "resources" / name, resource / name)
    for name in ("welcome.html", "credits.html", "licence.html"):
        shutil.copy2(browser / "resources/en" / name, resource / name)
    shutil.copy2(browser / "frontends/framebuffer/res/en/Messages", resource / "Messages")
    (resource / "Resources").mkdir()
    for path in list(resource.iterdir()):
        if path.is_file():shutil.copy2(path,resource / "Resources" / path.name)
    notices = stage / "system/licenses"
    notices.mkdir(parents=True)
    for name in ("netsurf", *LIBRARIES, "buildsystem", "nsgenbind"):
        notice = tree / name / ("LICENSE.md" if name == "libutf8proc" else "COPYING")
        shutil.copy2(notice, notices / (name + ".txt"))
    shutil.copy2(tree / "libnspsl/public_suffix_list.dat", notices / "public_suffix_list.dat")
    # The pinned amalgamation embeds both upstream license and authors.
    duktape=(browser / "content/handlers/javascript/duktape/duktape.c").read_text()
    license_start=duktape.index("/* LICENSE.txt */")
    authors=duktape.index("/* AUTHORS.rst */")
    authors_end=duktape.index("*/",authors+len("/* AUTHORS.rst */"))+2
    (notices / "Duktape.txt").write_text(duktape[license_start:authors_end]+"\n")
    shutil.copy2(src["zlib"] / "README", notices / "zlib.txt")
    shutil.copy2(ROOT / "third_party/pixman/COPYING",notices / "pixman.txt")
    shutil.copy2(ROOT / "third_party/bearssl/LICENSE.txt",notices / "BearSSL.txt")
    shutil.copy2(ROOT / "third_party/bearssl/README.ciuki",notices / "BearSSL-SOURCE.txt")
    shutil.copy2(ROOT / "third_party/cacert/MPL-2.0.txt",notices / "CACERT-MPL-2.0.txt")
    shutil.copy2(ROOT / "third_party/cacert/README.md",notices / "CACERT-SOURCE.txt")
    shutil.copy2(ROOT / "third_party/cacert/CACERT.PEM",resource / "CACERT.PEM")
    (notices / "netsurf-trust.json").write_text(json.dumps(trust,indent=2,sort_keys=True)+"\n")
    shutil.copy2(src["libpng"] / "LICENSE", notices / "libpng-host.txt")
    shutil.copy2(ROOT / "third_party/stb/LICENSE", notices / "stb_image.txt")
    # fx-24 image libraries linked into the browser.
    shutil.copy2(src["libpng"] / "LICENSE", notices / "libpng.txt")
    shutil.copy2(src["libjpeg"] / "LICENSE.md", notices / "libjpeg-turbo.txt")
    shutil.copy2(src["libjpeg"] / "README.ijg", notices / "libjpeg-turbo-README.ijg.txt")
    shutil.copy2(src["expat"] / "COPYING", notices / "expat.txt")
    shutil.copy2(src["libwebp"] / "COPYING", notices / "libwebp.txt")
    shutil.copy2(src["libwebp"] / "PATENTS", notices / "libwebp-PATENTS.txt")
    for name in ("COPYING.NEWLIB","SDK-MIT.txt","FreeType-FTL.txt","FreeType-LICENSE.txt","FreeType-NOTICE.txt"):
        shutil.copy2(SDK / "licenses" / name,notices / name)
    shutil.copy2(ROOT / "assets/fonts/liberation/LICENSE",notices / "Liberation.txt")
    shutil.copy2(ROOT / "assets/icons/upstream/COPYING",notices / "Tango.txt")
    shutil.copy2(ROOT / "assets/icons/upstream/AUTHORS",notices / "Tango-AUTHORS.txt")
    shutil.copytree(HERE / "patches",notices / "netsurf-patches")
    (notices / "NetSurf-SOURCE.txt").write_text("NetSurf 3.11, GPL-2.0-only.\nSource: "+expected["pins"]["archive"]["url"]+"\nSHA-256: "+expected["pins"]["archive"]["sha256"]+"\nCiuki changes: sdk/netsurf; applied patches retained in netsurf-patches.\nCiuki portrait is a separate CiukiOS project asset; Tango is Public Domain.\n")
    # JavaScript on, as chrome.c ciuki_chrome_defaults (booleans are 0/1:
    # "true" was never read as true; netsurf-port-plan.md).
    (resource / "Choices").write_text("enable_javascript:1\nmemory_cache_size:2097152\ndisc_cache_size:0\nmax_fetchers:8\n")
    # Chrome uses the system Tango store and approved portrait conversion.
    shutil.copy2(ROOT / "assets/brand/ciuki-logo.png",resource / "ciuki-logo.png")
    elf=None
    if not host:
        checker=module("netsurf_sdk_checker",ROOT / "sdk/tests/check_sdk.py")
        elf=checker.inspect(stage / "Programs/netsurf")
        elf["size_output"]=elf["size_output"].replace(str(stage / "Programs/netsurf"),"/Programs/netsurf")
        symbols=subprocess.check_output(["llvm-nm","-u",stage / "Programs/netsurf"],text=True)
        if symbols.strip():raise ValueError("unresolved NetSurf target import")
        table=subprocess.check_output(["llvm-readelf","-s",stage / "Programs/netsurf"],text=True)
        if "ciuki_register_surface" not in table:raise ValueError("libnsfb constructor discarded")
    (notices / "netsurf-inventory.json").write_text(json.dumps({"sources":expected["pins"],"libraries":LIBRARIES,
        "features":FEATURES,"ciuki_image_handlers":["PNG","JPEG"],"font":"SDK libfont/FreeType; Liberation system faces","psl":"MPL-2.0 data, retained public_suffix_list.dat", "elf":elf},indent=2,sort_keys=True)+"\n")
    generated=[tree / "libhubbub/src/treebuilder/autogenerated-element-type.c",
               browser / "frontends/framebuffer/ciuki/images.c",browser / "frontends/framebuffer/ciuki/anchors.h",
               browser / "frontends/framebuffer/ciuki/test_anchors.h",resource / "Messages"]
    record = {"inputs": expected, "files": hashes(stage), "host": host,"elf":elf,
              "generated":{str(p.relative_to(folder)):digest(p) for p in generated}}
    stamp.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return record


def validate_payloads():
    sources()  # Check pins and extracted originals even on cache hits.
    folder=EXTERNAL / "netsurf-target"
    record=json.loads((folder / "manifest.json").read_text())
    if record.get("inputs")!=inputs() or record.get("files")!=hashes(folder / "stage"):
        raise ValueError("missing/stale NetSurf payloads; run make apps")
    return record


def payloads():
    record=validate_payloads()
    stage=EXTERNAL / "netsurf-target/stage"
    return {"/"+name:stage / name for name in record["files"]},record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", action="store_true")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--directory", type=Path)
    args = parser.parse_args()
    build(args.host, args.clean, args.directory)
