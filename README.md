# uefi-sec-client
feature(security): implement uefi_sec_client for Secure Boot provisioning

Introduces `uefi_sec_client`, a command-line utility for the Qualcomm Linux (QLI) environment to manage UEFI Secure Boot keys via the uefisecapp Trusted Application.

Key features:
- **TA Communication**: Interfaces with `qcom.tz.uefisecapp` using QSEEComAPI.
- **Memory Management**: Implements `libdmabufheap` for shared memory allocation.
- **Key Operations**: Supports enrolling PK, KEK, db, and dbx using signed `.auth` files.
- **Status Checks**: Decodes and displays current Secure Boot state and key presence.
- **CLI Interface**: Mimics `QComSecBootTool` syntax (flags: -e, -r, -u, -d).

Integration:
- Added `uefi-sec-client.bb` Yocto recipe in `meta-qti-security-prop`.
- Added manual `Makefile` for standalone/ADB compilation.
- Configured dependencies for `QseeComApi` and `dmabufheap`.

Sync & Build Instructions:


# QLI UEFI Secure Boot Client Usage Guide

## Overview
The `uefi_sec_client` is a command-line tool designed for Qualcomm Linux IoT (QLI) platforms to manage UEFI Secure Boot keys (PK, KEK, db, dbx). It interacts with the TrustZone application (`qcom.tz.uefisecapp`) to safely read and write secure variables.

## Supported Variables
*   **PK (Platform Key)**: Root of trust. Controls access to KEK.
*   **KEK (Key Exchange Key)**: Controls access to db/dbx.
*   **db (Signature Database)**: Allowed signatures for boot images.
*   **dbx (Forbidden Signature Database)**: Revoked signatures.

## Compilation
Ensure you are in the QLI build environment with access to `QSEEComAPI.h`.

1.  Place the `.c` file in your source directory.
2.  Compile using `make` or manually:
    ```bash
    $CC uefi_sec_client.c -o uefi_sec_client -lQSEEComAPI -lpthread
    ```
### OR
1. Sync code in `QLI1.x` workspace `<workspace-path>/layers/meta-qti-security-prop/recipes-security/`
2. `cd <workspace-path>` 
3. `source setup-environment` (Choose any target)
4. `bitbake uefi-sec-client`

## Command Reference

| Flag | Long Option | Description |
| :--- | :--- | :--- |
| `-c` | `--checkStatus` | Displays the key presence. |
| `-e` | `--enroll` | Enrolls or Updates a key. Replaces existing data. |
| `-v` | `--var` | Specifies the variable name (`PK`, `KEK`, `db`, `dbx`). |
| `-f` | `--file` | Path to the signed authentication file (`.auth`) or ESL file (`.esl`). |

## Usage Examples

### 1. Check Security Status
Use this command to see which keys are provisioned.
```bash
uefi_sec_client -c
```
**Output Example:**
```text
[ PK] Present (Size: 850)
[KEK] Present (Size: 850)
[ db] Present (Size: 1500)
[dbx] Not Provisioned
```

### 2. Enroll/Update a Key
Used to provision keys initially or rotate them. 
*Requires a properly signed `.auth` file containing the new key and authentication descriptor.*

**Example: Enroll db**
```bash
uefi_sec_client -e -v db -f /data/db.auth
```

**Example: Enroll KEK**
```bash
uefi_sec_client -e -v KEK -f /data/KEK.auth
```

## Constraints & Error Handling
*   **PK Operations**: 
    *   **Enroll (`-e`)** is **NOT ALLOWED** for PK.
    *   **Update (`-e`)** is allowed (for Rotation or Clearing).
*   **Authentication**: All write operations in User Mode require signed `.auth` files.
*   **Error 0x1A**: Indicates "Security Violation". This happens if you try to write without a valid signature while Secure Boot is enabled.

Signed-off-by: Khalid Faisal Ansari <afaisal@qti.qualcomm.com>
