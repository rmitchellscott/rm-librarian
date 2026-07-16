# Librarian

[![rm1](https://img.shields.io/badge/rM1-supported-green)](https://remarkable.com/store/remarkable)
[![rm2](https://img.shields.io/badge/rM2-supported-green)](https://remarkable.com/store/remarkable-2)
[![rmpp](https://img.shields.io/badge/rMPP-supported-green)](https://remarkable.com/store/overview/remarkable-paper-pro)
[![rmppmove](https://img.shields.io/badge/rMPPMove-supported-green)](https://remarkable.com/products/remarkable-paper/pro-move)
[![rmppure](https://img.shields.io/badge/rMPPure-supported-green)](https://remarkable.com/products/remarkable-paper/pure)
<img src="assets/rm-librarian.svg" alt="rm-librarian Icon" width="125" align="right">
<p align="justify">

A xovi extension that provides a programmatic interface to xochitl's document library without the need to restart xochitl.

## Dependencies

- [xovi](https://github.com/asivery/rm-xovi-extensions) - Extension framework
    - xovi-message-broker - Required for shell and QML communication

[librm_lines](https://github.com/RedTTGMoss/librm_lines) is statically linked into the binary to parse `.rm` (lines) files for `getContentPages`. It is bundled at build time and requires no separate installation.

## Installation

1. Ensure dependencies are installed
2. Download the `.so` file for your architecture from the [latest release](https://github.com/rmitchellscott/rm-librarian/releases/latest) and place it in `/home/root/xovi/extensions.d/` on your reMarkable tablet
    - **reMarkable 1 & 2**: `librarian-armv7.so`
    - **reMarkable Paper Pro and Paper Pro Move**: `librarian-aarch64.so`
3. Restart xovi

## Shell Usage

All commands use the xovi-message-broker pipe interface:

```bash
echo '>e<signal>:<params>' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Lookup

Resolve a document or folder name to its UUID. Returns all matches (newline-separated) if multiple entries share the same name.

```bash
echo '>elookupEntry:My Document' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>elookupEntry:My Folder/My Document' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Import

Import a PDF, EPUB, or `.rmdoc` file from the local filesystem. For PDF and EPUB, the file extension is stripped from the display name (e.g., `report.pdf` becomes `report`). For `.rmdoc` files, the archive is extracted directly — the embedded UUID, metadata, and page data are preserved as-is. Returns the document's UUID.

```bash
# Import to root
echo '>eimportDocument:/path/to/file.pdf' > /run/xovi-mb; cat /run/xovi-mb-out

# Import into a folder (by name or UUID)
echo '>eimportDocument:/path/to/file.pdf,My Folder' > /run/xovi-mb; cat /run/xovi-mb-out

# Import an rmdoc archive
echo '>eimportDocument:/path/to/backup.rmdoc' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>eimportDocument:/path/to/backup.rmdoc,My Folder' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Import Image

Import a PNG or JPEG image, converting it to a single-page PDF for display in xochitl. Returns the new document's UUID.

```bash
echo '>eimportImage:/path/to/screenshot.png' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>eimportImage:/path/to/photo.jpg,My Folder' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Rename

```bash
echo '>erenameEntry:Old Name,New Name' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Move

```bash
# Move to a folder
echo '>emoveEntry:My Document,Destination Folder' > /run/xovi-mb; cat /run/xovi-mb-out

# Move to root (empty parent)
echo '>emoveEntry:My Document,' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Trash and Restore

```bash
echo '>etrashEntry:My Document' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>erestoreEntry:My Document' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Delete (Permanent)

```bash
echo '>edeleteEntry:My Document' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Clone

```bash
# Clone into same location
echo '>ecloneEntry:My Document,' > /run/xovi-mb; cat /run/xovi-mb-out

# Clone into a different folder
echo '>ecloneEntry:My Document,Other Folder' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Create

```bash
# Create notebook
echo '>ecreateNotebook:New Notebook' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>ecreateNotebook:New Notebook,My Folder' > /run/xovi-mb; cat /run/xovi-mb-out

# Create folder
echo '>ecreateFolder:New Folder' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>ecreateFolder:Subfolder,Parent Folder' > /run/xovi-mb; cat /run/xovi-mb-out

# Ensure folder exists (creates if missing, returns UUID)
echo '>eensureFolder:My Folder' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>eensureFolder:Projects/2026/March' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Metadata

```bash
# Pin/unpin
echo '>esetPinned:My Document,true' > /run/xovi-mb; cat /run/xovi-mb-out

# Set cover page to last visited
echo '>esetCover:My Document,-1' > /run/xovi-mb; cat /run/xovi-mb-out

# Set cover page to first page
echo '>esetCover:My Document,0' > /run/xovi-mb; cat /run/xovi-mb-out

# Set orientation
echo '>esetOrientation:My Document,landscape' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>esetOrientation:My Document,portrait' > /run/xovi-mb; cat /run/xovi-mb-out

# Set tags
echo '>esetTags:My Document,tag1;tag2;tag3' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Rescan

Scan the xochitl data directory and try to load entries that exist on disk but are still missing from the running library. Returns the number of entries that were poked for runtime loading. This is mainly useful after direct filesystem changes.

```bash
echo '>erescanLibrary:' > /run/xovi-mb; cat /run/xovi-mb-out
```

### Content Pages

List the page UUIDs in a document that contain non-deleted content — strokes, typed text, images, or highlights. Blank pages, and pages whose strokes were fully erased, are excluded even though their `.rm` files still exist. Returns the matching page UUIDs (newline-separated) in page order, or an empty response if none.

```bash
echo '>egetContentPages:My Notebook' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>egetContentPages:<doc-uuid>' > /run/xovi-mb; cat /run/xovi-mb-out
```

## QML Usage

All signals are accessible from QML via the xovi-message-broker:

```qml
import net.asivery.XoviMessageBroker 2.0
```

```qml
XoviMessageBroker { id: librarian }
```

```qml
librarian.sendSimpleSignal("lookupEntry", "My Document")
librarian.sendSimpleSignal("importDocument", "/tmp/report.pdf,My Folder")
librarian.sendSimpleSignal("importDocument", "/tmp/backup.rmdoc")
librarian.sendSimpleSignal("importImage", "/tmp/screenshot.png,Screenshots")
librarian.sendSimpleSignal("renameEntry", "My Document,New Name")
librarian.sendSimpleSignal("moveEntry", "My Document,Destination Folder")
librarian.sendSimpleSignal("trashEntry", "My Document")
librarian.sendSimpleSignal("restoreEntry", "My Document")
librarian.sendSimpleSignal("deleteEntry", "My Document")
librarian.sendSimpleSignal("cloneEntry", "My Document,Other Folder")
librarian.sendSimpleSignal("createNotebook", "New Notebook,My Folder")
librarian.sendSimpleSignal("createFolder", "New Folder,Parent Folder")
librarian.sendSimpleSignal("ensureFolder", "Projects/2026/March")
librarian.sendSimpleSignal("rescanLibrary", "")
librarian.sendSimpleSignal("getContentPages", "My Notebook")
librarian.sendSimpleSignal("setPinned", "My Document,true")
librarian.sendSimpleSignal("setCover", "My Document,-1")
librarian.sendSimpleSignal("setOrientation", "My Document,landscape")
librarian.sendSimpleSignal("setTags", "My Document,tag1;tag2;tag3")
```

### Available Signals

| Signal | Parameter | Returns |
|--------|-----------|---------|
| `lookupEntry` | `name` or `path` | UUID(s), newline-separated |
| `importDocument` | `filepath` or `filepath,parent` | UUID |
| `importImage` | `filepath` or `filepath,parent` | UUID |
| `renameEntry` | `entry,newName` | `ok` |
| `moveEntry` | `entry,parent` | `ok` |
| `trashEntry` | `entry` | `ok` |
| `restoreEntry` | `entry` | `ok` |
| `deleteEntry` | `entry` | `ok` |
| `cloneEntry` | `entry,parent` | UUID |
| `createNotebook` | `name` or `name,parent` | UUID |
| `createFolder` | `name` or `name,parent` | UUID |
| `ensureFolder` | `name` or `path` | UUID |
| `rescanLibrary` | *(none)* | count of runtime-missing filesystem entries loaded |
| `getContentPages` | `entry` | page UUIDs with content, newline-separated |
| `setPinned` | `entry,true/false` | `ok` |
| `setCover` | `entry,pageNumber` | `ok` |
| `setOrientation` | `entry,portrait/landscape` | `ok` |
| `setTags` | `entry,tag1;tag2;tag3` | `ok` |

## Name Resolution

All commands accept document names, folder paths, or UUIDs:

| Form | Example | Behavior |
|------|---------|----------|
| UUID | `9155cd95-147f-4bb5-93f1-a1519dac1021` | Direct lookup |
| Name | `My Document` | Searches root first, then all folders. Errors if ambiguous. |
| Anchored name | `/My Document` | Searches root only, no fallback. |
| Path | `My Folder/My Document` | Walks the folder hierarchy from root |

`restoreEntry` searches specifically within trashed entries.

All commands return `ERROR: <message>` on failure (not found, ambiguous name, etc.).

### Parameter Separator

Commands that take two arguments use `,` as the separator (e.g., `entry,newName`), splitting on the last comma. The first argument (entry name) can contain commas freely. Name and path resolution with `/` is best-effort convenience as reMarkables doesn't prevent `/` from being used in document or folder names. If a folder name contains `,` or a name contains `/`, use UUIDs instead:

```bash
# Get UUIDs first
echo '>elookupEntry:My Document' > /run/xovi-mb; cat /run/xovi-mb-out
echo '>elookupEntry:Destination Folder' > /run/xovi-mb; cat /run/xovi-mb-out

# Then use UUIDs directly
echo '>emoveEntry:<doc-uuid>,<folder-uuid>' > /run/xovi-mb; cat /run/xovi-mb-out
```

## Building

This repository vendors [librm_lines](https://github.com/RedTTGMoss/librm_lines) as a submodule. Clone with submodules, or initialize them in an existing clone:

```bash
git clone --recursive https://github.com/rmitchellscott/rm-librarian
# or, in an existing clone:
git submodule update --init
```

```bash
./build.sh
```

Builds for both architectures using Docker, compiling librm_lines as a static library and linking it into the extension:
- `librarian-aarch64.so` - reMarkable Paper Pro
- `librarian-armv7.so` - reMarkable 2

## License

Copyright (C) 2026 Mitchell Scott

Licensed under the GNU General Public License v3.0.

This project statically links [librm_lines](https://github.com/RedTTGMoss/librm_lines) by RedTTG, also licensed under the GNU General Public License v3.0.
