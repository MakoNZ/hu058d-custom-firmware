#!/usr/bin/env bash
set -euo pipefail

# Load project-local credentials/config if present.
# .env.hu058d is preferred so this helper does not collide with unrelated
# project tooling; .env is accepted as a fallback. Both files are trusted shell
# syntax and must never be committed to Git.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${HU058D_ENV_FILE:-$REPO_ROOT/.env.hu058d}"
if [[ ! -f "$ENV_FILE" && -z "${HU058D_ENV_FILE:-}" && -f "$REPO_ROOT/.env" ]]; then
  ENV_FILE="$REPO_ROOT/.env"
fi
if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

# HU-058D analysis data exporter
#
# Collects:
#   - exhaustive Gladys history for all HU058D features (or an override list)
#   - the clock's current 48-sample NTP history buffer
#   - a current clock status snapshot
#   - CSV conversions plus untouched JSON
#   - a tar.gz bundle convenient for later analysis/sharing
#
# Requirements: bash, curl, jq, tar

GLADYS_URL="${GLADYS_URL:-http://homepi.lan:8083}"
CLOCK_URL="${CLOCK_URL:-http://192.168.1.40}"
DEVICE_NAME="${DEVICE_NAME:-HU058D}"
OUTPUT_ROOT="${OUTPUT_ROOT:-./analysis-data}"
GLADYS_TOKEN="${GLADYS_TOKEN:-}"
GLADYS_EMAIL="${GLADYS_EMAIL:-}"
GLADYS_PASSWORD="${GLADYS_PASSWORD:-}"
GLADYS_FEATURES="${GLADYS_FEATURES:-}"
MAX_STATES="${MAX_STATES:-1000000000000}"

# Conservative default: one week. At a 60-second telemetry interval this is
# only ~10,080 states per feature, so Gladys is not asked to ingest the known
# universe merely because we got enthusiastic with a shell script.
INTERVAL_MINUTES=$((7 * 24 * 60))
MAKE_ARCHIVE=1

usage() {
  cat <<'USAGE'
Usage:
  export-hu058d-data.sh [options]

Options:
  --days N             Export the last N days from Gladys (default: 7)
  --hours N            Export the last N hours from Gladys
  --minutes N          Export the last N minutes from Gladys
  --output DIR         Root output directory (default: ./analysis-data)
  --gladys URL         Gladys base URL
  --clock URL          Clock base URL
  --device NAME        Gladys device name used for feature discovery
  --selectors LIST     Comma-separated Gladys feature selectors; skips discovery
  --no-archive         Do not create a .tar.gz bundle
  -h, --help           Show this help

Environment overrides:
  GLADYS_URL            Default: http://homepi.lan:8083
  CLOCK_URL             Default: http://192.168.1.40
  DEVICE_NAME           Default: HU058D
  OUTPUT_ROOT           Default: ./analysis-data
  GLADYS_TOKEN          Optional 24-hour Gladys bearer token
  GLADYS_EMAIL          Gladys login email; used with GLADYS_PASSWORD to obtain a fresh token
  GLADYS_PASSWORD       Gladys login password; used with GLADYS_EMAIL
  HU058D_ENV_FILE       Override credentials file (default: .env.hu058d, then .env)
  GLADYS_FEATURES       Optional comma-separated selector override
  MAX_STATES            Default: 1000000000000 (requests unsampled/exhaustive data)

Examples:
  ./tools/export-hu058d-data.sh
  ./tools/export-hu058d-data.sh --days 2
  ./tools/export-hu058d-data.sh --hours 12
  GLADYS_TOKEN='...' ./tools/export-hu058d-data.sh --days 30

Recommended persistent setup: create .env.hu058d beside README.md containing:
  GLADYS_EMAIL='your-gladys-login@example.com'
  GLADYS_PASSWORD='your-password'
Then chmod 600 .env.hu058d and keep it out of Git.
USAGE
}

is_pos_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

while (($#)); do
  case "$1" in
    --days)
      [[ $# -ge 2 ]] || { echo "Missing value for --days" >&2; exit 2; }
      is_pos_int "$2" || { echo "--days requires a positive integer" >&2; exit 2; }
      INTERVAL_MINUTES=$((10#$2 * 24 * 60)); shift 2 ;;
    --hours)
      [[ $# -ge 2 ]] || { echo "Missing value for --hours" >&2; exit 2; }
      is_pos_int "$2" || { echo "--hours requires a positive integer" >&2; exit 2; }
      INTERVAL_MINUTES=$((10#$2 * 60)); shift 2 ;;
    --minutes)
      [[ $# -ge 2 ]] || { echo "Missing value for --minutes" >&2; exit 2; }
      is_pos_int "$2" || { echo "--minutes requires a positive integer" >&2; exit 2; }
      INTERVAL_MINUTES=$((10#$2)); shift 2 ;;
    --output)
      [[ $# -ge 2 ]] || { echo "Missing value for --output" >&2; exit 2; }
      OUTPUT_ROOT="$2"; shift 2 ;;
    --gladys)
      [[ $# -ge 2 ]] || { echo "Missing value for --gladys" >&2; exit 2; }
      GLADYS_URL="${2%/}"; shift 2 ;;
    --clock)
      [[ $# -ge 2 ]] || { echo "Missing value for --clock" >&2; exit 2; }
      CLOCK_URL="${2%/}"; shift 2 ;;
    --device)
      [[ $# -ge 2 ]] || { echo "Missing value for --device" >&2; exit 2; }
      DEVICE_NAME="$2"; shift 2 ;;
    --selectors)
      [[ $# -ge 2 ]] || { echo "Missing value for --selectors" >&2; exit 2; }
      GLADYS_FEATURES="$2"; shift 2 ;;
    --no-archive)
      MAKE_ARCHIVE=0; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2 ;;
  esac
done

for cmd in curl jq tar; do
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Required command not found: $cmd" >&2
    exit 1
  }
done

GLADYS_URL="${GLADYS_URL%/}"
CLOCK_URL="${CLOCK_URL%/}"

stamp="$(date '+%Y%m%d-%H%M%S')"
outdir="${OUTPUT_ROOT%/}/${stamp}"
mkdir -p "$outdir/gladys"

curl_common=(-fsS --connect-timeout 5 --max-time 120)

# Gladys local REST API access tokens expire after 24 hours. If a token was not
# supplied explicitly, log in at runtime so the exporter remains maintenance-free.
if [[ -z "$GLADYS_TOKEN" ]]; then
  if [[ -n "$GLADYS_EMAIL" && -n "$GLADYS_PASSWORD" ]]; then
    echo "Authenticating to Gladys..."
    login_payload="$(jq -nc \
      --arg email "$GLADYS_EMAIL" \
      --arg password "$GLADYS_PASSWORD" \
      '{email:$email,password:$password}')"

    if ! login_response="$(curl "${curl_common[@]}" \
      -X POST \
      -H 'Content-Type: application/json;charset=UTF-8' \
      --data "$login_payload" \
      "$GLADYS_URL/api/v1/login")"; then
      echo "Gladys login failed. Check GLADYS_EMAIL / GLADYS_PASSWORD and GLADYS_URL." >&2
      exit 1
    fi

    GLADYS_TOKEN="$(jq -r '.access_token // empty' <<<"$login_response")"
    if [[ -z "$GLADYS_TOKEN" ]]; then
      echo "Gladys login succeeded as HTTP but returned no access_token." >&2
      jq . <<<"$login_response" >&2 || true
      exit 1
    fi
  else
    cat >&2 <<'EOF_AUTH'
No Gladys authentication configured.
Set GLADYS_TOKEN, or preferably put GLADYS_EMAIL and GLADYS_PASSWORD in
.env.hu058d so the exporter can obtain a fresh 24-hour access token each run.
EOF_AUTH
    exit 1
  fi
fi

gladys_auth=(-H "Authorization: Bearer $GLADYS_TOKEN")

cleanup_on_error() {
  rc=$?
  if (( rc != 0 )); then
    echo >&2
    echo "Export failed. Partial output, if any, is in: $outdir" >&2
  fi
  exit "$rc"
}
trap cleanup_on_error EXIT

echo "HU-058D data export"
echo "  Gladys: $GLADYS_URL"
echo "  Clock:  $CLOCK_URL"
echo "  Window: ${INTERVAL_MINUTES} minutes"
echo "  Output: $outdir"
if [[ -f "$ENV_FILE" ]]; then
  echo "  Config: $ENV_FILE"
fi
echo

# ---------------------------------------------------------------------------
# Discover Gladys feature selectors unless explicitly supplied.
# ---------------------------------------------------------------------------
if [[ -z "$GLADYS_FEATURES" ]]; then
  echo "Discovering Gladys features for device '$DEVICE_NAME'..."
  devices_json="$outdir/gladys/devices.json"

  if curl "${curl_common[@]}" "${gladys_auth[@]}" \
      "$GLADYS_URL/api/v1/device" -o "$devices_json"; then

    # Current Gladys normally returns an array. The alternate .devices path is
    # accepted as a harmless fallback in case the envelope changes.
    GLADYS_FEATURES="$(jq -r --arg device "$DEVICE_NAME" '
      def devs:
        if type == "array" then .
        elif type == "object" and (.devices? | type) == "array" then .devices
        else []
        end;
      [ devs[]
        | select(.name == $device)
        | (.features // .device_features // [])[]?
        | .selector // empty
      ]
      | unique
      | join(",")
    ' "$devices_json")"
  fi
fi

# Known-good fallback selectors from this project. This means the exporter is
# still useful if /api/v1/device is authenticated or feature discovery changes.
if [[ -z "$GLADYS_FEATURES" ]]; then
  echo "Feature discovery produced no selectors; using known HU058D selectors." >&2
  GLADYS_FEATURES="mqtt-hu058d-clock-drift,mqtt-hu058d-clock-free-heap"
fi

echo "Gladys selectors:"
tr ',' '\n' <<<"$GLADYS_FEATURES" | sed 's/^/  - /'
echo

# ---------------------------------------------------------------------------
# Gladys history.
# max_states is deliberately enormous: Gladys uses that ceiling to decide when
# to sample/aggregate. For this small device/window we want every stored state.
# ---------------------------------------------------------------------------
echo "Fetching exhaustive Gladys history..."
gladys_raw="$outdir/gladys/history.json"

curl "${curl_common[@]}" "${gladys_auth[@]}" -G \
  --data-urlencode "interval=$INTERVAL_MINUTES" \
  --data-urlencode "max_states=$MAX_STATES" \
  --data-urlencode "device_features=$GLADYS_FEATURES" \
  "$GLADYS_URL/api/v1/device_feature/aggregated_states" \
  -o "$gladys_raw"

jq -e 'type == "array"' "$gladys_raw" >/dev/null || {
  echo "Gladys history response was not the expected JSON array." >&2
  jq . "$gladys_raw" >&2 || true
  exit 1
}

# One long-form CSV makes analysis in Python/R/spreadsheets pleasantly boring.
gladys_csv="$outdir/gladys/history-long.csv"
jq -r '
  ["device","feature","unit","created_at","value","min_value","max_value","sum_value","count_value"],
  (.[] as $series
    | $series.values[]?
    | [
        ($series.device.name // ""),
        ($series.deviceFeature.name // ""),
        ($series.deviceFeature.unit // ""),
        .created_at,
        .value,
        .min_value,
        .max_value,
        .sum_value,
        .count_value
      ])
  | @csv
' "$gladys_raw" > "$gladys_csv"

# Also create a separate CSV per feature because humans keep inventing programs
# that prefer wide piles of small files to one sensible long table.
while IFS=$'\t' read -r feature index; do
  [[ -n "$feature" ]] || continue
  safe="$(printf '%s' "$feature" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9._-]+/-/g; s/^-+|-+$//g')"
  [[ -n "$safe" ]] || safe="feature-$index"

  jq -r --argjson idx "$index" '
    ["created_at","value","min_value","max_value","sum_value","count_value"],
    (.[ $idx ].values[]?
      | [.created_at,.value,.min_value,.max_value,.sum_value,.count_value])
    | @csv
  ' "$gladys_raw" > "$outdir/gladys/${safe}.csv"
done < <(jq -r 'to_entries[] | [(.value.deviceFeature.name // "unnamed"), (.key|tostring)] | @tsv' "$gladys_raw")

# ---------------------------------------------------------------------------
# Clock-side state: exact current in-RAM history plus a status snapshot.
# ---------------------------------------------------------------------------
echo "Fetching clock NTP history..."
clock_history="$outdir/clock-ntp-history.json"
curl "${curl_common[@]}" "$CLOCK_URL/api/ntp-history" -o "$clock_history"

jq -e '.samples | type == "array"' "$clock_history" >/dev/null || {
  echo "Clock history response was not the expected JSON object." >&2
  jq . "$clock_history" >&2 || true
  exit 1
}

jq -r '
  ["epoch","time_utc","correction_ms","drift_ppm","drift_valid","interval_s","server_index","server","server_ip"],
  (.samples[]?
    | [
        .epoch,
        (.epoch | strftime("%Y-%m-%dT%H:%M:%SZ")),
        .correction_ms,
        .drift_ppm,
        .drift_valid,
        .interval_s,
        .server_index,
        .server,
        .server_ip
      ])
  | @csv
' "$clock_history" > "$outdir/clock-ntp-history.csv"

echo "Fetching clock status snapshot..."
if ! curl "${curl_common[@]}" "$CLOCK_URL/api/status" -o "$outdir/clock-status.json"; then
  echo "Warning: could not fetch /api/status; continuing with history export." >&2
  rm -f "$outdir/clock-status.json"
fi

# ---------------------------------------------------------------------------
# Manifest and compact summary.
# ---------------------------------------------------------------------------
gladys_series_count="$(jq 'length' "$gladys_raw")"
gladys_state_count="$(jq '[.[].values | length] | add // 0' "$gladys_raw")"
clock_capacity="$(jq -r '.capacity // "?"' "$clock_history")"
clock_count="$(jq -r '.count // (.samples|length)' "$clock_history")"

cat > "$outdir/manifest.txt" <<EOF_MANIFEST
HU-058D analysis export
created_local=$(date '+%Y-%m-%dT%H:%M:%S%:z')
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
gladys_url=$GLADYS_URL
clock_url=$CLOCK_URL
device_name=$DEVICE_NAME
interval_minutes=$INTERVAL_MINUTES
max_states=$MAX_STATES
gladys_feature_selectors=$GLADYS_FEATURES
gladys_series_count=$gladys_series_count
gladys_state_count=$gladys_state_count
clock_history_capacity=$clock_capacity
clock_history_count=$clock_count
EOF_MANIFEST

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  {
    echo "git_commit=$(git rev-parse HEAD 2>/dev/null || true)"
    echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  } >> "$outdir/manifest.txt"
fi

echo
echo "Export complete:"
echo "  Gladys series: $gladys_series_count"
echo "  Gladys states: $gladys_state_count"
echo "  Clock history: $clock_count / $clock_capacity samples"
echo "  Directory:     $outdir"

if (( MAKE_ARCHIVE )); then
  archive="${outdir}.tar.gz"
  tar -C "$(dirname "$outdir")" -czf "$archive" "$(basename "$outdir")"
  echo "  Archive:       $archive"
fi

trap - EXIT
