#!/bin/bash
# TRACE ZERO MAP FORGE - build an offline map pack (.vmap) on a Mac.
#
#   Forge/forge.sh hungary                       # a Geofabrik region id
#   Forge/forge.sh europe/germany/berlin --out ~/Desktop
#   Forge/forge.sh --file my-area.osm.pbf --id my-area --name "My area"
#   Forge/forge.sh --list hung                   # find region ids
#
# Options:
#   --out DIR          where to write <id>.vmap (default: current folder)
#   --name NAME        display name in the app (default: Geofabrik name)
#   --profiles LIST    routing profiles, default car,bike,foot
#   --no-tiles         skip the map (routing and search only)
#   --no-search        skip the search index
#   --fresh            download the region again even if cached
#   --progress         machine-readable progress (used by the Mac app)
#
# Everything is downloaded once into ~/Library/Caches/TraceZeroForge:
# the OSM extract, Planetiler, a Java runtime if none is installed, and
# Planetiler's ocean/lake/natural-earth sources (~1.5 GB, shared by all
# regions). After that only new extracts are downloaded.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="${TRACEZERO_CACHE:-$HOME/Library/Caches/TraceZeroForge}"
PLANETILER_VERSION="0.10.2"
PLANETILER_URL="https://github.com/onthegomap/planetiler/releases/download/v${PLANETILER_VERSION}/planetiler.jar"
GEOFABRIK_INDEX="https://download.geofabrik.de/index-v1-nogeom.json"

REGION=""; FILE=""; ID=""; NAME=""; OUT="$PWD"; PROFILES="car,bike,foot"
TILES=1; SEARCH=1; FRESH=0; PROGRESS=0; LIST=""

die() { echo "error: $*" >&2; [ "$PROGRESS" = 1 ] && echo "ERROR $*"; exit 1; }
step() { echo "==> [$1/$STEPS] $2" >&2; [ "$PROGRESS" = 1 ] && echo "FORGE $1 $STEPS $2"; return 0; }

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --id) ID="$2"; shift 2 ;;
    --file) FILE="$2"; shift 2 ;;
    --profiles) PROFILES="$2"; shift 2 ;;
    --no-tiles) TILES=0; shift ;;
    --no-search) SEARCH=0; shift ;;
    --fresh) FRESH=1; shift ;;
    --progress) PROGRESS=1; shift ;;
    --list) LIST="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) REGION="$1"; shift ;;
  esac
done

mkdir -p "$CACHE"
PY="$(command -v python3 || true)"
[ -n "$PY" ] || die "python3 not found (install the Xcode command line tools: xcode-select --install)"

geofabrik_index() {
  local idx="$CACHE/geofabrik-index.json"
  if [ ! -s "$idx" ] || [ -n "$(find "$idx" -mtime +1 2>/dev/null)" ]; then
    curl -fsSL --retry 3 -o "$idx.part" "$GEOFABRIK_INDEX" || die "cannot download the Geofabrik region list"
    mv "$idx.part" "$idx"
  fi
  echo "$idx"
}

if [ -n "$LIST" ] || { [ -z "$REGION" ] && [ -z "$FILE" ]; }; then
  idx="$(geofabrik_index)"
  "$PY" -I - "$idx" "$LIST" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
q = sys.argv[2].lower()
for f in sorted(d["features"], key=lambda f: f["properties"]["id"]):
    p = f["properties"]
    if "pbf" not in p.get("urls", {}): continue
    if q and q not in p["id"].lower() and q not in p["name"].lower(): continue
    print(f'{p["id"]:<40} {p["name"]}')
EOF
  exit 0
fi

STEPS=4; [ "$TILES" = 1 ] || STEPS=3

# ------------------------------------------------------------- 1. extract
step 1 "Getting OpenStreetMap data"
SOURCE_URL=""
if [ -n "$FILE" ]; then
  [ -f "$FILE" ] || die "$FILE not found"
  PBF="$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")"
  [ -n "$ID" ] || ID="$(basename "$FILE" | sed -E 's/(-latest)?\.osm\.pbf$//')"
else
  idx="$(geofabrik_index)"
  info="$("$PY" -I - "$idx" "$REGION" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
r = sys.argv[2].strip("/").split("/")[-1]
for f in d["features"]:
    p = f["properties"]
    if p["id"] == r and "pbf" in p.get("urls", {}):
        print(p["urls"]["pbf"]); print(p["name"]); break
EOF
)"
  [ -n "$info" ] || die "unknown region '$REGION' (try: $0 --list $REGION)"
  SOURCE_URL="$(echo "$info" | sed -n 1p)"
  [ -n "$NAME" ] || NAME="$(echo "$info" | sed -n 2p)"
  [ -n "$ID" ] || ID="$(basename "$SOURCE_URL" | sed -E 's/-latest\.osm\.pbf$//')"
  PBF="$CACHE/extracts/$ID.osm.pbf"
  mkdir -p "$CACHE/extracts"
  if [ "$FRESH" = 1 ] || [ ! -s "$PBF" ] || [ -n "$(find "$PBF" -mtime +7 2>/dev/null)" ]; then
    echo "    downloading $SOURCE_URL" >&2
    curl -fL --retry 3 -C - -o "$PBF.part" "$SOURCE_URL" || die "download failed"
    if curl -fsSL -o "$PBF.md5" "$SOURCE_URL.md5" 2>/dev/null; then
      want="$(cut -d' ' -f1 < "$PBF.md5")"
      got="$(md5 -q "$PBF.part" 2>/dev/null || md5sum "$PBF.part" | cut -d' ' -f1)"
      [ "$want" = "$got" ] || { rm -f "$PBF.part"; die "checksum mismatch, please try again"; }
    fi
    mv "$PBF.part" "$PBF"
  else
    echo "    using cached $PBF (less than a week old; --fresh to update)" >&2
  fi
fi
[ -n "$NAME" ] || NAME="$ID"
PACK="$OUT/$ID.vmap"
mkdir -p "$PACK"

# ------------------------------------------------------------- 2. tiles
if [ "$TILES" = 1 ]; then
  step 2 "Drawing the map (Planetiler)"
  JAVA=""
  if command -v java >/dev/null && java -version 2>&1 | grep -qE 'version "(2[1-9]|[3-9][0-9])'; then
    JAVA="$(command -v java)"
  else
    JDK="$(find "$CACHE/jdk" -path '*/Contents/Home/bin/java' -type f 2>/dev/null | head -1 || true)"
    if [ -z "$JDK" ]; then
      arch="$(uname -m)"; [ "$arch" = "arm64" ] && arch=aarch64 || arch=x64
      echo "    downloading a Java 21 runtime (Eclipse Temurin)" >&2
      mkdir -p "$CACHE/jdk"
      curl -fL --retry 3 -o "$CACHE/jdk/jdk.tar.gz" \
        "https://api.adoptium.net/v3/binary/latest/21/ga/mac/$arch/jdk/hotspot/normal/eclipse" || die "Java download failed"
      tar xzf "$CACHE/jdk/jdk.tar.gz" -C "$CACHE/jdk" && rm "$CACHE/jdk/jdk.tar.gz"
      JDK="$(find "$CACHE/jdk" -path '*/Contents/Home/bin/java' -type f | head -1)"
    fi
    JAVA="$JDK"
  fi
  [ -x "$JAVA" ] || die "no Java 21+ runtime available"
  JAR="$CACHE/planetiler-$PLANETILER_VERSION.jar"
  if [ ! -s "$JAR" ]; then
    echo "    downloading Planetiler $PLANETILER_VERSION" >&2
    curl -fL --retry 3 -o "$JAR.part" "$PLANETILER_URL" || die "Planetiler download failed"
    mv "$JAR.part" "$JAR"
  fi
  mem_gb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 8589934592) / 1073741824 ))
  heap=$(( mem_gb / 2 )); [ "$heap" -lt 2 ] && heap=2
  # Planetiler keeps its downloads (oceans, lakes, natural earth) in ./data,
  # so it runs inside the cache folder where they are reused.
  (cd "$CACHE" && "$JAVA" "-Xmx${heap}g" -jar "$JAR" --osm-path="$PBF" --output="$PACK/map.pmtiles.part.pmtiles" \
     --download --force --languages=en --nodemap-type=sparsearray --storage=mmap 1>&2) || die "Planetiler failed"
  mv "$PACK/map.pmtiles.part.pmtiles" "$PACK/map.pmtiles"
fi

# ------------------------------------------------------------- 3. routing + search
s=3; [ "$TILES" = 1 ] || s=2
step $s "Building routing and search"
TZ="$HERE/tzforge"
if [ ! -x "$TZ" ]; then
  if [ -f "$HERE/Makefile" ]; then make -s -C "$HERE" tzforge >&2 || die "could not compile tzforge (is Xcode installed?)"; fi
fi
[ -x "$TZ" ] || die "tzforge not found next to forge.sh"
args=(build "$PBF" "$PACK" --name "$NAME" --id "$ID" --profiles "$PROFILES")
[ -n "$SOURCE_URL" ] && args+=(--source "$SOURCE_URL")
[ "$SEARCH" = 1 ] || args+=(--no-search)
[ "$PROGRESS" = 1 ] && args+=(--progress)
"$TZ" "${args[@]}" || die "tzforge failed"

# ------------------------------------------------------------- 4. check
step $((s + 1)) "Checking the pack"
"$TZ" verify "$PACK" --pairs 50 >&2 || die "the routing check failed; please report this"
du -sh "$PACK" >&2
echo "==> $PACK is ready. Copy it to the iPhone with Finder → iPhone → Files → TRACE//ZERO." >&2
[ "$PROGRESS" = 1 ] && echo "PACK $PACK"
exit 0
