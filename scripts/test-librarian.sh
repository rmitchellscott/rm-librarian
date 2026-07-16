#!/usr/bin/env bash
set -Eeuo pipefail

XOCHITL_DIR="${XOCHITL_DIR:-/home/root/.local/share/remarkable/xochitl}"
MB_IN="${MB_IN:-/run/xovi-mb}"
MB_OUT="${MB_OUT:-/run/xovi-mb-out}"
MB_TIMEOUT="${MB_TIMEOUT:-10}"
POLL_SECONDS="${POLL_SECONDS:-20}"

PREFIX="librarian-test-$(date +%s)"
WORKDIR="/tmp/${PREFIX}"

ROOT_FOLDER="${PREFIX}-root"
DEST_FOLDER="${PREFIX}-dest"
NESTED_PATH="${ROOT_FOLDER}/projects/2026"
DOC_NAME="${PREFIX}-doc"
DOC_RENAMED="${PREFIX}-doc-renamed"
IMG_NAME="${PREFIX}-img"
NOTE_NAME="${PREFIX}-note"
MANUAL_NAME="${PREFIX}-manual"
TAG_A="${PREFIX}-tag-a"
TAG_B="${PREFIX}-tag-b"

ROOT_UUID=""
DEST_UUID=""
NESTED_UUID=""
NOTE_UUID=""
DOC_UUID=""
IMG_UUID=""
MANUAL_UUID=""

log() {
  printf '[test-librarian] %s\n' "$*"
}

die() {
  printf '[test-librarian] ERROR: %s\n' "$*" >&2
  exit 1
}

on_err() {
  local line="$1"
  die "unexpected failure at line ${line}"
}
trap 'on_err $LINENO' ERR

cleanup() {
  rm -rf "$WORKDIR"

  for id in "$MANUAL_UUID" "$NOTE_UUID" "$DOC_UUID" "$IMG_UUID"; do
    is_uuid "$id" || continue
    send_signal trashEntry "$id" >/dev/null 2>&1 || true
    send_signal deleteEntry "$id" >/dev/null 2>&1 || true
  done

  for path in "$NESTED_PATH" "$DEST_FOLDER" "$ROOT_FOLDER"; do
    send_signal trashEntry "$path" >/dev/null 2>&1 || true
    send_signal deleteEntry "$path" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

is_uuid() {
  [[ "$1" =~ ^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$ ]]
}

require_file() {
  [[ -e "$1" ]] || die "missing required file: $1"
}

send_signal() {
  local signal="$1"
  local params="${2-}"
  local output=""
  local last=""
  local line=""

  printf '>e%s:%s\n' "$signal" "$params" > "$MB_IN"
  if command -v timeout >/dev/null 2>&1; then
    output="$(timeout "${MB_TIMEOUT}" cat "$MB_OUT" || true)"
  else
    output="$(cat "$MB_OUT")"
  fi

  while IFS= read -r line; do
    line="${line%$'\r'}"
    [[ -n "$line" ]] || continue
    last="$line"
  done <<< "$output"

  printf '%s' "$last"
}

assert_not_error() {
  local output="$1"
  if [[ "$output" == ERROR:* ]]; then
    die "broker returned error: $output"
  fi
}

assert_eq() {
  local expected="$1"
  local actual="$2"
  local message="$3"
  [[ "$expected" == "$actual" ]] || die "$message: expected '$expected', got '$actual'"
}

assert_nonempty() {
  local value="$1"
  local message="$2"
  [[ -n "$value" ]] || die "$message"
}

assert_uuid() {
  local value="$1"
  local message="$2"
  is_uuid "$value" || die "$message: $value"
}

assert_file_contains() {
  local file="$1"
  local needle="$2"
  grep -F "$needle" "$file" >/dev/null || die "expected '$needle' in $file"
}

assert_file_missing() {
  [[ ! -e "$1" ]] || die "expected file to be missing: $1"
}

wait_for_lookup() {
  local target="$1"
  local output=""
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    output="$(send_signal lookupEntry "$target")"
    if [[ "$output" != ERROR:* && -n "$output" ]]; then
      printf '%s' "$output"
      return 0
    fi
    sleep 1
  done
  die "timed out waiting for lookupEntry('$target')"
}

try_lookup() {
  local target="$1"
  local output=""
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    output="$(send_signal lookupEntry "$target")"
    if [[ "$output" != ERROR:* && -n "$output" ]]; then
      printf '%s' "$output"
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_for_missing_lookup() {
  local target="$1"
  local output=""
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    output="$(send_signal lookupEntry "$target")"
    if [[ "$output" == ERROR:* ]]; then
      return 0
    fi
    sleep 1
  done
  die "timed out waiting for lookupEntry('$target') to disappear"
}

wait_for_path_value() {
  local file="$1"
  local needle="$2"
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    if grep -F "$needle" "$file" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "timed out waiting for '$needle' in $file"
}

wait_for_json_string_field() {
  local file="$1"
  local key="$2"
  local value="$3"
  local pattern="\"${key}\"[[:space:]]*:[[:space:]]*\"${value}\""
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    if grep -E "$pattern" "$file" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  log "Current contents of $file:"
  sed -n '1,200p' "$file" >&2 || true
  die "timed out waiting for JSON field ${key}=${value} in $file"
}

wait_for_json_bool_field() {
  local file="$1"
  local key="$2"
  local value="$3"
  local pattern="\"${key}\"[[:space:]]*:[[:space:]]*${value}"
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    if grep -E "$pattern" "$file" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "timed out waiting for JSON field ${key}=${value} in $file"
}

wait_for_json_number_field() {
  local file="$1"
  local key="$2"
  local value="$3"
  local pattern="\"${key}\"[[:space:]]*:[[:space:]]*${value}"
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    if grep -E "$pattern" "$file" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "timed out waiting for JSON field ${key}=${value} in $file"
}

wait_for_file() {
  local file="$1"
  local i
  for ((i=0; i<POLL_SECONDS; i++)); do
    [[ -e "$file" ]] && return 0
    sleep 1
  done
  die "timed out waiting for file $file"
}

count_prefixed_entries() {
  local prefix="$1"
  grep -l "\"visibleName\"[[:space:]]*:[[:space:]]*\"${prefix}" "$XOCHITL_DIR"/*.metadata 2>/dev/null | wc -l | tr -d ' '
}

find_metadata_uuid_by_name_parent() {
  local name="$1"
  local parent="$2"
  local file
  for file in "$XOCHITL_DIR"/*.metadata; do
    [[ -e "$file" ]] || continue
    if grep -F "\"visibleName\": \"$name\"" "$file" >/dev/null 2>&1 &&
       grep -F "\"parent\": \"$parent\"" "$file" >/dev/null 2>&1; then
      basename "$file" .metadata
      return 0
    fi
  done
  return 1
}

make_pdf() {
  cat > "$1" <<'EOF'
%PDF-1.1
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>
endobj
trailer
<< /Root 1 0 R >>
%%EOF
EOF
}

make_png() {
  # Embedded copy of sample_white.png as a minimal 1x1 truecolor PNG.
  printf '%b' \
    '\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90\x77\x53\xde'\
    '\x00\x00\x00\x0c\x49\x44\x41\x54\x78\xda\x63\xf8\xff\xff\x3f\x00\x05\xfe\x02\xfe\x33\x12\x95\x14'\
    '\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82' > "$1"
}

make_manual_import() {
  local uuid="$1"
  local name="$2"
  local parent="$3"
  local pdf_path="$XOCHITL_DIR/${uuid}.pdf"
  local meta_path="$XOCHITL_DIR/${uuid}.metadata"
  local content_path="$XOCHITL_DIR/${uuid}.content"

  cp "$WORKDIR/minimal.pdf" "$pdf_path"
  cat > "$meta_path" <<EOF
{
  "createdTime": "$(date +%s%3N)",
  "lastModified": "$(date +%s%3N)",
  "lastOpened": "0",
  "lastOpenedPage": 0,
  "parent": "$parent",
  "pinned": false,
  "type": "DocumentType",
  "visibleName": "$name"
}
EOF
  cat > "$content_path" <<'EOF'
{
  "coverPageNumber": -1,
  "documentMetadata": {},
  "extraMetadata": {},
  "fileType": "pdf",
  "fontName": "",
  "formatVersion": 2,
  "lineHeight": -1,
  "margins": 125,
  "orientation": "portrait",
  "pageCount": 0,
  "pageTags": [],
  "tags": [],
  "textScale": 1,
  "transform": {}
}
EOF
}

log "Using XOCHITL_DIR=$XOCHITL_DIR"
require_file "$MB_IN"
require_file "$MB_OUT"
mkdir -p "$WORKDIR"
make_pdf "$WORKDIR/minimal.pdf"
make_png "$WORKDIR/minimal.png"
cp "$WORKDIR/minimal.pdf" "$WORKDIR/${DOC_NAME}.pdf"
cp "$WORKDIR/minimal.png" "$WORKDIR/${IMG_NAME}.png"

log "Creating top-level folders"
ROOT_UUID="$(send_signal createFolder "$ROOT_FOLDER")"
assert_not_error "$ROOT_UUID"
assert_nonempty "$ROOT_UUID" "root folder UUID missing"
assert_eq "$ROOT_UUID" "$(wait_for_lookup "/$ROOT_FOLDER")" "root folder lookup mismatch"

DEST_UUID="$(send_signal createFolder "$DEST_FOLDER")"
assert_not_error "$DEST_UUID"
assert_nonempty "$DEST_UUID" "dest folder UUID missing"
assert_eq "$DEST_UUID" "$(wait_for_lookup "/$DEST_FOLDER")" "dest folder lookup mismatch"

log "Ensuring nested folder path"
NESTED_UUID="$(send_signal ensureFolder "$NESTED_PATH")"
log "ensureFolder returned: $NESTED_UUID"
assert_not_error "$NESTED_UUID"
assert_nonempty "$NESTED_UUID" "ensureFolder returned empty UUID"
second_nested_uuid="$(send_signal ensureFolder "$NESTED_PATH")"
log "ensureFolder second call returned: $second_nested_uuid"
assert_eq "$NESTED_UUID" "$second_nested_uuid" "ensureFolder not idempotent"
lookup_nested_uuid="$(wait_for_lookup "/$NESTED_PATH")"
log "lookupEntry for nested path returned: $lookup_nested_uuid"
assert_eq "$NESTED_UUID" "$lookup_nested_uuid" "nested folder lookup mismatch"

log "Creating notebook"
NOTE_UUID="$(send_signal createNotebook "$NOTE_NAME,$NESTED_PATH")"
log "createNotebook returned: $NOTE_UUID"
assert_not_error "$NOTE_UUID"
assert_nonempty "$NOTE_UUID" "notebook lookup failed"
NOTE_META="$XOCHITL_DIR/${NOTE_UUID}.metadata"
wait_for_file "$NOTE_META"
wait_for_json_string_field "$NOTE_META" "visibleName" "$NOTE_NAME"
wait_for_json_string_field "$NOTE_META" "parent" "$NESTED_UUID"

log "Importing PDF document"
DOC_UUID="$(send_signal importDocument "$WORKDIR/${DOC_NAME}.pdf,$NESTED_PATH")"
log "importDocument returned: $DOC_UUID"
assert_not_error "$DOC_UUID"
assert_nonempty "$DOC_UUID" "importDocument returned empty UUID"
assert_uuid "$DOC_UUID" "importDocument returned non-UUID"
DOC_META="$XOCHITL_DIR/${DOC_UUID}.metadata"
DOC_CONTENT="$XOCHITL_DIR/${DOC_UUID}.content"
wait_for_file "$DOC_META"
wait_for_file "$DOC_CONTENT"
wait_for_json_string_field "$DOC_META" "parent" "$NESTED_UUID"

log "Importing PNG image"
IMG_UUID="$(send_signal importImage "$WORKDIR/${IMG_NAME}.png,$NESTED_PATH")"
log "importImage returned: $IMG_UUID"
assert_not_error "$IMG_UUID"
assert_nonempty "$IMG_UUID" "importImage returned empty UUID"
assert_uuid "$IMG_UUID" "importImage returned non-UUID"
IMG_META="$XOCHITL_DIR/${IMG_UUID}.metadata"
wait_for_file "$IMG_META"
wait_for_json_string_field "$IMG_META" "parent" "$NESTED_UUID"

log "Renaming imported PDF"
output="$(send_signal renameEntry "$DOC_UUID,${DOC_RENAMED}")"
assert_eq "ok" "$output" "renameEntry failed"
wait_for_json_string_field "$DOC_META" "visibleName" "$DOC_RENAMED"

log "Setting pinned flag"
output="$(send_signal setPinned "$DOC_UUID,true")"
assert_eq "ok" "$output" "setPinned failed"
wait_for_json_bool_field "$DOC_META" "pinned" "true"

log "Setting orientation"
output="$(send_signal setOrientation "$DOC_UUID,landscape")"
assert_eq "ok" "$output" "setOrientation failed"
wait_for_json_string_field "$DOC_CONTENT" "orientation" "landscape"

log "Setting cover page"
output="$(send_signal setCover "$DOC_UUID,-1")"
assert_eq "ok" "$output" "setCover failed"
wait_for_json_number_field "$DOC_CONTENT" "coverPageNumber" "-1"

log "Setting tags"
output="$(send_signal setTags "$DOC_UUID,${TAG_A};${TAG_B}")"
assert_eq "ok" "$output" "setTags failed"
wait_for_path_value "$DOC_CONTENT" "\"${TAG_A}\""
wait_for_path_value "$DOC_CONTENT" "\"${TAG_B}\""

log "Moving renamed document"
output="$(send_signal moveEntry "$DOC_UUID,/$DEST_FOLDER")"
assert_eq "ok" "$output" "moveEntry failed"
assert_eq "$DOC_UUID" "$(wait_for_lookup "/$DEST_FOLDER/$DOC_RENAMED")" "moved document lookup mismatch"
wait_for_json_string_field "$DOC_META" "parent" "$DEST_UUID"

log "Cloning moved document"
before_clone="$(count_prefixed_entries "$PREFIX")"
clone_uuid="$(send_signal cloneEntry "$DOC_UUID,/$DEST_FOLDER")"
assert_not_error "$clone_uuid"
assert_nonempty "$clone_uuid" "cloneEntry returned empty UUID"
for ((i=0; i<POLL_SECONDS; i++)); do
  after_clone="$(count_prefixed_entries "$PREFIX")"
  if [[ "$after_clone" -gt "$before_clone" ]]; then
    break
  fi
  sleep 1
done
[[ "$after_clone" -gt "$before_clone" ]] || die "cloneEntry did not create a new entry"

log "Checking getContentPages on imported PDF"
output="$(send_signal getContentPages "$DOC_UUID")"
assert_not_error "$output"
[[ -z "$output" ]] || die "expected imported PDF to have no content pages, got: $output"

log "Trashing and restoring image"
output="$(send_signal trashEntry "$IMG_UUID")"
assert_eq "ok" "$output" "trashEntry failed"
wait_for_json_string_field "$IMG_META" "parent" "trash"
wait_for_missing_lookup "/$NESTED_PATH/$IMG_NAME"

output="$(send_signal restoreEntry "$IMG_UUID")"
assert_eq "ok" "$output" "restoreEntry failed"
wait_for_json_string_field "$IMG_META" "parent" "$NESTED_UUID"

log "Deleting image permanently"
output="$(send_signal trashEntry "$IMG_UUID")"
assert_eq "ok" "$output" "second trashEntry failed"
wait_for_json_string_field "$IMG_META" "parent" "trash"
output="$(send_signal deleteEntry "$IMG_UUID")"
assert_eq "ok" "$output" "deleteEntry failed"
for ((i=0; i<POLL_SECONDS; i++)); do
  if [[ ! -e "$IMG_META" ]]; then
    break
  fi
  sleep 1
done
assert_file_missing "$IMG_META"
IMG_UUID=""

log "Creating manual on-disk document and rescanning"
MANUAL_UUID="$(cat /proc/sys/kernel/random/uuid)"
make_manual_import "$MANUAL_UUID" "$MANUAL_NAME" "$ROOT_UUID"
output="$(send_signal rescanLibrary "")"
assert_not_error "$output"
[[ "$output" =~ ^[0-9]+$ ]] || die "rescanLibrary did not return a count: $output"
assert_eq "$MANUAL_UUID" "$(wait_for_lookup "/$ROOT_FOLDER/$MANUAL_NAME")" "rescanned manual document lookup mismatch"

log "All requested librarian operations completed successfully"
log "Prefix: $PREFIX"
