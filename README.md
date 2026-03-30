# Librarian

[![rm1](https://img.shields.io/badge/rM1-supported-green)](https://remarkable.com/store/remarkable)
[![rm2](https://img.shields.io/badge/rM2-supported-green)](https://remarkable.com/store/remarkable-2)
[![rmpp](https://img.shields.io/badge/rMPP-supported-green)](https://remarkable.com/store/overview/remarkable-paper-pro)
[![rmppm](https://img.shields.io/badge/rMPPM-supported-green)](https://remarkable.com/products/remarkable-paper/pro-move)
<img src="assets/rm-librarian.svg" alt="rm-librarian Icon" width="125" align="right">
<p align="justify">

A xovi extension that provides a programmatic interface to xochitl's document library without the need to restart xochitl.

## Dependencies

- [xovi](https://github.com/asivery/rm-xovi-extensions) - Extension framework
    - xovi-message-broker - Required for shell and QML communication

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

Import a file from the local filesystem. The file extension is stripped from the display name (e.g., `report.pdf` becomes `report`). Returns the new document's UUID.

```bash
# Import to root
echo '>eimportDocument:/path/to/file.pdf' > /run/xovi-mb; cat /run/xovi-mb-out

# Import into a folder (by name or UUID)
echo '>eimportDocument:/path/to/file.pdf,My Folder' > /run/xovi-mb; cat /run/xovi-mb-out
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
librarian.sendSimpleSignal("renameEntry", "My Document,New Name")
librarian.sendSimpleSignal("moveEntry", "My Document,Destination Folder")
librarian.sendSimpleSignal("trashEntry", "My Document")
librarian.sendSimpleSignal("restoreEntry", "My Document")
librarian.sendSimpleSignal("deleteEntry", "My Document")
librarian.sendSimpleSignal("cloneEntry", "My Document,Other Folder")
librarian.sendSimpleSignal("createNotebook", "New Notebook,My Folder")
librarian.sendSimpleSignal("createFolder", "New Folder,Parent Folder")
librarian.sendSimpleSignal("ensureFolder", "Projects/2026/March")
librarian.sendSimpleSignal("setPinned", "My Document,true")
librarian.sendSimpleSignal("setCover", "My Document,3")
librarian.sendSimpleSignal("setOrientation", "My Document,landscape")
librarian.sendSimpleSignal("setTags", "My Document,tag1;tag2;tag3")
```

### Available Signals

| Signal | Parameter | Returns |
|--------|-----------|---------|
| `lookupEntry` | `name` or `path` | UUID(s), newline-separated |
| `importDocument` | `filepath` or `filepath,parent` | UUID |
| `renameEntry` | `entry,newName` | `ok` |
| `moveEntry` | `entry,parent` | `ok` |
| `trashEntry` | `entry` | `ok` |
| `restoreEntry` | `entry` | `ok` |
| `deleteEntry` | `entry` | `ok` |
| `cloneEntry` | `entry,parent` | `ok` |
| `createNotebook` | `name` or `name,parent` | `ok` |
| `createFolder` | `name` or `name,parent` | `ok` |
| `ensureFolder` | `name` or `path` | UUID |
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

```bash
./build.sh
```

Builds for both architectures using Docker:
- `librarian-aarch64.so` - reMarkable Paper Pro
- `librarian-armv7.so` - reMarkable 2

## License

GPLv3
