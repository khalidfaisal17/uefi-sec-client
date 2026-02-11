# uefi-sec-client
feat(security): implement uefi_sec_client for Secure Boot provisioning

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

Signed-off-by: Khalid Faisal Ansari <afaisal@qti.qualcomm.com>
